[CmdletBinding()]
param([switch] $KeepArtifacts)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceAnalyzer = Join-Path $scriptRoot "Test-FluxCapL3Trace.ps1"
$sourceWpr = Join-Path $scriptRoot "fluxcap-copy-qualification.wprp"
$sourceWpa = Join-Path $scriptRoot "fluxcap-copy-events.wpaProfile"
$powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
$root = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("FluxCapQualificationSelfTest-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $root | Out-Null

function Write-Utf8Json([object] $Value, [string] $Path) {
    $Value | ConvertTo-Json -Depth 10 | Set-Content `
        -LiteralPath $Path -Encoding UTF8
}

function Hash([string] $Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function TextHash([string] $Value) {
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Value)
        return (($algorithm.ComputeHash($bytes) | ForEach-Object {
            $_.ToString("x2")
        }) -join "")
    } finally {
        $algorithm.Dispose()
    }
}

function Invoke-Analyzer([string] $Output) {
    & $powershell `
        -NoProfile `
        -ExecutionPolicy Bypass `
        -File $analyzer `
        -Manifest $manifestPath `
        -GenericEventsCsv $eventsPath `
        -RuntimeEvidenceJson $runtimePath `
        -Output $Output `
        -ConfirmReviewedWorkloadInterval `
        -ConfirmNoFullFrameD3D11Copies `
        -ConfirmNoCopyEnginePackets `
        -ConfirmMediaFoundationActivity `
        -ConfirmNoLostEvents `
        -Reviewer "qualification-self-test" | Out-Null
    return $LASTEXITCODE
}

function Update-RuntimeTupleHash {
    $tuple = $runtime.qualificationTuple
    $canonical = @(
        "v2", [string]$tuple.adapterLuid,
        [string]$tuple.captureBackend, [string]$tuple.captureTarget,
        [string]$tuple.rotation, [string]$tuple.outputIdentity,
        [string]$tuple.transformBackend, [string]$tuple.format,
        [string]$tuple.colorSpace,
        ("{0}x{1}" -f $tuple.captureSurfaceWidth,
            $tuple.captureSurfaceHeight),
        ("{0},{1},{2},{3}" -f $tuple.sourceX, $tuple.sourceY,
            $tuple.sourceWidth, $tuple.sourceHeight),
        ("{0}x{1}" -f $tuple.outputWidth, $tuple.outputHeight),
        [string]$tuple.codec, [string]$tuple.encoderInputSubtype,
        [string]$tuple.encoderOutputSubtype, [string]$tuple.encoderMftClsid
    ) -join "|"
    $runtime.tupleSha256 = TextHash $canonical
}

function Sync-RuntimeBinding {
    Write-Utf8Json $runtime $runtimePath
    $manifest.runtimeEvidence.sha256 = Hash $runtimePath
    Write-Utf8Json $manifest $manifestPath
}

function Sync-ExportBinding {
    Write-Utf8Json $exportManifest $exportManifestPath
    $manifest.exportManifest.sha256 = Hash $exportManifestPath
    Write-Utf8Json $manifest $manifestPath
}

function Write-EventsAndBind([object[]] $Rows) {
    $Rows | Export-Csv `
        -LiteralPath $eventsPath -NoTypeInformation -Encoding UTF8
    $exportManifest.genericEventsCsvSha256 = Hash $eventsPath
    Sync-ExportBinding
}

try {
    $analyzer = Join-Path $root "trace-analyzer.ps1"
    $wpr = Join-Path $root "capture-profile.wprp"
    $wpa = Join-Path $root "export-profile.wpaProfile"
    Copy-Item -LiteralPath $sourceAnalyzer -Destination $analyzer
    Copy-Item -LiteralPath $sourceWpr -Destination $wpr
    Copy-Item -LiteralPath $sourceWpa -Destination $wpa

    $executable = Join-Path $root "fixture.exe"
    $etl = Join-Path $root "fixture.etl"
    $stdout = Join-Path $root "stdout.txt"
    $stderr = Join-Path $root "stderr.txt"
    $exporter = Join-Path $root "wpaexporter.exe"
    Set-Content -LiteralPath $executable -Value "fixture executable" -Encoding UTF8
    Set-Content -LiteralPath $etl -Value "fixture etl" -Encoding UTF8
    Set-Content -LiteralPath $stdout -Value "fixture stdout" -Encoding UTF8
    Set-Content -LiteralPath $stderr -Value "" -Encoding UTF8
    Set-Content -LiteralPath $exporter -Value "fixture exporter" -Encoding UTF8

    $pidValue = 424242
    $qualificationRunNonce = "0123456789abcdef0123456789abcdef"
    $runtimePath = Join-Path $root "runtime-l2-evidence.json"
    $adapterLuid = "0x00000000:0x00000001"
    $mftClsid = "{00000000-0000-0000-0000-000000000001}"
    $tupleCanonical = @(
        "v2", $adapterLuid, "wgc", "window", "not_applicable",
        "not_applicable",
        "deterministic_planar", "NV12", "YCBCR_STUDIO_G22_LEFT_P709",
        "1280x720", "320,40,640,640", "320x320", "h264",
        "MFVideoFormat_NV12", "MFVideoFormat_H264", $mftClsid
    ) -join "|"
    $runtime = [ordered]@{
        schemaVersion = 2
        evidenceLevel = 2
        evidenceName = "l2_external_texture_identity_and_encoder_bind"
        processId = $pidValue
        executableSha256 = Hash $executable
        qualificationRunNonce = $qualificationRunNonce
        adapterLuid = $adapterLuid
        countersScope = "capture_encoder_instance_lifetime_including_warmup"
        tupleSha256 = TextHash $tupleCanonical
        qualificationTuple = [ordered]@{
            adapterLuid = $adapterLuid
            captureBackend = "wgc"
            captureTarget = "window"
            rotation = "not_applicable"
            outputIdentity = "not_applicable"
            transformBackend = "deterministic_planar"
            format = "NV12"
            colorSpace = "YCBCR_STUDIO_G22_LEFT_P709"
            hdr10StaticMetadata = $false
            captureSurfaceWidth = 1280
            captureSurfaceHeight = 720
            logicalSourceWidth = 1280
            logicalSourceHeight = 720
            sourceX = 320
            sourceY = 40
            sourceWidth = 640
            sourceHeight = 640
            outputWidth = 320
            outputHeight = 320
            codec = "h264"
            encoderInputSubtype = "MFVideoFormat_NV12"
            encoderOutputSubtype = "MFVideoFormat_H264"
            encoderMft = "Fixture MFT $mftClsid"
            encoderMftClsid = $mftClsid
            encoderMftFriendlyName = "Fixture MFT"
        }
        encoderMatchingTransformCount = 1
        externalSubmissions = 7
        externalIdentityVerifiedSubmissions = 7
        externalVideoEncoderBoundSubmissions = 7
        encoderCopiedSubmissions = 0
        encoderDirectSubmissions = 7
        producerIngressCopySubmissions = 0
        producerIngressTransformSubmissions = 7
        busPublishedFrames = 7
        busCopiedPublishes = 0
        busDirectPublishes = 7
        desktopPresentFrames = 0
        measuredSourcePresentations = 5
        measuredDesktopPresentFrames = 0
        packetCodecMismatches = 0
        fluxcapExplicitCopyFree = $true
        measuredConsumerFrames = 5
        measuredDurationMs = 1000
        warmupMs = 100
        mftInternalCopyObservable = $false
        driverPrivateSurfaceCopyObservable = $false
        hardwareDmaCopyObservable = $false
    }
    Write-Utf8Json $runtime $runtimePath

    $eventsPath = Join-Path $root "events.csv"
    $eventRows = @(
        [pscustomobject]@{
            'Provider Name' = 'Microsoft-Windows-Direct3D11'
            Process = "fixture.exe ($pidValue)"
            'Event Name' = 'Present'; Message = 'D3D11 activity'
        },
        [pscustomobject]@{
            'Provider Name' = 'Microsoft-Windows-DXGI'
            Process = "fixture.exe ($pidValue)"
            'Event Name' = 'Present'; Message = 'DXGI activity'
        },
        [pscustomobject]@{
            'Provider Name' = 'Microsoft-Windows-DxgKrnl'
            Process = 'System (4)'
            'Event Name' = 'QueuePacket'; Message = 'graphics queue activity'
        },
        [pscustomobject]@{
            'Provider Name' = 'Microsoft-Windows-MediaFoundation-Performance'
            Process = "fixture.exe ($pidValue)"
            'Event Name' = 'ProcessInput'; Message = 'encoder activity'
        }
    )
    $eventRows | Export-Csv `
        -LiteralPath $eventsPath -NoTypeInformation -Encoding UTF8

    $exportManifestPath = Join-Path $root "wpa-export-manifest.json"
    $exportManifest = [ordered]@{
        schemaVersion = 1
        etl = $etl
        etlSha256 = Hash $etl
        wpaProfile = $wpa
        wpaProfileSha256 = Hash $wpa
        exporter = $exporter
        exporterSha256 = Hash $exporter
        exporterFileVersion = "fixture"
        genericEventsCsv = $eventsPath
        genericEventsCsvSha256 = Hash $eventsPath
    }
    Write-Utf8Json $exportManifest $exportManifestPath

    $manifestPath = Join-Path $root "capture-manifest.json"
    $expectedWorkload = [ordered]@{
        adapterLuid = $adapterLuid
        captureBackend = "wgc"
        captureTarget = "window"
        outputIndex = $null
        rotation = "not_applicable"
        transformBackend = "deterministic_planar"
        format = "NV12"
        codec = "h264"
        sourceWidth = 640
        sourceHeight = 640
        outputWidth = 320
        outputHeight = 320
    }
    $manifest = [ordered]@{
        schemaVersion = 3
        qualification = "FluxCap L3 ETW copy observation"
        status = "trace-captured"
        evidenceLevel = 0
        traceScope = "spawned-process-lifetime-including-warmup"
        executable = $executable
        executableSha256 = Hash $executable
        executableSha256After = Hash $executable
        executableHashStable = $true
        processId = $pidValue
        qualificationRunNonce = $qualificationRunNonce
        expectedWorkload = $expectedWorkload
        expectedWorkloadSha256 = TextHash (
            $expectedWorkload | ConvertTo-Json -Compress)
        exitCode = 0
        traceStartUtc = '2026-01-01T00:00:00.0000000Z'
        traceEndUtc = '2026-01-01T00:00:01.0000000Z'
        etl = $etl
        etlSha256 = Hash $etl
        stdout = $stdout
        stdoutSha256 = Hash $stdout
        stderr = $stderr
        stderrSha256 = Hash $stderr
        profiles = [ordered]@{
            inputsStable = $true
            wpr = $wpr; wprSha256 = Hash $wpr
            wpa = $wpa; wpaSha256 = Hash $wpa
            analyzer = $analyzer; analyzerSha256 = Hash $analyzer
        }
        runtimeEvidence = [ordered]@{
            path = $runtimePath; sha256 = Hash $runtimePath
        }
        runtimeEvidenceRejection = $null
        exportManifest = [ordered]@{
            path = $exportManifestPath; sha256 = Hash $exportManifestPath
        }
        runtimeBoundary = [ordered]@{
            mftInternalCopyObservable = $false
            driverPrivateSurfaceObservable = $false
            hardwareDmaAndCacheObservable = $false
        }
    }
    Write-Utf8Json $manifest $manifestPath

    $passedPath = Join-Path $root "passed.json"
    if ((Invoke-Analyzer $passedPath) -ne 0) {
        throw "valid synthetic qualification did not pass"
    }
    $passed = Get-Content -LiteralPath $passedPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    if ($passed.status -ne 'passed' -or [int]$passed.evidenceLevel -ne 3) {
        throw "valid synthetic qualification returned the wrong result"
    }

    $manifest.processId = [string]$pidValue
    Write-Utf8Json $manifest $manifestPath
    $manifestPidPath = Join-Path $root "manifest-pid-type.json"
    if ((Invoke-Analyzer $manifestPidPath) -ne 1) {
        throw "string capture-manifest PID did not fail screening"
    }
    $manifestPidResult = Get-Content `
        -LiteralPath $manifestPidPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifestPidResult.manifestIntegrity.processIdValid `
        -or $manifestPidResult.runtimeL2.complete) {
        throw "string capture-manifest PID survived a native-type gate"
    }
    $manifest.processId = $pidValue
    Write-Utf8Json $manifest $manifestPath

    $manifest.exitCode = "0"
    Write-Utf8Json $manifest $manifestPath
    if ((Invoke-Analyzer (Join-Path $root "manifest-exit-type.json")) -ne 1) {
        throw "string capture-manifest exit code did not fail screening"
    }
    $manifest.exitCode = 0
    Write-Utf8Json $manifest $manifestPath

    $manifest.qualificationRunNonce = 17
    Write-Utf8Json $manifest $manifestPath
    if ((Invoke-Analyzer (Join-Path $root "manifest-nonce-type.json")) -ne 1) {
        throw "non-string capture-manifest nonce did not fail screening"
    }
    $manifest.qualificationRunNonce = $qualificationRunNonce
    Write-Utf8Json $manifest $manifestPath

    $runtime.qualificationRunNonce = 17
    Sync-RuntimeBinding
    if ((Invoke-Analyzer (Join-Path $root "runtime-nonce-type.json")) -ne 3) {
        throw "non-string runtime nonce did not fail L2"
    }
    $runtime.qualificationRunNonce = $qualificationRunNonce
    Sync-RuntimeBinding

    $expectedWorkload.sourceWidth = "640"
    $manifest.expectedWorkloadSha256 = TextHash (
        $expectedWorkload | ConvertTo-Json -Compress)
    Write-Utf8Json $manifest $manifestPath
    if ((Invoke-Analyzer (Join-Path $root "expected-type-tamper.json")) -ne 1) {
        throw "string expected-workload geometry did not fail schema-v3 screening"
    }
    $expectedWorkload.sourceWidth = 640
    $manifest.expectedWorkloadSha256 = TextHash (
        $expectedWorkload | ConvertTo-Json -Compress)
    Write-Utf8Json $manifest $manifestPath

    $savedEtlSha256 = $manifest.etlSha256
    $manifest.etlSha256 = 17
    Write-Utf8Json $manifest $manifestPath
    if ((Invoke-Analyzer (Join-Path $root "manifest-hash-type.json")) -ne 1) {
        throw "non-string manifest SHA-256 did not fail screening"
    }
    $manifest.etlSha256 = $savedEtlSha256
    Write-Utf8Json $manifest $manifestPath

    $savedProvider = $eventRows[0].'Provider Name'
    $savedMessage = $eventRows[0].Message
    $eventRows[0].'Provider Name' = 'Fixture-Other-Provider'
    $eventRows[0].Message =
        "Microsoft-Windows-Direct3D11 fixture.exe ($pidValue)"
    Write-EventsAndBind $eventRows
    if ((Invoke-Analyzer (Join-Path $root "provider-column-spoof.json")) -ne 1) {
        throw "provider text outside the dedicated provider column was accepted"
    }
    $eventRows[0].'Provider Name' = $savedProvider
    $eventRows[0].Message = $savedMessage
    Write-EventsAndBind $eventRows

    $savedProcess = $eventRows[0].Process
    $eventRows[0].Process = 'fixture.exe'
    $eventRows[0].Message = "D3D11 activity fixture.exe ($pidValue)"
    Write-EventsAndBind $eventRows
    if ((Invoke-Analyzer (Join-Path $root "pid-column-spoof.json")) -ne 1) {
        throw "PID text outside a dedicated process column was accepted"
    }
    $eventRows[0].Process = $savedProcess
    $eventRows[0].Message = $savedMessage
    Write-EventsAndBind $eventRows

    $runtime.qualificationTuple.outputIdentity = "0:spoof"
    Update-RuntimeTupleHash
    Sync-RuntimeBinding
    if ((Invoke-Analyzer (Join-Path $root "wgc-output-identity.json")) -ne 3) {
        throw "WGC accepted a Desktop Duplication output identity"
    }
    $runtime.qualificationTuple.outputIdentity = "not_applicable"
    Update-RuntimeTupleHash
    Sync-RuntimeBinding

    $runtime.tupleSha256 = 17
    Sync-RuntimeBinding
    if ((Invoke-Analyzer (Join-Path $root "tuple-hash-type.json")) -ne 3) {
        throw "non-string tuple SHA-256 did not fail L2"
    }
    Update-RuntimeTupleHash
    Sync-RuntimeBinding

    $wgcExpectedWorkload = $manifest.expectedWorkload |
        ConvertTo-Json -Depth 10 | ConvertFrom-Json
    $wgcTuple = $runtime.qualificationTuple |
        ConvertTo-Json -Depth 10 | ConvertFrom-Json
    $ddExpectedWorkload = [ordered]@{
        adapterLuid = $adapterLuid
        captureBackend = "desktop_duplication"
        captureTarget = "monitor"
        outputIndex = 2
        rotation = "identity"
        transformBackend = "deterministic_planar"
        format = "NV12"
        codec = "h264"
        sourceWidth = 640
        sourceHeight = 640
        outputWidth = 320
        outputHeight = 320
    }
    $manifest.expectedWorkload = $ddExpectedWorkload
    $manifest.expectedWorkloadSha256 = TextHash (
        $ddExpectedWorkload | ConvertTo-Json -Compress)
    $runtime.qualificationTuple.captureBackend = "desktop_duplication"
    $runtime.qualificationTuple.captureTarget = "monitor"
    $runtime.qualificationTuple.rotation = "identity"
    $runtime.qualificationTuple.outputIdentity =
        '2:\\.\DISPLAY2:-1920,0,0,1080:1920x1080'
    $runtime.qualificationTuple.captureSurfaceWidth = 1920
    $runtime.qualificationTuple.captureSurfaceHeight = 1080
    $runtime.qualificationTuple.logicalSourceWidth = 1920
    $runtime.qualificationTuple.logicalSourceHeight = 1080
    $runtime.qualificationTuple.sourceX = 640
    $runtime.qualificationTuple.sourceY = 220
    $runtime.desktopPresentFrames = 7
    $runtime.measuredSourcePresentations = 5
    $runtime.measuredDesktopPresentFrames = 5
    Update-RuntimeTupleHash
    Sync-RuntimeBinding
    if ((Invoke-Analyzer (Join-Path $root "desktop-duplication-valid.json")) -ne 0) {
        throw "valid schema-v3 Desktop Duplication output identity did not pass"
    }

    $runtime.desktopPresentFrames = 8
    Sync-RuntimeBinding
    if ((Invoke-Analyzer (Join-Path $root "dd-present-range.json")) -ne 3) {
        throw "Desktop Duplication lifetime presents exceeding bus publishes passed"
    }
    $runtime.desktopPresentFrames = 7
    Sync-RuntimeBinding

    foreach ($rotationFixture in @(
            [pscustomobject]@{
                Name = "rotate90"; SurfaceWidth = 1080; SurfaceHeight = 1920
            },
            [pscustomobject]@{
                Name = "rotate180"; SurfaceWidth = 1920; SurfaceHeight = 1080
            },
            [pscustomobject]@{
                Name = "rotate270"; SurfaceWidth = 1080; SurfaceHeight = 1920
            })) {
        $ddExpectedWorkload.rotation = $rotationFixture.Name
        $ddExpectedWorkload.transformBackend = "video_processor"
        $manifest.expectedWorkloadSha256 = TextHash (
            $ddExpectedWorkload | ConvertTo-Json -Compress)
        $runtime.qualificationTuple.rotation = $rotationFixture.Name
        $runtime.qualificationTuple.transformBackend = "video_processor"
        $runtime.qualificationTuple.captureSurfaceWidth =
            $rotationFixture.SurfaceWidth
        $runtime.qualificationTuple.captureSurfaceHeight =
            $rotationFixture.SurfaceHeight
        Update-RuntimeTupleHash
        Sync-RuntimeBinding
        if ((Invoke-Analyzer (Join-Path $root (
                    "desktop-duplication-$($rotationFixture.Name).json"))) -ne 0) {
            throw "valid $($rotationFixture.Name) Desktop Duplication tuple did not pass"
        }
    }

    $runtime.qualificationTuple.captureSurfaceWidth = 1920
    $runtime.qualificationTuple.captureSurfaceHeight = 1080
    Update-RuntimeTupleHash
    Sync-RuntimeBinding
    if ((Invoke-Analyzer (Join-Path $root "dd-rotate270-geometry.json")) -ne 3) {
        throw "rotated Desktop Duplication with unrotated surface geometry passed"
    }

    $ddExpectedWorkload.rotation = "identity"
    $ddExpectedWorkload.transformBackend = "deterministic_planar"
    $manifest.expectedWorkloadSha256 = TextHash (
        $ddExpectedWorkload | ConvertTo-Json -Compress)
    $runtime.qualificationTuple.rotation = "identity"
    $runtime.qualificationTuple.transformBackend = "deterministic_planar"
    $runtime.qualificationTuple.captureSurfaceWidth = 1920
    $runtime.qualificationTuple.captureSurfaceHeight = 1080
    Update-RuntimeTupleHash
    Sync-RuntimeBinding

    $ddExpectedWorkload.outputIndex = "2"
    $manifest.expectedWorkloadSha256 = TextHash (
        $ddExpectedWorkload | ConvertTo-Json -Compress)
    Write-Utf8Json $manifest $manifestPath
    if ((Invoke-Analyzer (Join-Path $root "output-index-type.json")) -ne 1) {
        throw "string Desktop Duplication outputIndex did not fail screening"
    }
    $ddExpectedWorkload.outputIndex = 2
    $manifest.expectedWorkloadSha256 = TextHash (
        $ddExpectedWorkload | ConvertTo-Json -Compress)
    Write-Utf8Json $manifest $manifestPath

    $runtime.qualificationTuple.outputIdentity =
        '2:\\.\DISPLAY2:malformed'
    Update-RuntimeTupleHash
    Sync-RuntimeBinding
    if ((Invoke-Analyzer (Join-Path $root "output-identity-malformed.json")) -ne 3) {
        throw "malformed Desktop Duplication output identity did not fail L2"
    }

    $manifest.expectedWorkload = $wgcExpectedWorkload
    $manifest.expectedWorkloadSha256 = TextHash (
        $wgcExpectedWorkload | ConvertTo-Json -Compress)
    $runtime.qualificationTuple = $wgcTuple
    $runtime.desktopPresentFrames = 0
    $runtime.measuredSourcePresentations = 5
    $runtime.measuredDesktopPresentFrames = 0
    Update-RuntimeTupleHash
    Sync-RuntimeBinding

    $runtime.desktopPresentFrames = "0"
    Sync-RuntimeBinding
    if ((Invoke-Analyzer (Join-Path $root "present-counter-type.json")) -ne 3) {
        throw "string presentation counter did not fail native-type validation"
    }
    $runtime.desktopPresentFrames = 0
    Sync-RuntimeBinding

    $runtime.measuredSourcePresentations = 0
    Sync-RuntimeBinding
    if ((Invoke-Analyzer (Join-Path $root "wgc-source-presents.json")) -ne 3) {
        throw "WGC with no measured source presentations passed L2"
    }
    $runtime.measuredSourcePresentations = 5
    Sync-RuntimeBinding

    $runtime.evidenceLevel = 0
    $runtime.evidenceName = "none"
    $manifest.exitCode = 1
    Sync-RuntimeBinding
    $unsupportedPath = Join-Path $root "unsupported-runtime.json"
    if ((Invoke-Analyzer $unsupportedPath) -ne 1) {
        throw "unsupported workload output did not fail screening"
    }
    $unsupported = Get-Content `
        -LiteralPath $unsupportedPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([int]$unsupported.evidenceLevel -ne 0 `
        -or $unsupported.evidenceName -ne "none" `
        -or $unsupported.runtimeL2.complete) {
        throw "unsupported workload output was mislabeled as evidence"
    }
    $runtime.evidenceLevel = 2
    $runtime.evidenceName = "l2_external_texture_identity_and_encoder_bind"
    $manifest.exitCode = 0
    Sync-RuntimeBinding

    $runtime.externalSubmissions = [double]7.5
    Sync-RuntimeBinding
    if ((Invoke-Analyzer (Join-Path $root "runtime-float-count.json")) -ne 3) {
        throw "floating-point runtime counter did not fail native-type validation"
    }
    $runtime.externalSubmissions = 7
    Sync-RuntimeBinding

    [void]$runtime.Remove("desktopPresentFrames")
    Sync-RuntimeBinding
    if ((Invoke-Analyzer (Join-Path $root "legacy-v2-missing-present.json")) -ne 3) {
        throw "legacy runtime v2 without presentation counters entered L3"
    }
    $runtime.desktopPresentFrames = 0
    Sync-RuntimeBinding

    $runtime.encoderCopiedSubmissions = 1
    Write-Utf8Json $runtime $runtimePath
    $manifest.runtimeEvidence.sha256 = Hash $runtimePath
    Write-Utf8Json $manifest $manifestPath
    if ((Invoke-Analyzer (Join-Path $root "runtime-tamper.json")) -ne 3) {
        throw "hash-valid runtime semantic tamper did not fail the L2 gate"
    }

    $runtime.encoderCopiedSubmissions = 0
    Write-Utf8Json $runtime $runtimePath
    $manifest.runtimeEvidence.sha256 = Hash $runtimePath
    Write-Utf8Json $manifest $manifestPath

    [void]$runtime.Remove("encoderCopiedSubmissions")
    Write-Utf8Json $runtime $runtimePath
    $manifest.runtimeEvidence.sha256 = Hash $runtimePath
    Write-Utf8Json $manifest $manifestPath
    if ((Invoke-Analyzer (Join-Path $root "runtime-missing-field.json")) -ne 3) {
        throw "missing runtime counter did not fail the L2 gate"
    }
    $runtime.encoderCopiedSubmissions = 0

    $runtime.processId = [string]$pidValue
    Write-Utf8Json $runtime $runtimePath
    $manifest.runtimeEvidence.sha256 = Hash $runtimePath
    Write-Utf8Json $manifest $manifestPath
    if ((Invoke-Analyzer (Join-Path $root "runtime-type-tamper.json")) -ne 3) {
        throw "string runtime PID did not fail strict JSON type validation"
    }
    $runtime.processId = $pidValue
    Write-Utf8Json $runtime $runtimePath
    $manifest.runtimeEvidence.sha256 = Hash $runtimePath
    Write-Utf8Json $manifest $manifestPath

    Add-Content -LiteralPath $eventsPath -Value "tampered CSV"
    if ((Invoke-Analyzer (Join-Path $root "csv-tamper.json")) -ne 1) {
        throw "CSV replacement did not fail export-manifest screening"
    }

    Add-Content -LiteralPath $etl -Value "tampered"
    if ((Invoke-Analyzer (Join-Path $root "etl-tamper.json")) -ne 1) {
        throw "ETL tamper did not fail manifest screening"
    }

    Write-Output "FluxCap qualification self-test passed"
} finally {
    if (-not $KeepArtifacts -and (Test-Path -LiteralPath $root)) {
        $resolvedRoot = [System.IO.Path]::GetFullPath($root)
        $tempRoot = [System.IO.Path]::GetFullPath(
            [System.IO.Path]::GetTempPath())
        if (-not $resolvedRoot.StartsWith(
                $tempRoot, [StringComparison]::OrdinalIgnoreCase) `
            -or (Split-Path -Leaf $resolvedRoot) `
                -notlike 'FluxCapQualificationSelfTest-*') {
            throw "refusing to remove an unexpected self-test path"
        }
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}
