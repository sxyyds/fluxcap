# Invokes the Desktop Duplication resilience runtime-L2 evidence workload
# for all four traces and preserves executable/runtime/stdout artifacts with
# SHA-256 manifests under qualification-results.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BenchPath,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [string]$AdapterLabel = 'nvidia',
    [int]$DurationMs = 5000,
    [int]$WarmupMs = 1000
)

$ErrorActionPreference = 'Stop'

$traces = @(
    @{ Name = 'dd-identity';     Dir = 'dd-identity-320' },
    @{ Name = 'dd-baked-cursor'; Dir = 'dd-baked-cursor-320' },
    @{ Name = 'dd-controller';   Dir = 'dd-controller-virtual' },
    @{ Name = 'gdi';             Dir = 'gdi-320' }
)

if (-not (Test-Path $BenchPath)) {
    throw "bench not found: $BenchPath"
}
$resolvedBench = (Resolve-Path $BenchPath).Path
$benchHash = (Get-FileHash -Algorithm SHA256 $resolvedBench).Hash
$date = Get-Date -Format 'yyyyMMdd'

$summary = @()
foreach ($trace in $traces) {
    $directory = Join-Path $OutputRoot `
        ("l2-{0}-{1}-{2}" -f $AdapterLabel, $trace.Dir, $date)
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $nonce = -join ((1..32) | ForEach-Object {
        '{0:x}' -f (Get-Random -Maximum 16)
    })
    $evidencePath = Join-Path $directory 'runtime-l2-evidence.json'
    $stdoutPath = Join-Path $directory 'workload.stdout.txt'

    & $resolvedBench `
        --trace $trace.Name `
        --duration-ms $DurationMs `
        --warmup-ms $WarmupMs `
        --runtime-evidence-json "$evidencePath" `
        --qualification-run-nonce $nonce `
        1> $stdoutPath
    $exit = $LASTEXITCODE

    if (-not (Test-Path $evidencePath)) {
        throw "trace $($trace.Name) produced no evidence file"
    }
    $evidence = Get-Content $evidencePath -Raw | ConvertFrom-Json
    $adapterLuid = $evidence.adapterLuid
    $passed = [bool]$evidence.passed

    $manifest = [ordered]@{
        schemaVersion = 1
        capturedUtc = (Get-Date).ToUniversalTime().ToString(
            "yyyy-MM-ddTHH:mm:ssZ")
        status = 'runtime-l2-only'
        evidenceLevel = 2
        qualificationTuple = [ordered]@{
            adapterLuid = $adapterLuid
            captureBackend = $evidence.qualificationTuple.captureBackend
            captureTarget = $evidence.qualificationTuple.captureTarget
            format = $evidence.qualificationTuple.format
            colorSpace = $evidence.qualificationTuple.colorSpace
            output = ("{0}x{1}" -f $evidence.qualificationTuple.outputWidth,
                $evidence.qualificationTuple.outputHeight)
            idleRepublishIntervalMs =
                $evidence.qualificationTuple.idleRepublishIntervalMs
        }
        artifacts = [ordered]@{
            executable = [ordered]@{
                path = [IO.Path]::GetFileName($resolvedBench)
                bytes = (Get-Item $resolvedBench).Length
                sha256 = $benchHash
            }
            runtimeEvidence = [ordered]@{
                path = 'runtime-l2-evidence.json'
                bytes = (Get-Item $evidencePath).Length
                sha256 = (Get-FileHash -Algorithm SHA256 $evidencePath).Hash
            }
            stdout = [ordered]@{
                path = 'workload.stdout.txt'
                bytes = (Get-Item $stdoutPath).Length
                sha256 = (Get-FileHash -Algorithm SHA256 $stdoutPath).Hash
            }
        }
        l3 = [ordered]@{
            status = 'not-run'
            etl = $null
            genericEventsCsv = $null
            manualWpaReview = $false
        }
        observationBoundary = [ordered]@{
            mftInternalCopyObservable = $false
            driverPrivateSurfaceObservable = $false
            hardwareDmaAndCacheObservable = $false
        }
        workloadExitCode = $exit
        gatesPassed = $passed
    }
    $manifestPath = Join-Path $directory 'manifest.json'
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -Path $manifestPath `
        -Encoding UTF8

    $readme = @"
# Runtime L2 evidence — $AdapterLabel / $($trace.Dir)

Preserved from a real ``$($trace.Name)`` trace of
``fluxcap_gpu_dd_resilience_bench`` on $date (exit code $exit, gates
``$(if ($passed) { 'PASS' } else { 'FAIL' })``).

This is runtime L2 bus-publish copy evidence only: no encoder is involved,
no WPR ETL was captured, and no L3 claim is made. Driver-private surfaces,
hardware DMA, and caches remain unobservable. Idle heartbeat and session
rebuild counters are observations, not forced gates, because system desktop
activity is not controlled by the workload.

``manifest.json`` binds the preserved executable, runtime JSON, and stdout
by SHA-256. Requalifying another adapter, format, ROI, or option set
requires a new artifact set.
"@
    Set-Content -Path (Join-Path $directory 'README.md') -Value $readme `
        -Encoding UTF8

    $summary += [pscustomobject]@{
        trace = $trace.Name
        directory = $directory
        exit = $exit
        passed = $passed
        published = $evidence.busPublishedFrames
        copies = $evidence.producerIngressCopySubmissions
        monotonicViolations =
            $evidence.measuredMonotonicViolations
    }
}

$summary | Format-Table -AutoSize
$failed = @($summary | Where-Object { -not $_.passed })
if ($failed.Count -ne 0) {
    throw ("{0} trace(s) failed gates" -f $failed.Count)
}
