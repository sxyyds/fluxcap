[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $Executable,

    [string[]] $ArgumentList = @(),

    [string] $OutputDirectory = (Join-Path $PWD "fluxcap-l3-artifacts"),

    [string] $WpaExporterPath,

    # Optional machine-readable L2 output written by the workload. The file is
    # read only after the workload exits, so the workload may create it.
    [string] $RuntimeEvidenceJson,

    # Exact adapter LUID printed by --list-adapters. Required for an L2/L3
    # runtime-evidence tuple so a trace cannot be relabeled after capture.
    [string] $ExpectedAdapterLuid,

    [ValidateSet("not_applicable", "identity", "rotate90", "rotate180", "rotate270")]
    [string] $ExpectedRotation = "not_applicable",

    [switch] $SkipWpaExport
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$wprProfile = Join-Path $scriptRoot "fluxcap-copy-qualification.wprp"
$wpaProfile = Join-Path $scriptRoot "fluxcap-copy-events.wpaProfile"
$analyzer = Join-Path $scriptRoot "Test-FluxCapL3Trace.ps1"
foreach ($asset in @($wprProfile, $wpaProfile, $analyzer)) {
    if (-not (Test-Path -LiteralPath $asset -PathType Leaf)) {
        throw "Qualification asset was not found: '$asset'."
    }
}
$wprProfileHashBefore = (Get-FileHash `
    -LiteralPath $wprProfile -Algorithm SHA256).Hash
$wpaProfileHashBefore = (Get-FileHash `
    -LiteralPath $wpaProfile -Algorithm SHA256).Hash
$analyzerHashBefore = (Get-FileHash `
    -LiteralPath $analyzer -Algorithm SHA256).Hash

$wpr = (Get-Command wpr.exe -ErrorAction Stop).Source
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "WPR GPU/kernel qualification requires an elevated PowerShell session."
}

if (-not $WpaExporterPath) {
    $exporter = Get-Command wpaexporter.exe -ErrorAction SilentlyContinue
    if ($exporter) {
        $WpaExporterPath = $exporter.Source
    } else {
        $candidate = Join-Path ${env:ProgramFiles(x86)} `
            "Windows Kits\10\Windows Performance Toolkit\wpaexporter.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $WpaExporterPath = $candidate
        }
    }
}
if (-not $SkipWpaExport -and $WpaExporterPath) {
    $exporterItem = Get-Item -LiteralPath $WpaExporterPath -ErrorAction Stop
    $depsJson = Join-Path $exporterItem.DirectoryName "wpaexporter.deps.json"
    if (-not (Test-Path -LiteralPath $depsJson -PathType Leaf)) {
        throw "WPAExporter installation is incomplete: '$depsJson' is missing. Repair the Windows Performance Toolkit or use -SkipWpaExport to preserve an ETL for later export."
    }
}

$runtimeEvidenceCandidate = $null
$qualificationRunNonce = $null
if (-not [string]::IsNullOrWhiteSpace($RuntimeEvidenceJson)) {
    $runtimeEvidenceCandidate = [System.IO.Path]::GetFullPath(
        $RuntimeEvidenceJson)
    if (Test-Path -LiteralPath $runtimeEvidenceCandidate) {
        throw "RuntimeEvidenceJson must be a fresh workload output path; move or remove the existing file before capture."
    }
    if (@($ArgumentList | Where-Object {
            [string]$_ -like '--qualification-run-nonce*'
        }).Count -ne 0) {
        throw "The runner owns --qualification-run-nonce; remove it from ArgumentList."
    }
    $qualificationRunNonce = [Guid]::NewGuid().ToString("N")
}

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$executableHash = (Get-FileHash `
    -LiteralPath $resolvedExecutable -Algorithm SHA256).Hash
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null
$stamp = [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss")
$suffix = [Guid]::NewGuid().ToString("N").Substring(0, 8)
$runDirectory = Join-Path $resolvedOutput "fluxcap-l3-$stamp-$suffix"
$exportDirectory = Join-Path $runDirectory "wpa-export"
New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $exportDirectory | Out-Null

$etl = Join-Path $runDirectory "fluxcap-copy.etl"
$stdout = Join-Path $runDirectory "workload.stdout.txt"
$stderr = Join-Path $runDirectory "workload.stderr.txt"
$manifestPath = Join-Path $runDirectory "capture-manifest.json"
$exportManifestPath = Join-Path $runDirectory "wpa-export-manifest.json"
$qualificationPath = Join-Path $runDirectory "qualification.json"
$capturedWprProfile = Join-Path $runDirectory "capture-profile.wprp"
$capturedWpaProfile = Join-Path $runDirectory "export-profile.wpaProfile"
$capturedAnalyzer = Join-Path $runDirectory "trace-analyzer.ps1"
$capturedRuntimeEvidence = Join-Path $runDirectory "runtime-l2-evidence.json"
$profileSpec = "$wprProfile!FluxCapCopyQualification"
$traceStarted = $false
$process = $null
$startUtc = $null
$endUtc = $null

# Start-Process joins ArgumentList elements into one command line. Quote each
# element with the CommandLineToArgvW/CRT backslash rules so paths containing
# whitespace or quotes remain one workload argument.
function ConvertTo-ProcessArgument([AllowNull()][string] $Value) {
    if ($null -eq $Value -or $Value.Length -eq 0) {
        return '""'
    }
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append([char]0x22)
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq [char]0x5c) {
            ++$backslashes
            continue
        }
        if ($character -eq [char]0x22) {
            if ($backslashes -gt 0) {
                [void]$builder.Append([char]0x5c, $backslashes * 2)
            }
            [void]$builder.Append([char]0x5c)
            [void]$builder.Append([char]0x22)
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append([char]0x5c, $backslashes)
            $backslashes = 0
        }
        [void]$builder.Append($character)
    }
    if ($backslashes -gt 0) {
        [void]$builder.Append([char]0x5c, $backslashes * 2)
    }
    [void]$builder.Append([char]0x22)
    return $builder.ToString()
}
$effectiveArgumentList = @($ArgumentList)
if ($qualificationRunNonce) {
    $effectiveArgumentList += @(
        "--qualification-run-nonce", $qualificationRunNonce)
}
$processArgumentList = @($effectiveArgumentList | ForEach-Object {
    ConvertTo-ProcessArgument ([string]$_)
})

function Get-ExactArgumentValue(
    [string[]] $Arguments,
    [string] $Name,
    [AllowNull()][string] $DefaultValue = $null) {
    $matches = New-Object System.Collections.Generic.List[string]
    for ($index = 0; $index -lt $Arguments.Count; ++$index) {
        $argument = [string]$Arguments[$index]
        if ($argument -eq $Name) {
            if ($index + 1 -ge $Arguments.Count) {
                throw "$Name requires a value."
            }
            $matches.Add([string]$Arguments[++$index])
        } elseif ($argument.StartsWith(
                "$Name=", [StringComparison]::Ordinal)) {
            $matches.Add($argument.Substring($Name.Length + 1))
        }
    }
    if ($matches.Count -gt 1) {
        throw "$Name may be specified only once for qualification."
    }
    if ($matches.Count -eq 1) { return $matches[0] }
    return $DefaultValue
}

function Get-TextSha256([string] $Text) {
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return (($algorithm.ComputeHash($bytes) | ForEach-Object {
            $_.ToString("x2")
        }) -join "")
    } finally {
        $algorithm.Dispose()
    }
}

function Test-JsonIntegerType([object] $Value) {
    return $Value -is [byte] -or $Value -is [sbyte] `
        -or $Value -is [int16] -or $Value -is [uint16] `
        -or $Value -is [int32] -or $Value -is [uint32] `
        -or $Value -is [int64] -or $Value -is [uint64]
}

function Test-JsonUInt64Type([object] $Value) {
    if (-not (Test-JsonIntegerType $Value)) { return $false }
    try {
        return [decimal]$Value -ge 0
    } catch {
        return $false
    }
}

$expectedWorkload = $null
$expectedWorkloadSha256 = $null
if ($runtimeEvidenceCandidate) {
    if ($ExpectedAdapterLuid -notmatch '^0x[0-9A-F]{8}:0x[0-9A-F]{8}$') {
        throw "ExpectedAdapterLuid must use canonical 0xHHHHHHHH:0xLLLLLLLL form."
    }
    $consumerMode = Get-ExactArgumentValue `
        $effectiveArgumentList "--consumer-mode" "inproc"
    $busFormat = Get-ExactArgumentValue `
        $effectiveArgumentList "--bus-format" $null
    $codec = Get-ExactArgumentValue $effectiveArgumentList "--codec" $null
    $captureBackendArgument = Get-ExactArgumentValue `
        $effectiveArgumentList "--capture-backend" "wgc"
    $planarBackendArgument = Get-ExactArgumentValue `
        $effectiveArgumentList "--planar-backend" $null
    $pairs = Get-ExactArgumentValue $effectiveArgumentList "--pairs" $null
    $outputSize = Get-ExactArgumentValue $effectiveArgumentList "--size" $null
    $sourceSize = Get-ExactArgumentValue `
        $effectiveArgumentList "--source-size" $outputSize
    $runtimeArgument = Get-ExactArgumentValue `
        $effectiveArgumentList "--runtime-evidence-json" $null
    $outputIndex = Get-ExactArgumentValue `
        $effectiveArgumentList "--output-index" $null
    if ($consumerMode -ne "inproc" -or $busFormat -notin @("nv12", "p010") `
        -or $codec -notin @("h264", "hevc", "av1") `
        -or $captureBackendArgument -notin @("wgc", "desktop-duplication") `
        -or $planarBackendArgument `
            -notin @("deterministic-planar", "video-processor") `
        -or $pairs -ne "1" `
        -or $outputSize -notin @("320", "640") `
        -or $sourceSize -notin @("320", "640")) {
        throw "Runtime qualification requires one explicit inproc planar tuple with codec/backend/geometry and pairs=1."
    }
    if ([string]::IsNullOrWhiteSpace($runtimeArgument) `
        -or -not [string]::Equals(
            [System.IO.Path]::GetFullPath($runtimeArgument),
            $runtimeEvidenceCandidate,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "ArgumentList --runtime-evidence-json must exactly match RuntimeEvidenceJson."
    }
    $captureBackend = if ($captureBackendArgument -eq "desktop-duplication") {
        "desktop_duplication"
    } else {
        "wgc"
    }
    $captureTarget = if ($captureBackend -eq "desktop_duplication") {
        "monitor"
    } else {
        "window"
    }
    if (($captureBackend -eq "wgc" `
            -and $ExpectedRotation -ne "not_applicable") `
        -or ($captureBackend -eq "desktop_duplication" `
            -and $ExpectedRotation -eq "not_applicable")) {
        throw "ExpectedRotation is inconsistent with the selected capture backend."
    }
    $parsedOutputIndex = [uint32]0
    if ($captureBackend -eq "desktop_duplication") {
        if ($null -eq $outputIndex -or $outputIndex -notmatch '^[0-9]+$' `
            -or -not [uint32]::TryParse(
                $outputIndex,
                [Globalization.NumberStyles]::None,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$parsedOutputIndex) `
            -or $parsedOutputIndex -gt 31) {
            throw "Desktop Duplication qualification requires --output-index in [0,31]."
        }
        if ($ExpectedRotation -ne "identity" `
            -and $planarBackendArgument -ne "video-processor") {
            throw "Rotated Desktop Duplication qualification requires video-processor."
        }
    } elseif ($null -ne $outputIndex) {
        throw "WGC qualification does not accept --output-index."
    }
    $expectedWorkload = [ordered]@{
        adapterLuid = $ExpectedAdapterLuid
        captureBackend = $captureBackend
        captureTarget = $captureTarget
        outputIndex = if ($captureBackend -eq "desktop_duplication") {
            $parsedOutputIndex
        } else {
            $null
        }
        rotation = $ExpectedRotation
        transformBackend = $planarBackendArgument.Replace("-", "_")
        format = $busFormat.ToUpperInvariant()
        codec = $codec
        sourceWidth = [uint32]$sourceSize
        sourceHeight = [uint32]$sourceSize
        outputWidth = [uint32]$outputSize
        outputHeight = [uint32]$outputSize
    }
    $expectedWorkloadJson = $expectedWorkload |
        ConvertTo-Json -Compress
    $expectedWorkloadSha256 = Get-TextSha256 $expectedWorkloadJson
}

try {
    & $wpr -start $profileSpec -filemode
    if ($LASTEXITCODE -ne 0) {
        throw "WPR failed to start profile '$profileSpec' (exit $LASTEXITCODE)."
    }
    $traceStarted = $true
    $startUtc = [DateTime]::UtcNow

    $process = Start-Process `
        -FilePath $resolvedExecutable `
        -ArgumentList $processArgumentList `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -WindowStyle Hidden `
        -PassThru
    $process.WaitForExit()
    $endUtc = [DateTime]::UtcNow

    & $wpr -stop $etl
    if ($LASTEXITCODE -ne 0) {
        throw "WPR failed to save '$etl' (exit $LASTEXITCODE)."
    }
    $traceStarted = $false
} catch {
    if ($traceStarted) {
        & $wpr -cancel 2>$null | Out-Null
    }
    throw
}

# Preserve the exact qualification inputs alongside the ETL. A later repository
# edit must not silently change what an existing result claims to have used.
Copy-Item -LiteralPath $wprProfile -Destination $capturedWprProfile
Copy-Item -LiteralPath $wpaProfile -Destination $capturedWpaProfile
Copy-Item -LiteralPath $analyzer -Destination $capturedAnalyzer

$runtimeEvidencePath = $null
$runtimeEvidenceRejection = $null
if ($runtimeEvidenceCandidate) {
    if (Test-Path -LiteralPath $runtimeEvidenceCandidate -PathType Leaf) {
        $runtimeEvidenceItem = Get-Item -LiteralPath $runtimeEvidenceCandidate
        $runtimeWriteTimeValid = $runtimeEvidenceItem.LastWriteTimeUtc `
            -ge $startUtc.AddSeconds(-2) `
            -and $runtimeEvidenceItem.LastWriteTimeUtc `
                -le $endUtc.AddSeconds(2)
        if (-not $runtimeWriteTimeValid) {
            $runtimeEvidenceRejection =
                "runtime evidence file timestamp is outside the capture run"
        } elseif ($process.ExitCode -ne 0) {
            $runtimeEvidenceRejection =
                "workload exit code $($process.ExitCode) cannot produce runtime evidence"
        } elseif ($runtimeEvidenceItem.Length -eq 0) {
            $runtimeEvidenceRejection =
                "workload runtime output is empty"
        } else {
            $runtimeEnvelopeValid = $false
            try {
                $runtimeEnvelope = Get-Content `
                    -LiteralPath $runtimeEvidenceCandidate `
                    -Raw -Encoding UTF8 | ConvertFrom-Json
                $runtimeEnvelopeValid =
                    (Test-JsonIntegerType $runtimeEnvelope.schemaVersion) `
                    -and [uint64]$runtimeEnvelope.schemaVersion -eq 2 `
                    -and (Test-JsonIntegerType $runtimeEnvelope.evidenceLevel) `
                    -and [uint64]$runtimeEnvelope.evidenceLevel -eq 2 `
                    -and $runtimeEnvelope.evidenceName -is [string] `
                    -and $runtimeEnvelope.evidenceName -ceq
                        "l2_external_texture_identity_and_encoder_bind" `
                    -and (Test-JsonIntegerType $runtimeEnvelope.processId) `
                    -and [uint64]$runtimeEnvelope.processId -eq
                        [uint64]$process.Id `
                    -and $runtimeEnvelope.qualificationRunNonce -is [string] `
                    -and $runtimeEnvelope.qualificationRunNonce -ceq
                        $qualificationRunNonce `
                    -and $runtimeEnvelope.executableSha256 -is [string] `
                    -and [string]::Equals(
                        [string]$runtimeEnvelope.executableSha256,
                        $executableHash,
                        [StringComparison]::OrdinalIgnoreCase)
                if ($runtimeEnvelopeValid) {
                    $runtimeCaptureBackend =
                        $runtimeEnvelope.qualificationTuple.captureBackend
                    $presentationTypesValid =
                        (Test-JsonUInt64Type `
                            $runtimeEnvelope.desktopPresentFrames) `
                        -and (Test-JsonUInt64Type `
                            $runtimeEnvelope.measuredSourcePresentations) `
                        -and (Test-JsonUInt64Type `
                            $runtimeEnvelope.measuredDesktopPresentFrames) `
                        -and (Test-JsonUInt64Type `
                            $runtimeEnvelope.busPublishedFrames)
                    $runtimeEnvelopeValid = $presentationTypesValid `
                        -and (($runtimeCaptureBackend -ceq "wgc" `
                                -and [uint64]$runtimeEnvelope.desktopPresentFrames `
                                    -eq 0 `
                                -and [uint64]$runtimeEnvelope.measuredDesktopPresentFrames `
                                    -eq 0 `
                                -and [uint64]$runtimeEnvelope.measuredSourcePresentations `
                                    -gt 0) `
                            -or ($runtimeCaptureBackend -ceq
                                    "desktop_duplication" `
                                -and [uint64]$runtimeEnvelope.desktopPresentFrames `
                                    -gt 0 `
                                -and [uint64]$runtimeEnvelope.measuredSourcePresentations `
                                    -gt 0 `
                                -and [uint64]$runtimeEnvelope.measuredDesktopPresentFrames `
                                    -gt 0 `
                                -and [uint64]$runtimeEnvelope.measuredDesktopPresentFrames `
                                    -le [uint64]$runtimeEnvelope.desktopPresentFrames `
                                -and [uint64]$runtimeEnvelope.desktopPresentFrames `
                                    -le [uint64]$runtimeEnvelope.busPublishedFrames))
                }
            } catch {
                $runtimeEnvelopeValid = $false
            }
            if (-not $runtimeEnvelopeValid) {
                $runtimeEvidenceRejection =
                    "workload runtime output is not a canonical current-contract L2 claim for this PID, nonce, executable, and capture backend"
            }
        }
        if ($null -eq $runtimeEvidenceRejection `
            -and -not [string]::Equals(
                $runtimeEvidenceCandidate,
                $capturedRuntimeEvidence,
                [StringComparison]::OrdinalIgnoreCase)) {
            Copy-Item `
                -LiteralPath $runtimeEvidenceCandidate `
                -Destination $capturedRuntimeEvidence
            $runtimeEvidencePath = $capturedRuntimeEvidence
        } elseif ($null -eq $runtimeEvidenceRejection) {
            $runtimeEvidencePath = $capturedRuntimeEvidence
        }
    } else {
        $runtimeEvidenceRejection =
            "workload did not create the requested runtime evidence file"
    }
}

$videoControllers = @(
    Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
        ForEach-Object {
            [ordered]@{
                name = $_.Name
                driverVersion = $_.DriverVersion
                pnpDeviceId = $_.PNPDeviceID
                videoProcessor = $_.VideoProcessor
            }
        }
)
$os = Get-CimInstance Win32_OperatingSystem
$wprItem = Get-Item -LiteralPath $wpr
$exporterItem = if ($WpaExporterPath `
    -and (Test-Path -LiteralPath $WpaExporterPath -PathType Leaf)) {
    Get-Item -LiteralPath $WpaExporterPath
} else {
    $null
}
$etlHash = (Get-FileHash -LiteralPath $etl -Algorithm SHA256).Hash
$executableHashAfter = (Get-FileHash `
    -LiteralPath $resolvedExecutable -Algorithm SHA256).Hash
$capturedWprProfileHash = (Get-FileHash `
    -LiteralPath $capturedWprProfile -Algorithm SHA256).Hash
$capturedWpaProfileHash = (Get-FileHash `
    -LiteralPath $capturedWpaProfile -Algorithm SHA256).Hash
$capturedAnalyzerHash = (Get-FileHash `
    -LiteralPath $capturedAnalyzer -Algorithm SHA256).Hash
$qualificationInputsStable = [string]::Equals(
        $wprProfileHashBefore,
        $capturedWprProfileHash,
        [StringComparison]::OrdinalIgnoreCase) `
    -and [string]::Equals(
        $wpaProfileHashBefore,
        $capturedWpaProfileHash,
        [StringComparison]::OrdinalIgnoreCase) `
    -and [string]::Equals(
        $analyzerHashBefore,
        $capturedAnalyzerHash,
        [StringComparison]::OrdinalIgnoreCase)
$manifest = [ordered]@{
    schemaVersion = 3
    qualification = "FluxCap L3 ETW copy observation"
    status = "trace-captured"
    evidenceLevel = 0
    traceScope = "spawned-process-lifetime-including-warmup"
    executable = $resolvedExecutable
    executableSha256 = $executableHash
    executableSha256After = $executableHashAfter
    executableHashStable = [string]::Equals(
        $executableHash,
        $executableHashAfter,
        [StringComparison]::OrdinalIgnoreCase)
    arguments = @($effectiveArgumentList)
    qualificationRunNonce = $qualificationRunNonce
    expectedWorkload = $expectedWorkload
    expectedWorkloadSha256 = $expectedWorkloadSha256
    processId = $process.Id
    exitCode = $process.ExitCode
    traceStartUtc = $startUtc.ToString("o")
    traceEndUtc = $endUtc.ToString("o")
    etl = $etl
    etlSha256 = $etlHash
    stdout = $stdout
    stdoutSha256 = (Get-FileHash -LiteralPath $stdout -Algorithm SHA256).Hash
    stderr = $stderr
    stderrSha256 = (Get-FileHash -LiteralPath $stderr -Algorithm SHA256).Hash
    profiles = [ordered]@{
        inputsStable = $qualificationInputsStable
        wpr = $capturedWprProfile
        wprSha256 = $capturedWprProfileHash
        wpa = $capturedWpaProfile
        wpaSha256 = $capturedWpaProfileHash
        analyzer = $capturedAnalyzer
        analyzerSha256 = $capturedAnalyzerHash
    }
    runtimeEvidence = if ($runtimeEvidencePath) {
        [ordered]@{
            path = $runtimeEvidencePath
            sha256 = (Get-FileHash `
                -LiteralPath $runtimeEvidencePath -Algorithm SHA256).Hash
        }
    } else {
        $null
    }
    runtimeEvidenceRejection = $runtimeEvidenceRejection
    exportManifest = $null
    tooling = [ordered]@{
        wpr = $wpr
        wprFileVersion = $wprItem.VersionInfo.FileVersion
        wpaExporter = if ($exporterItem) { $exporterItem.FullName } else { $null }
        wpaExporterFileVersion = if ($exporterItem) {
            $exporterItem.VersionInfo.FileVersion
        } else {
            $null
        }
        # Some incomplete ADK installations print help/version text and then
        # fail because this runtime dependency manifest is absent. Record it
        # explicitly; a real ETL export is the acceptance test.
        wpaExporterDepsJsonPresent = if ($exporterItem) {
            Test-Path -LiteralPath (Join-Path `
                $exporterItem.DirectoryName "wpaexporter.deps.json")
        } else {
            $false
        }
    }
    os = [ordered]@{
        caption = $os.Caption
        version = $os.Version
        buildNumber = $os.BuildNumber
    }
    videoControllers = $videoControllers
    providers = @(
        [ordered]@{ name = "Microsoft-Windows-Direct3D11"; keywords = "0xffffffffffffffff" },
        [ordered]@{ name = "Microsoft-Windows-DXGI"; keywords = "0xffffffffffffffff" },
        [ordered]@{ name = "Microsoft-Windows-DxgKrnl"; keywords = "0x277" },
        [ordered]@{ name = "Microsoft-Windows-MediaFoundation-Performance" },
        [ordered]@{ name = "Microsoft-Windows-MediaFoundation-Performance-Core" },
        [ordered]@{ name = "Microsoft-Windows-MediaFoundation-Platform" },
        [ordered]@{ name = "Microsoft-Windows-MFH264Enc" }
    )
    runtimeBoundary = [ordered]@{
        mftInternalCopyObservable = $false
        driverPrivateSurfaceObservable = $false
        hardwareDmaAndCacheObservable = $false
    }
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content `
    -LiteralPath $manifestPath -Encoding UTF8

if (-not $SkipWpaExport) {
    if (-not $exporterItem) {
        throw "WPAExporter was not found; ETL and capture manifest remain in '$runDirectory'."
    }
    & $WpaExporterPath `
        -i $etl `
        -profile $capturedWpaProfile `
        -outputfolder $exportDirectory `
        -prefix "fluxcap_" `
        -outputformat CSV
    if ($LASTEXITCODE -ne 0) {
        throw "WPAExporter failed (exit $LASTEXITCODE); ETL remains in '$runDirectory'."
    }
    $genericEvents = Get-ChildItem -LiteralPath $exportDirectory -Filter *.csv |
        Where-Object { $_.Name -match "FluxCapCopyEvents|GenericEvents" } |
        Select-Object -First 1
    if (-not $genericEvents) {
        throw "WPAExporter produced no Generic Events CSV in '$exportDirectory'."
    }
    $exportManifest = [ordered]@{
        schemaVersion = 1
        etl = $etl
        etlSha256 = $etlHash
        wpaProfile = $capturedWpaProfile
        wpaProfileSha256 = $capturedWpaProfileHash
        exporter = $exporterItem.FullName
        exporterSha256 = (Get-FileHash `
            -LiteralPath $exporterItem.FullName -Algorithm SHA256).Hash
        exporterFileVersion = $exporterItem.VersionInfo.FileVersion
        genericEventsCsv = $genericEvents.FullName
        genericEventsCsvSha256 = (Get-FileHash `
            -LiteralPath $genericEvents.FullName -Algorithm SHA256).Hash
    }
    $exportManifest | ConvertTo-Json -Depth 6 | Set-Content `
        -LiteralPath $exportManifestPath -Encoding UTF8
    $manifest.exportManifest = [ordered]@{
        path = $exportManifestPath
        sha256 = (Get-FileHash `
            -LiteralPath $exportManifestPath -Algorithm SHA256).Hash
    }
    $manifest | ConvertTo-Json -Depth 8 | Set-Content `
        -LiteralPath $manifestPath -Encoding UTF8
    $analyzerArguments = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $capturedAnalyzer,
        "-Manifest", $manifestPath,
        "-GenericEventsCsv", $genericEvents.FullName,
        "-Output", $qualificationPath
    )
    if ($runtimeEvidencePath) {
        $analyzerArguments += @(
            "-RuntimeEvidenceJson", $runtimeEvidencePath)
    }
    $powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
    & $powershell @analyzerArguments
    $analyzerExitCode = $LASTEXITCODE
    if ($analyzerExitCode -notin @(0, 1, 2, 3)) {
        throw "Trace screening failed with exit $analyzerExitCode."
    }
}

[pscustomobject]@{
    RunDirectory = $runDirectory
    Manifest = $manifestPath
    Etl = $etl
    Qualification = if (Test-Path $qualificationPath) {
        $qualificationPath
    } else {
        $null
    }
    RuntimeEvidenceCaptured = [bool]$runtimeEvidencePath
    RuntimeEvidenceRejection = $runtimeEvidenceRejection
    WorkloadExitCode = $process.ExitCode
}
