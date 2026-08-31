[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $Manifest,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $GenericEventsCsv,

    [Parameter(Mandatory = $true)]
    [string] $Output,

    # L3 includes L2. This must be machine-readable output from the exact
    # workload PID, not a reviewer assertion.
    [string] $RuntimeEvidenceJson,

    [switch] $ConfirmReviewedWorkloadInterval,

    [switch] $ConfirmNoFullFrameD3D11Copies,

    [switch] $ConfirmNoCopyEnginePackets,

    [switch] $ConfirmMediaFoundationActivity,

    [switch] $ConfirmNoLostEvents,

    [string] $Reviewer = ""
)

$ErrorActionPreference = "Stop"
$capture = Get-Content -LiteralPath $Manifest -Raw -Encoding UTF8 |
    ConvertFrom-Json
$events = @(Import-Csv -LiteralPath $GenericEventsCsv -Encoding UTF8)

function Join-EventText([object] $event) {
    return (($event.PSObject.Properties | ForEach-Object {
        [string]$_.Value
    }) -join " ")
}

function Get-EventProvider([object] $Event) {
    $values = New-Object System.Collections.Generic.List[string]
    foreach ($name in @(
            "Provider Name", "ProviderName", "Provider",
            "Provider Id", "ProviderId", "Provider ID")) {
        $property = $Event.PSObject.Properties[$name]
        if ($null -ne $property `
            -and -not [string]::IsNullOrWhiteSpace([string]$property.Value)) {
            $values.Add(([string]$property.Value).Trim())
        }
    }
    if ($values.Count -eq 0) { return "" }
    $distinct = @($values | Select-Object -Unique)
    if ($distinct.Count -ne 1) { return "" }
    return $distinct[0]
}

function Get-EventProcessId([object] $Event) {
    $numericValues = New-Object System.Collections.Generic.List[string]
    foreach ($name in @("ProcessId", "Process ID", "PID")) {
        $property = $Event.PSObject.Properties[$name]
        if ($null -ne $property `
            -and -not [string]::IsNullOrWhiteSpace([string]$property.Value)) {
            $numericValues.Add(([string]$property.Value).Trim())
        }
    }
    if ($numericValues.Count -ne 0) {
        $parsedValues = New-Object System.Collections.Generic.List[uint64]
        foreach ($value in $numericValues) {
            $parsed = [uint64]0
            if ($value -notmatch '^[0-9]+$' `
                -or -not [uint64]::TryParse(
                    $value,
                    [Globalization.NumberStyles]::None,
                    [Globalization.CultureInfo]::InvariantCulture,
                    [ref]$parsed)) {
                return [uint64]0
            }
            $parsedValues.Add($parsed)
        }
        $distinct = @($parsedValues | Select-Object -Unique)
        if ($distinct.Count -ne 1) { return [uint64]0 }
        return [uint64]$distinct[0]
    }
    $processValues = New-Object System.Collections.Generic.List[uint64]
    foreach ($name in @("Process", "Process Name")) {
        $property = $Event.PSObject.Properties[$name]
        if ($null -ne $property `
            -and ([string]$property.Value).Trim() -match '\(([0-9]+)\)$') {
            $parsed = [uint64]0
            if (-not [uint64]::TryParse(
                    $Matches[1],
                    [Globalization.NumberStyles]::None,
                    [Globalization.CultureInfo]::InvariantCulture,
                    [ref]$parsed)) {
                return [uint64]0
            }
            $processValues.Add($parsed)
        }
    }
    $distinct = @($processValues | Select-Object -Unique)
    if ($distinct.Count -eq 1) { return [uint64]$distinct[0] }
    return [uint64]0
}

function Test-EventProvider(
    [object] $Event,
    [string[]] $Names,
    [string[]] $Guids) {
    $provider = Get-EventProvider $Event
    if ([string]::IsNullOrWhiteSpace($provider)) { return $false }
    foreach ($name in $Names) {
        if ([string]::Equals(
                $provider, $name, [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    $normalized = $provider.Trim('{}').ToUpperInvariant()
    return $Guids -contains $normalized
}

function Test-HashBinding(
    [object] $Path,
    [object] $ExpectedSha256) {
    if ($Path -isnot [string] `
        -or $ExpectedSha256 -isnot [string] `
        -or [string]::IsNullOrWhiteSpace([string]$Path) `
        -or [string]$ExpectedSha256 -notmatch '^[0-9a-fA-F]{64}$' `
        -or -not (Test-Path -LiteralPath ([string]$Path) -PathType Leaf)) {
        return $false
    }
    $actual = (Get-FileHash `
        -LiteralPath ([string]$Path) -Algorithm SHA256).Hash
    return [string]::Equals(
        $actual, [string]$ExpectedSha256,
        [StringComparison]::OrdinalIgnoreCase)
}

function Get-RequiredJsonProperty(
    [object] $Object,
    [string] $Name,
    [object] $Errors) {
    if ($null -eq $Object) {
        $Errors.Add("missing JSON object for '$Name'")
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        $Errors.Add("runtime evidence lacks required field '$Name'")
        return $null
    }
    return $property.Value
}

function Read-RequiredJsonString(
    [object] $Object,
    [string] $Name,
    [object] $Errors) {
    $value = Get-RequiredJsonProperty $Object $Name $Errors
    if ($null -eq $value) { return "" }
    if ($value -isnot [string] -or [string]::IsNullOrWhiteSpace($value)) {
        $Errors.Add("runtime evidence field '$Name' must be a nonempty string")
        return ""
    }
    return [string]$value
}

function Test-JsonIntegerType([object] $Value) {
    return $Value -is [byte] -or $Value -is [sbyte] `
        -or $Value -is [int16] -or $Value -is [uint16] `
        -or $Value -is [int32] -or $Value -is [uint32] `
        -or $Value -is [int64] -or $Value -is [uint64]
}

function Test-JsonUInt32Type([object] $Value) {
    if (-not (Test-JsonIntegerType $Value)) { return $false }
    try {
        $number = [decimal]$Value
        return $number -ge 0 -and $number -le [uint32]::MaxValue
    } catch {
        return $false
    }
}

function Test-JsonInt32Type([object] $Value) {
    if (-not (Test-JsonIntegerType $Value)) { return $false }
    try {
        $number = [decimal]$Value
        return $number -ge [int32]::MinValue `
            -and $number -le [int32]::MaxValue
    } catch {
        return $false
    }
}

function Read-RequiredJsonUInt64(
    [object] $Object,
    [string] $Name,
    [object] $Errors) {
    $value = Get-RequiredJsonProperty $Object $Name $Errors
    if ($null -eq $value) { return [uint64]0 }
    if (-not (Test-JsonIntegerType $value)) {
        $Errors.Add("runtime evidence field '$Name' must be an integer")
        return [uint64]0
    }
    try {
        if ([decimal]$value -lt 0) { throw "negative" }
        return [uint64]$value
    } catch {
        $Errors.Add("runtime evidence field '$Name' is outside uint64 range")
        return [uint64]0
    }
}

function Read-RequiredJsonBool(
    [object] $Object,
    [string] $Name,
    [object] $Errors) {
    $value = Get-RequiredJsonProperty $Object $Name $Errors
    if ($null -eq $value) { return $false }
    if ($value -isnot [bool]) {
        $Errors.Add("runtime evidence field '$Name' must be a JSON boolean")
        return $false
    }
    return [bool]$value
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

$captureProcessIdValid = (Test-JsonUInt32Type $capture.processId) `
    -and [uint64]$capture.processId -gt 0
$captureProcessId = if ($captureProcessIdValid) {
    [uint32]$capture.processId
} else {
    [uint32]0
}

$providerD3D = @($events | Where-Object {
    Test-EventProvider $_ @("Microsoft-Windows-Direct3D11") `
        @("DB6F6DDB-AC77-4E88-8253-819DF9BBF140")
})
$providerDxgi = @($events | Where-Object {
    Test-EventProvider $_ @("Microsoft-Windows-DXGI") `
        @("CA11C036-0102-4A2D-A6AD-F03CFED5D3C9")
})
$providerDxgKrnl = @($events | Where-Object {
    Test-EventProvider $_ @("Microsoft-Windows-DxgKrnl") `
        @("802EC45A-1E99-4B83-9920-87C98277BA9D")
})
$mediaFoundationNames = @(
    "Microsoft-Windows-MediaFoundation-Performance",
    "Microsoft-Windows-MediaFoundation-Performance-Core",
    "Microsoft-Windows-MediaFoundation-Platform",
    "Microsoft-Windows-MFH264Enc")
$mediaFoundationGuids = @(
    "F404B94E-27E0-4384-BFE8-1D8D390B0AA3",
    "B20E65AC-C905-4014-8F78-1B6A508142EB",
    "BC97B970-D001-482F-8745-B8D7D5759F99",
    "2A49DE31-8A5B-4D3A-A904-7FC7409AE90D")
$providerMf = @($events | Where-Object {
    Test-EventProvider $_ $mediaFoundationNames $mediaFoundationGuids
})
$targetEvents = @($events | Where-Object {
    $captureProcessIdValid `
        -and (Get-EventProcessId $_) -eq [uint64]$captureProcessId
})
$targetD3D = @($targetEvents | Where-Object {
    Test-EventProvider $_ @("Microsoft-Windows-Direct3D11") `
        @("DB6F6DDB-AC77-4E88-8253-819DF9BBF140")
})
$targetDxgi = @($targetEvents | Where-Object {
    Test-EventProvider $_ @("Microsoft-Windows-DXGI") `
        @("CA11C036-0102-4A2D-A6AD-F03CFED5D3C9")
})
$targetMf = @($targetEvents | Where-Object {
    Test-EventProvider $_ $mediaFoundationNames $mediaFoundationGuids
})

# These are candidates, not verdicts: dimensions/subresources must be reviewed
# before deciding whether a call is a prohibited full-frame copy.
$d3dCopyCandidates = @($targetD3D | Where-Object {
    (Join-EventText $_) -match (
        "CopyResource|CopySubresourceRegion|ResourceCopy|ResourceCopyRegion")
})

# Generic Events cannot reliably map every kernel packet/context back to the
# workload. Keep this only as a reviewer hint; the GPU Hardware Queue view is
# the authoritative L3 review input.
$copyEngineHeuristicCandidates = @($providerDxgKrnl | Where-Object {
    (Join-EventText $_) -match (
        "CopyEngine|Engine[^ ]*Copy|DMA_PACKET_TYPE_COPY|PacketType[^ ]*Copy")
})
$lostEventCandidates = @($events | Where-Object {
    $text = Join-EventText $_
    $text -match "EventTrace|Kernel-EventTracing|NT Kernel Logger" `
        -and $text -match "EventsLost|BuffersLost|LostEvent"
})

$manifestSchemaValid = (Test-JsonUInt32Type $capture.schemaVersion) `
    -and $capture.schemaVersion -eq 3
$captureExitCodeValid = Test-JsonInt32Type $capture.exitCode
$captureNonceValid = $capture.qualificationRunNonce -is [string] `
    -and [string]$capture.qualificationRunNonce -cmatch '^[0-9a-f]{32}$'
$captureHeaderValid = $captureProcessIdValid `
    -and $captureExitCodeValid `
    -and $captureNonceValid `
    -and $capture.qualification -is [string] `
    -and $capture.qualification -ceq "FluxCap L3 ETW copy observation" `
    -and $capture.status -is [string] `
    -and $capture.status -ceq "trace-captured" `
    -and (Test-JsonUInt32Type $capture.evidenceLevel) `
    -and $capture.evidenceLevel -eq 0 `
    -and $capture.traceScope -is [string] `
    -and $capture.traceScope -ceq "spawned-process-lifetime-including-warmup"
$executableHashStable = $capture.executable -is [string] `
    -and $capture.executableSha256 -is [string] `
    -and $capture.executableSha256 `
        -match '^[0-9a-fA-F]{64}$' `
    -and $capture.executableSha256After -is [string] `
    -and $capture.executableSha256After `
        -match '^[0-9a-fA-F]{64}$' `
    -and $capture.executableHashStable -is [bool] `
    -and $capture.executableHashStable `
    -and [string]::Equals(
        [string]$capture.executableSha256,
        [string]$capture.executableSha256After,
        [StringComparison]::OrdinalIgnoreCase)
$timeRangeValid = $false
try {
    if ($capture.traceStartUtc -isnot [string] `
        -or $capture.traceEndUtc -isnot [string]) {
        throw "trace timestamps must be JSON strings"
    }
    $start = [DateTimeOffset]::Parse(
        [string]$capture.traceStartUtc,
        [Globalization.CultureInfo]::InvariantCulture)
    $end = [DateTimeOffset]::Parse(
        [string]$capture.traceEndUtc,
        [Globalization.CultureInfo]::InvariantCulture)
    $timeRangeValid = $end -gt $start `
        -and $start.Offset -eq [TimeSpan]::Zero `
        -and $end.Offset -eq [TimeSpan]::Zero
} catch {
    $timeRangeValid = $false
}
$etlHashValid = Test-HashBinding `
    $capture.etl $capture.etlSha256
$stdoutHashValid = Test-HashBinding `
    $capture.stdout $capture.stdoutSha256
$stderrHashValid = Test-HashBinding `
    $capture.stderr $capture.stderrSha256
$runtimeBoundaryValid = $null -ne $capture.runtimeBoundary `
    -and $null -ne $capture.runtimeBoundary.PSObject.Properties[
        "mftInternalCopyObservable"] `
    -and $null -ne $capture.runtimeBoundary.PSObject.Properties[
        "driverPrivateSurfaceObservable"] `
    -and $null -ne $capture.runtimeBoundary.PSObject.Properties[
        "hardwareDmaAndCacheObservable"] `
    -and $capture.runtimeBoundary.mftInternalCopyObservable -is [bool] `
    -and $capture.runtimeBoundary.driverPrivateSurfaceObservable -is [bool] `
    -and $capture.runtimeBoundary.hardwareDmaAndCacheObservable -is [bool] `
    -and -not $capture.runtimeBoundary.mftInternalCopyObservable `
    -and -not $capture.runtimeBoundary.driverPrivateSurfaceObservable `
    -and -not $capture.runtimeBoundary.hardwareDmaAndCacheObservable
$expectedWorkloadShapeValid = $false
$expectedWorkloadHashValid = $false
$expected = $capture.expectedWorkload
if ($null -ne $expected) {
    $requiredExpectedProperties = @(
        "adapterLuid", "captureBackend", "captureTarget", "outputIndex",
        "rotation", "transformBackend", "format", "codec",
        "sourceWidth", "sourceHeight", "outputWidth", "outputHeight")
    $expectedPropertiesValid = @($requiredExpectedProperties |
        Where-Object {
            $null -eq $expected.PSObject.Properties[$_]
        }).Count -eq 0
    $expectedStringTypesValid = $expected.adapterLuid -is [string] `
        -and $expected.captureBackend -is [string] `
        -and $expected.captureTarget -is [string] `
        -and $expected.rotation -is [string] `
        -and $expected.transformBackend -is [string] `
        -and $expected.format -is [string] `
        -and $expected.codec -is [string]
    $expectedDimensionsValid = (Test-JsonUInt32Type $expected.sourceWidth) `
        -and (Test-JsonUInt32Type $expected.sourceHeight) `
        -and (Test-JsonUInt32Type $expected.outputWidth) `
        -and (Test-JsonUInt32Type $expected.outputHeight) `
        -and [uint64]$expected.sourceWidth -in @(320, 640) `
        -and [uint64]$expected.sourceHeight -eq [uint64]$expected.sourceWidth `
        -and [uint64]$expected.outputWidth -in @(320, 640) `
        -and [uint64]$expected.outputHeight -eq [uint64]$expected.outputWidth
    $expectedBackendValid = if ($expected.captureBackend -ceq
            "desktop_duplication") {
        $expected.captureTarget -ceq "monitor" `
            -and $expected.rotation -cin @(
                "identity", "rotate90", "rotate180", "rotate270") `
            -and (Test-JsonUInt32Type $expected.outputIndex) `
            -and [uint64]$expected.outputIndex -le 31
    } elseif ($expected.captureBackend -ceq "wgc") {
        $expected.captureTarget -ceq "window" `
            -and $expected.rotation -ceq "not_applicable" `
            -and $null -eq $expected.outputIndex
    } else {
        $false
    }
    $expectedWorkloadShapeValid = $expectedPropertiesValid `
        -and $expectedStringTypesValid `
        -and $expectedDimensionsValid `
        -and $expectedBackendValid `
        -and $expected.adapterLuid `
            -cmatch '^0x[0-9A-F]{8}:0x[0-9A-F]{8}$' `
        -and $expected.transformBackend -cin @(
            "deterministic_planar", "video_processor") `
        -and $expected.format -cin @("NV12", "P010") `
        -and $expected.codec -cin @("h264", "hevc", "av1")
    if ($capture.expectedWorkloadSha256 -is [string] `
        -and [string]$capture.expectedWorkloadSha256 `
            -cmatch '^[0-9a-f]{64}$') {
        $expectedWorkloadJson = $expected | ConvertTo-Json -Compress
        $expectedWorkloadHashValid = [string]::Equals(
            (Get-TextSha256 $expectedWorkloadJson),
            [string]$capture.expectedWorkloadSha256,
            [StringComparison]::Ordinal)
    }
}
$expectedWorkloadValid = $expectedWorkloadShapeValid `
    -and $expectedWorkloadHashValid
$profileHashesValid = $false
$runningAnalyzerHashValid = $false
if ($capture.profiles) {
    $runningAnalyzerHashValid = Test-HashBinding `
        $MyInvocation.MyCommand.Path `
        $capture.profiles.analyzerSha256
    $profileHashesValid = $capture.profiles.inputsStable -is [bool] `
        -and $capture.profiles.inputsStable `
        -and (Test-HashBinding `
            $capture.profiles.wpr `
            $capture.profiles.wprSha256) `
        -and (Test-HashBinding `
            $capture.profiles.wpa `
            $capture.profiles.wpaSha256) `
        -and (Test-HashBinding `
            $capture.profiles.analyzer `
            $capture.profiles.analyzerSha256) `
        -and $runningAnalyzerHashValid
}
$exportManifestHashValid = $false
$exportBindingValid = $false
if ($capture.exportManifest `
    -and (Test-HashBinding `
        $capture.exportManifest.path `
        $capture.exportManifest.sha256)) {
    $exportManifestHashValid = $true
    try {
        $exportManifest = Get-Content `
            -LiteralPath ([string]$capture.exportManifest.path) `
            -Raw -Encoding UTF8 | ConvertFrom-Json
        $exportStringFields = @(
            $exportManifest.etl, $exportManifest.etlSha256,
            $exportManifest.wpaProfile, $exportManifest.wpaProfileSha256,
            $exportManifest.exporter, $exportManifest.exporterSha256,
            $exportManifest.genericEventsCsv,
            $exportManifest.genericEventsCsvSha256)
        if (@($exportStringFields | Where-Object {
                $_ -isnot [string]
            }).Count -ne 0) {
            throw "export manifest paths and hashes must be JSON strings"
        }
        $resolvedCsv = (Resolve-Path -LiteralPath $GenericEventsCsv).Path
        $boundCsv = (Resolve-Path `
            -LiteralPath ([string]$exportManifest.genericEventsCsv)).Path
        $exportBindingValid = (Test-JsonUInt32Type `
                $exportManifest.schemaVersion) `
            -and $exportManifest.schemaVersion -eq 1 `
            -and [string]::Equals(
                $resolvedCsv, $boundCsv,
                [StringComparison]::OrdinalIgnoreCase) `
            -and (Test-HashBinding `
                $boundCsv `
                $exportManifest.genericEventsCsvSha256) `
            -and [string]::Equals(
                [string]$exportManifest.etlSha256,
                [string]$capture.etlSha256,
                [StringComparison]::OrdinalIgnoreCase) `
            -and (Test-HashBinding `
                $exportManifest.etl `
                $exportManifest.etlSha256) `
            -and [string]::Equals(
                [string]$exportManifest.wpaProfileSha256,
                [string]$capture.profiles.wpaSha256,
                [StringComparison]::OrdinalIgnoreCase) `
            -and (Test-HashBinding `
                $exportManifest.wpaProfile `
                $exportManifest.wpaProfileSha256) `
            -and (Test-HashBinding `
                $exportManifest.exporter `
                $exportManifest.exporterSha256)
    } catch {
        $exportBindingValid = $false
    }
}
$manifestIntegrity = $manifestSchemaValid `
    -and $captureHeaderValid `
    -and $executableHashStable `
    -and $timeRangeValid `
    -and $etlHashValid `
    -and $stdoutHashValid `
    -and $stderrHashValid `
    -and $profileHashesValid `
    -and $runtimeBoundaryValid `
    -and $expectedWorkloadValid `
    -and $exportManifestHashValid `
    -and $exportBindingValid

if ([string]::IsNullOrWhiteSpace($RuntimeEvidenceJson) `
    -and $capture.runtimeEvidence `
    -and $capture.runtimeEvidence.path) {
    $RuntimeEvidenceJson = [string]$capture.runtimeEvidence.path
}
$runtimeEvidence = $null
$runtimeEvidencePath = $null
$runtimeEvidenceHash = $null
$runtimeManifestBindingValid = $false
$runtimeEvidenceErrors = New-Object System.Collections.Generic.List[string]
if (-not $captureProcessIdValid) {
    $runtimeEvidenceErrors.Add(
        "capture manifest processId must be a positive JSON integer")
}
if (-not $captureExitCodeValid -or $capture.exitCode -ne 0) {
    $runtimeEvidenceErrors.Add(
        "capture workload must exit successfully before it can emit L2 evidence")
}
if (-not $captureNonceValid) {
    $runtimeEvidenceErrors.Add(
        "capture manifest qualification nonce is not a canonical JSON string")
}
if (-not $expectedWorkloadValid) {
    $runtimeEvidenceErrors.Add(
        "capture manifest expected workload is not a canonical schema-v3 tuple")
}
$external = [uint64]0
$identity = [uint64]0
$bound = [uint64]0
$desktopPresentFrames = [uint64]0
$measuredSourcePresentations = [uint64]0
$measuredDesktopPresentFrames = [uint64]0
$captureSurfaceWidth = [uint32]0
$captureSurfaceHeight = [uint32]0
$logicalSourceWidth = [uint32]0
$logicalSourceHeight = [uint32]0
$sourceX = [uint32]0
$sourceY = [uint32]0
$sourceWidth = [uint32]0
$sourceHeight = [uint32]0
$outputWidth = [uint32]0
$outputHeight = [uint32]0
$tuple = $null
$tupleSha256Valid = $false
if ([string]::IsNullOrWhiteSpace($RuntimeEvidenceJson) `
    -or -not (Test-Path -LiteralPath $RuntimeEvidenceJson -PathType Leaf)) {
    $runtimeEvidenceErrors.Add("runtime L2 evidence JSON is absent")
} else {
    $runtimeEvidencePath = (Resolve-Path -LiteralPath $RuntimeEvidenceJson).Path
    $runtimeEvidenceHash = (Get-FileHash `
        -LiteralPath $runtimeEvidencePath -Algorithm SHA256).Hash
    try {
        $runtimeEvidence = Get-Content `
            -LiteralPath $runtimeEvidencePath -Raw -Encoding UTF8 |
            ConvertFrom-Json
    } catch {
        $runtimeEvidenceErrors.Add("runtime evidence is not valid JSON")
    }

    if (-not $capture.runtimeEvidence `
        -or $capture.runtimeEvidence.path -isnot [string] `
        -or [string]::IsNullOrWhiteSpace(
            [string]$capture.runtimeEvidence.path) `
        -or $capture.runtimeEvidence.sha256 -isnot [string] `
        -or [string]$capture.runtimeEvidence.sha256 `
            -notmatch '^[0-9a-fA-F]{64}$' `
        -or $null -ne $capture.runtimeEvidenceRejection) {
        $runtimeEvidenceErrors.Add(
            "capture manifest does not bind runtime evidence")
    } else {
        $runtimeManifestBindingValid = [string]::Equals(
            $runtimeEvidenceHash,
            [string]$capture.runtimeEvidence.sha256,
            [StringComparison]::OrdinalIgnoreCase) `
            -and (Test-HashBinding `
                $capture.runtimeEvidence.path `
                $capture.runtimeEvidence.sha256)
        if (-not $runtimeManifestBindingValid) {
            $runtimeEvidenceErrors.Add(
                "runtime evidence does not match the capture-manifest hash binding")
        }
    }

    if ($runtimeEvidence) {
        try {
            $schemaVersion = Read-RequiredJsonUInt64 `
                $runtimeEvidence "schemaVersion" $runtimeEvidenceErrors
            $evidenceLevel = Read-RequiredJsonUInt64 `
                $runtimeEvidence "evidenceLevel" $runtimeEvidenceErrors
            $evidenceName = Read-RequiredJsonString `
                $runtimeEvidence "evidenceName" $runtimeEvidenceErrors
            $runtimePid = Read-RequiredJsonUInt64 `
                $runtimeEvidence "processId" $runtimeEvidenceErrors
            $executableSha256 = Read-RequiredJsonString `
                $runtimeEvidence "executableSha256" $runtimeEvidenceErrors
            $qualificationRunNonce = Read-RequiredJsonString `
                $runtimeEvidence "qualificationRunNonce" $runtimeEvidenceErrors
            $topLevelAdapterLuid = Read-RequiredJsonString `
                $runtimeEvidence "adapterLuid" $runtimeEvidenceErrors
            $countersScope = Read-RequiredJsonString `
                $runtimeEvidence "countersScope" $runtimeEvidenceErrors
            $declaredTupleSha256 = Read-RequiredJsonString `
                $runtimeEvidence "tupleSha256" $runtimeEvidenceErrors
            $tuple = Get-RequiredJsonProperty `
                $runtimeEvidence "qualificationTuple" $runtimeEvidenceErrors

            if ($schemaVersion -ne 2) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence schemaVersion must be exactly 2")
            }
            if ($evidenceLevel -ne 2) {
                $runtimeEvidenceErrors.Add("runtime evidenceLevel must be 2")
            }
            if ($evidenceName -cne
                    "l2_external_texture_identity_and_encoder_bind") {
                $runtimeEvidenceErrors.Add(
                    "runtime evidenceName does not identify canonical L2 evidence")
            }
            if ($runtimePid -eq 0 -or $runtimePid -gt [uint32]::MaxValue `
                -or -not $captureProcessIdValid `
                -or $runtimePid -ne [uint64]$captureProcessId) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence PID does not match the trace workload")
            }
            if ($executableSha256 -notmatch '^[0-9a-fA-F]{64}$' `
                -or -not [string]::Equals(
                    $executableSha256,
                    [string]$capture.executableSha256,
                    [StringComparison]::OrdinalIgnoreCase)) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence executable hash does not match")
            }
            if ($qualificationRunNonce -notmatch '^[0-9a-f]{32}$' `
                -or [string]$capture.qualificationRunNonce `
                    -cne $qualificationRunNonce) {
                $runtimeEvidenceErrors.Add(
                    "runtime qualification nonce does not match the capture manifest")
            }
            if ($countersScope -ne `
                    "capture_encoder_instance_lifetime_including_warmup") {
                $runtimeEvidenceErrors.Add(
                    "runtime counters do not cover capture/encoder lifetime including warmup")
            }

            $adapterLuid = Read-RequiredJsonString `
                $tuple "adapterLuid" $runtimeEvidenceErrors
            $captureBackend = Read-RequiredJsonString `
                $tuple "captureBackend" $runtimeEvidenceErrors
            $captureTarget = Read-RequiredJsonString `
                $tuple "captureTarget" $runtimeEvidenceErrors
            $rotation = Read-RequiredJsonString `
                $tuple "rotation" $runtimeEvidenceErrors
            $outputIdentity = Read-RequiredJsonString `
                $tuple "outputIdentity" $runtimeEvidenceErrors
            $transformBackend = Read-RequiredJsonString `
                $tuple "transformBackend" $runtimeEvidenceErrors
            $format = Read-RequiredJsonString `
                $tuple "format" $runtimeEvidenceErrors
            $colorSpace = Read-RequiredJsonString `
                $tuple "colorSpace" $runtimeEvidenceErrors
            $hdr10StaticMetadata = Read-RequiredJsonBool `
                $tuple "hdr10StaticMetadata" $runtimeEvidenceErrors
            $captureSurfaceWidth64 = Read-RequiredJsonUInt64 `
                $tuple "captureSurfaceWidth" $runtimeEvidenceErrors
            $captureSurfaceHeight64 = Read-RequiredJsonUInt64 `
                $tuple "captureSurfaceHeight" $runtimeEvidenceErrors
            $logicalSourceWidth64 = Read-RequiredJsonUInt64 `
                $tuple "logicalSourceWidth" $runtimeEvidenceErrors
            $logicalSourceHeight64 = Read-RequiredJsonUInt64 `
                $tuple "logicalSourceHeight" $runtimeEvidenceErrors
            $sourceX64 = Read-RequiredJsonUInt64 `
                $tuple "sourceX" $runtimeEvidenceErrors
            $sourceY64 = Read-RequiredJsonUInt64 `
                $tuple "sourceY" $runtimeEvidenceErrors
            $sourceWidth64 = Read-RequiredJsonUInt64 `
                $tuple "sourceWidth" $runtimeEvidenceErrors
            $sourceHeight64 = Read-RequiredJsonUInt64 `
                $tuple "sourceHeight" $runtimeEvidenceErrors
            $outputWidth64 = Read-RequiredJsonUInt64 `
                $tuple "outputWidth" $runtimeEvidenceErrors
            $outputHeight64 = Read-RequiredJsonUInt64 `
                $tuple "outputHeight" $runtimeEvidenceErrors
            $codec = Read-RequiredJsonString `
                $tuple "codec" $runtimeEvidenceErrors
            $encoderInputSubtype = Read-RequiredJsonString `
                $tuple "encoderInputSubtype" $runtimeEvidenceErrors
            $encoderOutputSubtype = Read-RequiredJsonString `
                $tuple "encoderOutputSubtype" $runtimeEvidenceErrors
            $encoderMft = Read-RequiredJsonString `
                $tuple "encoderMft" $runtimeEvidenceErrors
            $mftClsid = Read-RequiredJsonString `
                $tuple "encoderMftClsid" $runtimeEvidenceErrors
            $mftFriendlyName = Read-RequiredJsonString `
                $tuple "encoderMftFriendlyName" $runtimeEvidenceErrors

            if ($adapterLuid -notmatch '^0x[0-9A-F]{8}:0x[0-9A-F]{8}$' `
                -or $topLevelAdapterLuid -cne $adapterLuid) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence adapter LUID is not canonical or consistent")
            }
            if ($captureBackend -notin @("wgc", "desktop_duplication")) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence has an unsupported capture backend")
            }
            if ($captureTarget -notin @("window", "monitor")) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence has an unsupported capture target")
            }
            if (($captureBackend -eq "desktop_duplication" `
                    -and ($captureTarget -ne "monitor" `
                        -or $rotation -notin @(
                            "identity", "rotate90", "rotate180", "rotate270") `
                        -or $outputIdentity -eq "not_applicable")) `
                -or ($captureBackend -eq "wgc" `
                    -and ($rotation -ne "not_applicable" `
                        -or $outputIdentity -ne "not_applicable"))) {
                $runtimeEvidenceErrors.Add(
                    "runtime capture backend/target/rotation tuple is inconsistent")
            }
            $outputIdentityParsed = $false
            $identityOutputIndex = [uint32]0
            $identityLeft = [int32]0
            $identityTop = [int32]0
            $identityRight = [int32]0
            $identityBottom = [int32]0
            $identityLogicalWidth = [uint32]0
            $identityLogicalHeight = [uint32]0
            if ($captureBackend -eq "desktop_duplication") {
                $identityMatch = [regex]::Match(
                    $outputIdentity,
                    '^([0-9]+):(\\\\\.\\DISPLAY[0-9]+):(-?[0-9]+),(-?[0-9]+),(-?[0-9]+),(-?[0-9]+):([0-9]+)x([0-9]+)$',
                    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
                if ($identityMatch.Success) {
                    try {
                        $identityOutputIndex = [uint32]$identityMatch.Groups[1].Value
                        $identityLeft = [int32]$identityMatch.Groups[3].Value
                        $identityTop = [int32]$identityMatch.Groups[4].Value
                        $identityRight = [int32]$identityMatch.Groups[5].Value
                        $identityBottom = [int32]$identityMatch.Groups[6].Value
                        $identityLogicalWidth =
                            [uint32]$identityMatch.Groups[7].Value
                        $identityLogicalHeight =
                            [uint32]$identityMatch.Groups[8].Value
                        $canonicalIndex = $identityOutputIndex.ToString(
                            [Globalization.CultureInfo]::InvariantCulture)
                        $outputIdentityParsed = $identityMatch.Groups[1].Value `
                                -ceq $canonicalIndex `
                            -and $identityRight -gt $identityLeft `
                            -and $identityBottom -gt $identityTop `
                            -and $identityLogicalWidth -gt 0 `
                            -and $identityLogicalHeight -gt 0
                    } catch {
                        $outputIdentityParsed = $false
                    }
                }
                if (-not $outputIdentityParsed) {
                    $runtimeEvidenceErrors.Add(
                        "runtime Desktop Duplication output identity is not canonical")
                }
            }
            if ($transformBackend `
                -notin @("deterministic_planar", "video_processor")) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence has an unsupported transform backend")
            }
            if ($captureBackend -eq "desktop_duplication" `
                -and $rotation -ne "identity" `
                -and $transformBackend -ne "video_processor") {
                $runtimeEvidenceErrors.Add(
                    "rotated Desktop Duplication must use video_processor")
            }
            if ($format -notin @("NV12", "P010")) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence format must be NV12 or P010")
            }
            if ($colorSpace -ne "YCBCR_STUDIO_G22_LEFT_P709" `
                -or $hdr10StaticMetadata) {
                $runtimeEvidenceErrors.Add(
                    "runtime v2 supports only explicitly SDR BT.709 planar evidence")
            }
            $dimensionValues = @(
                $captureSurfaceWidth64, $captureSurfaceHeight64,
                $logicalSourceWidth64, $logicalSourceHeight64,
                $sourceX64, $sourceY64, $sourceWidth64, $sourceHeight64,
                $outputWidth64, $outputHeight64)
            if (@($dimensionValues | Where-Object {
                    $_ -gt [uint32]::MaxValue
                }).Count -ne 0) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence geometry exceeds uint32 range")
            } else {
                $captureSurfaceWidth = [uint32]$captureSurfaceWidth64
                $captureSurfaceHeight = [uint32]$captureSurfaceHeight64
                $logicalSourceWidth = [uint32]$logicalSourceWidth64
                $logicalSourceHeight = [uint32]$logicalSourceHeight64
                $sourceX = [uint32]$sourceX64
                $sourceY = [uint32]$sourceY64
                $sourceWidth = [uint32]$sourceWidth64
                $sourceHeight = [uint32]$sourceHeight64
                $outputWidth = [uint32]$outputWidth64
                $outputHeight = [uint32]$outputHeight64
            }
            if ($captureBackend -eq "desktop_duplication" `
                -and $outputIdentityParsed) {
                $rectWidth = [int64]$identityRight - [int64]$identityLeft
                $rectHeight = [int64]$identityBottom - [int64]$identityTop
                $expectedSurfaceWidth = if ($rotation -in @(
                        "rotate90", "rotate270")) {
                    $logicalSourceHeight
                } else {
                    $logicalSourceWidth
                }
                $expectedSurfaceHeight = if ($rotation -in @(
                        "rotate90", "rotate270")) {
                    $logicalSourceWidth
                } else {
                    $logicalSourceHeight
                }
                if ($identityLogicalWidth -ne $logicalSourceWidth `
                    -or $identityLogicalHeight -ne $logicalSourceHeight `
                    -or $rectWidth -ne $logicalSourceWidth `
                    -or $rectHeight -ne $logicalSourceHeight `
                    -or $captureSurfaceWidth -ne $expectedSurfaceWidth `
                    -or $captureSurfaceHeight -ne $expectedSurfaceHeight) {
                    $runtimeEvidenceErrors.Add(
                        "runtime Desktop Duplication output identity and rotated surface geometry disagree")
                }
            } elseif ($captureBackend -eq "wgc" `
                -and ($captureSurfaceWidth -ne $logicalSourceWidth `
                    -or $captureSurfaceHeight -ne $logicalSourceHeight)) {
                $runtimeEvidenceErrors.Add(
                    "runtime WGC capture and logical source geometry disagree")
            }
            if ($captureSurfaceWidth -eq 0 -or $captureSurfaceHeight -eq 0 `
                -or $logicalSourceWidth -eq 0 -or $logicalSourceHeight -eq 0 `
                -or $sourceWidth -eq 0 -or $sourceHeight -eq 0 `
                -or $outputWidth -eq 0 -or $outputHeight -eq 0) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence dimensions must be nonzero")
            }
            if ([uint64]$sourceX + $sourceWidth -gt $logicalSourceWidth `
                -or [uint64]$sourceY + $sourceHeight -gt $logicalSourceHeight) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence source region exceeds the capture surface")
            }
            if (($outputWidth -band 1) -ne 0 `
                -or ($outputHeight -band 1) -ne 0) {
                $runtimeEvidenceErrors.Add(
                    "runtime NV12/P010 output dimensions must be even")
            }
            if ($codec -notin @("h264", "hevc", "av1")) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence has an unsupported codec")
            }
            $expectedInputSubtype = if ($format -eq "P010") {
                "MFVideoFormat_P010"
            } else {
                "MFVideoFormat_NV12"
            }
            $expectedOutputSubtype = @{
                h264 = "MFVideoFormat_H264"
                hevc = "MFVideoFormat_HEVC"
                av1 = "MFVideoFormat_AV1"
            }[$codec]
            if ($encoderInputSubtype -ne $expectedInputSubtype `
                -or $encoderOutputSubtype -ne $expectedOutputSubtype) {
                $runtimeEvidenceErrors.Add(
                    "runtime encoder media subtypes do not match format/codec")
            }
            if ($mftClsid -notmatch `
                    '^\{[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\}$' `
                -or $mftClsid -eq `
                    '{00000000-0000-0000-0000-000000000000}') {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence lacks a non-null canonical encoder MFT CLSID")
            }
            if ($encoderMft -ne "$mftFriendlyName $mftClsid") {
                $runtimeEvidenceErrors.Add(
                    "runtime encoder MFT combined identity is inconsistent")
            }
            $expected = $capture.expectedWorkload
            if ($null -eq $expected `
                -or [string]$expected.adapterLuid -cne $adapterLuid `
                -or [string]$expected.captureBackend -cne $captureBackend `
                -or [string]$expected.captureTarget -cne $captureTarget `
                -or [string]$expected.rotation -cne $rotation `
                -or [string]$expected.transformBackend -cne $transformBackend `
                -or [string]$expected.format -cne $format `
                -or [string]$expected.codec -cne $codec `
                -or [uint64]$expected.sourceWidth -ne $sourceWidth `
                -or [uint64]$expected.sourceHeight -ne $sourceHeight `
                -or [uint64]$expected.outputWidth -ne $outputWidth `
                -or [uint64]$expected.outputHeight -ne $outputHeight) {
                $runtimeEvidenceErrors.Add(
                    "runtime tuple does not match the pre-bound expected workload")
            }
            if (($captureBackend -eq "desktop_duplication" `
                    -and (-not (Test-JsonUInt32Type $expected.outputIndex) `
                        -or -not $outputIdentityParsed `
                        -or $identityOutputIndex -ne
                            [uint64]$expected.outputIndex)) `
                -or ($captureBackend -eq "wgc" `
                    -and $null -ne $expected.outputIndex)) {
                $runtimeEvidenceErrors.Add(
                    "runtime output identity does not match expected output index")
            }

            $canonicalTuple = @(
                "v2", $adapterLuid, $captureBackend, $captureTarget, $rotation,
                $outputIdentity,
                $transformBackend, $format, $colorSpace,
                ("{0}x{1}" -f $captureSurfaceWidth, $captureSurfaceHeight),
                ("{0},{1},{2},{3}" -f $sourceX, $sourceY,
                    $sourceWidth, $sourceHeight),
                ("{0}x{1}" -f $outputWidth, $outputHeight),
                $codec, $encoderInputSubtype, $encoderOutputSubtype, $mftClsid
            ) -join "|"
            $actualTupleSha256 = Get-TextSha256 $canonicalTuple
            $tupleSha256Valid = $declaredTupleSha256 `
                    -match '^[0-9a-f]{64}$' `
                -and [string]::Equals(
                    $actualTupleSha256,
                    $declaredTupleSha256,
                    [StringComparison]::Ordinal)
            if (-not $tupleSha256Valid) {
                $runtimeEvidenceErrors.Add(
                    "runtime qualification tuple hash is invalid")
            }

            $matchingTransforms = Read-RequiredJsonUInt64 `
                $runtimeEvidence "encoderMatchingTransformCount" `
                $runtimeEvidenceErrors
            $external = Read-RequiredJsonUInt64 `
                $runtimeEvidence "externalSubmissions" $runtimeEvidenceErrors
            $identity = Read-RequiredJsonUInt64 `
                $runtimeEvidence "externalIdentityVerifiedSubmissions" `
                $runtimeEvidenceErrors
            $bound = Read-RequiredJsonUInt64 `
                $runtimeEvidence "externalVideoEncoderBoundSubmissions" `
                $runtimeEvidenceErrors
            $copied = Read-RequiredJsonUInt64 `
                $runtimeEvidence "encoderCopiedSubmissions" $runtimeEvidenceErrors
            $direct = Read-RequiredJsonUInt64 `
                $runtimeEvidence "encoderDirectSubmissions" $runtimeEvidenceErrors
            $ingressCopies = Read-RequiredJsonUInt64 `
                $runtimeEvidence "producerIngressCopySubmissions" `
                $runtimeEvidenceErrors
            $ingressTransforms = Read-RequiredJsonUInt64 `
                $runtimeEvidence "producerIngressTransformSubmissions" `
                $runtimeEvidenceErrors
            $busPublished = Read-RequiredJsonUInt64 `
                $runtimeEvidence "busPublishedFrames" $runtimeEvidenceErrors
            $busCopied = Read-RequiredJsonUInt64 `
                $runtimeEvidence "busCopiedPublishes" $runtimeEvidenceErrors
            $busDirect = Read-RequiredJsonUInt64 `
                $runtimeEvidence "busDirectPublishes" $runtimeEvidenceErrors
            $desktopPresentFrames = Read-RequiredJsonUInt64 `
                $runtimeEvidence "desktopPresentFrames" $runtimeEvidenceErrors
            $measuredSourcePresentations = Read-RequiredJsonUInt64 `
                $runtimeEvidence "measuredSourcePresentations" `
                $runtimeEvidenceErrors
            $measuredDesktopPresentFrames = Read-RequiredJsonUInt64 `
                $runtimeEvidence "measuredDesktopPresentFrames" `
                $runtimeEvidenceErrors
            $codecMismatches = Read-RequiredJsonUInt64 `
                $runtimeEvidence "packetCodecMismatches" $runtimeEvidenceErrors
            $measuredFrames = Read-RequiredJsonUInt64 `
                $runtimeEvidence "measuredConsumerFrames" $runtimeEvidenceErrors
            $measuredDurationMs = Read-RequiredJsonUInt64 `
                $runtimeEvidence "measuredDurationMs" $runtimeEvidenceErrors
            $warmupMs = Read-RequiredJsonUInt64 `
                $runtimeEvidence "warmupMs" $runtimeEvidenceErrors
            $explicitCopyFree = Read-RequiredJsonBool `
                $runtimeEvidence "fluxcapExplicitCopyFree" $runtimeEvidenceErrors
            $mftObservable = Read-RequiredJsonBool `
                $runtimeEvidence "mftInternalCopyObservable" $runtimeEvidenceErrors
            $driverObservable = Read-RequiredJsonBool `
                $runtimeEvidence "driverPrivateSurfaceCopyObservable" `
                $runtimeEvidenceErrors
            $dmaObservable = Read-RequiredJsonBool `
                $runtimeEvidence "hardwareDmaCopyObservable" $runtimeEvidenceErrors
            if ($external -eq 0 -or $identity -ne $external `
                -or $bound -ne $external -or $direct -ne $external) {
                $runtimeEvidenceErrors.Add(
                    "external/identity/bind/direct lifetime counters are inconsistent")
            }
            if ($matchingTransforms -eq 0 -or $measuredFrames -eq 0 `
                -or $measuredDurationMs -eq 0) {
                $runtimeEvidenceErrors.Add(
                    "runtime encoder selection or measured interval is empty")
            }
            if ($copied -ne 0 -or $ingressCopies -ne 0 `
                -or $busCopied -ne 0 -or -not $explicitCopyFree) {
                $runtimeEvidenceErrors.Add(
                    "FluxCap reported an explicit copy in the lifetime scope")
            }
            if ($ingressTransforms -eq 0 -or $busPublished -eq 0 `
                -or $busDirect -ne $busPublished) {
                $runtimeEvidenceErrors.Add(
                    "runtime direct planar producer counters are incomplete")
            }
            if (($captureBackend -eq "wgc" `
                    -and ($desktopPresentFrames -ne 0 `
                        -or $measuredDesktopPresentFrames -ne 0 `
                        -or $measuredSourcePresentations -eq 0)) `
                -or ($captureBackend -eq "desktop_duplication" `
                    -and ($desktopPresentFrames -eq 0 `
                        -or $measuredSourcePresentations -eq 0 `
                        -or $measuredDesktopPresentFrames -eq 0 `
                        -or $measuredDesktopPresentFrames `
                            -gt $desktopPresentFrames `
                        -or $desktopPresentFrames -gt $busPublished))) {
                $runtimeEvidenceErrors.Add(
                    "runtime presentation counters are inconsistent with the capture backend")
            }
            if ($codecMismatches -ne 0) {
                $runtimeEvidenceErrors.Add(
                    "runtime encoder packet codec did not match the requested tuple")
            }
            if ($mftObservable -or $driverObservable -or $dmaObservable) {
                $runtimeEvidenceErrors.Add(
                    "runtime evidence must keep all private copy boundaries unobservable")
            }
        } catch {
            $runtimeEvidenceErrors.Add(
                "runtime evidence contains invalid field types or counts: $($_.Exception.Message)")
        }
    }
}
$runtimeL2Valid = $runtimeEvidenceErrors.Count -eq 0

$providerCoverage = $providerD3D.Count -gt 0 `
    -and $providerDxgi.Count -gt 0 `
    -and $providerDxgKrnl.Count -gt 0 `
    -and $providerMf.Count -gt 0 `
    -and $targetEvents.Count -gt 0 `
    -and $targetD3D.Count -gt 0 `
    -and $targetDxgi.Count -gt 0 `
    -and $targetMf.Count -gt 0
$automatedScreen = $manifestIntegrity `
    -and $providerCoverage `
    -and $lostEventCandidates.Count -eq 0 `
    -and $capture.exitCode -eq 0
$manualPass = $ConfirmReviewedWorkloadInterval `
    -and $ConfirmNoFullFrameD3D11Copies `
    -and $ConfirmNoCopyEnginePackets `
    -and $ConfirmMediaFoundationActivity `
    -and $ConfirmNoLostEvents `
    -and -not [string]::IsNullOrWhiteSpace($Reviewer)
$passed = $automatedScreen -and $runtimeL2Valid -and $manualPass
$status = if (-not $automatedScreen) {
    "failed-screening"
} elseif (-not $runtimeL2Valid) {
    "insufficient-runtime-l2-evidence"
} elseif ($passed) {
    "passed"
} else {
    "needs-manual-wpa-review"
}

$result = [ordered]@{
    schemaVersion = 2
    status = $status
    evidenceLevel = if ($passed) { 3 } elseif ($runtimeL2Valid) { 2 } else { 0 }
    evidenceName = if ($passed) {
        "l3_etw_no_observed_full_frame_copy"
    } elseif ($runtimeL2Valid) {
        "l2_external_texture_identity_and_encoder_bind"
    } else {
        "none"
    }
    captureManifest = (Resolve-Path -LiteralPath $Manifest).Path
    captureManifestSha256 = (Get-FileHash `
        -LiteralPath $Manifest -Algorithm SHA256).Hash
    etlSha256 = [string]$capture.etlSha256
    genericEventsCsv = (Resolve-Path -LiteralPath $GenericEventsCsv).Path
    genericEventsCsvSha256 = (Get-FileHash `
        -LiteralPath $GenericEventsCsv -Algorithm SHA256).Hash
    runtimeEvidenceJson = if ($runtimeEvidencePath) {
        $runtimeEvidencePath
    } else {
        $null
    }
    runtimeEvidenceJsonSha256 = $runtimeEvidenceHash
    processId = $capture.processId
    workloadExitCode = $capture.exitCode
    manifestIntegrity = [ordered]@{
        complete = $manifestIntegrity
        schemaValid = $manifestSchemaValid
        captureHeaderValid = $captureHeaderValid
        processIdValid = $captureProcessIdValid
        exitCodeTypeValid = $captureExitCodeValid
        qualificationNonceValid = $captureNonceValid
        executableHashStable = $executableHashStable
        timeRangeValid = $timeRangeValid
        etlHashValid = $etlHashValid
        stdoutHashValid = $stdoutHashValid
        stderrHashValid = $stderrHashValid
        profileHashesValid = $profileHashesValid
        runningAnalyzerHashValid = $runningAnalyzerHashValid
        runtimeBoundaryValid = $runtimeBoundaryValid
        expectedWorkloadShapeValid = $expectedWorkloadShapeValid
        expectedWorkloadHashValid = $expectedWorkloadHashValid
        exportManifestHashValid = $exportManifestHashValid
        exportBindingValid = $exportBindingValid
    }
    runtimeL2 = [ordered]@{
        complete = $runtimeL2Valid
        manifestHashBindingValid = $runtimeManifestBindingValid
        validationErrors = @($runtimeEvidenceErrors)
        externalSubmissions = $external
        identityVerifiedSubmissions = $identity
        videoEncoderBoundSubmissions = $bound
        desktopPresentFrames = $desktopPresentFrames
        measuredSourcePresentations = $measuredSourcePresentations
        measuredDesktopPresentFrames = $measuredDesktopPresentFrames
    }
    qualificationScope = if ($runtimeEvidence) {
        [ordered]@{
            tupleSha256 = [string]$runtimeEvidence.tupleSha256
            adapterLuid = [string]$tuple.adapterLuid
            captureBackend = [string]$tuple.captureBackend
            captureTarget = [string]$tuple.captureTarget
            rotation = [string]$tuple.rotation
            outputIdentity = [string]$tuple.outputIdentity
            transformBackend = [string]$tuple.transformBackend
            format = [string]$tuple.format
            colorSpace = [string]$tuple.colorSpace
            captureSurfaceWidth = $captureSurfaceWidth
            captureSurfaceHeight = $captureSurfaceHeight
            logicalSourceWidth = $logicalSourceWidth
            logicalSourceHeight = $logicalSourceHeight
            sourceX = $sourceX
            sourceY = $sourceY
            sourceWidth = $sourceWidth
            sourceHeight = $sourceHeight
            outputWidth = $outputWidth
            outputHeight = $outputHeight
            codec = [string]$tuple.codec
            encoderInputSubtype = [string]$tuple.encoderInputSubtype
            encoderOutputSubtype = [string]$tuple.encoderOutputSubtype
            encoderMft = [string]$tuple.encoderMft
            encoderMftClsid = [string]$tuple.encoderMftClsid
            encoderMftFriendlyName =
                [string]$tuple.encoderMftFriendlyName
        }
    } else {
        $null
    }
    providerCoverage = [ordered]@{
        direct3D11Events = $providerD3D.Count
        dxgiEvents = $providerDxgi.Count
        dxgKrnlEvents = $providerDxgKrnl.Count
        mediaFoundationEvents = $providerMf.Count
        targetProcessEvents = $targetEvents.Count
        targetDirect3D11Events = $targetD3D.Count
        targetDxgiEvents = $targetDxgi.Count
        targetMediaFoundationEvents = $targetMf.Count
        complete = $providerCoverage
    }
    candidates = [ordered]@{
        targetD3D11CopyCallsRequiringDimensionReview = $d3dCopyCandidates.Count
        systemWideCopyEngineStringMatches = $copyEngineHeuristicCandidates.Count
        copyEngineStringMatchesAreHeuristicOnly = $true
        traceLostEventIndicators = $lostEventCandidates.Count
    }
    manualReview = [ordered]@{
        reviewedWorkloadInterval = [bool]$ConfirmReviewedWorkloadInterval
        confirmedNoFullFrameD3D11Copies = [bool]$ConfirmNoFullFrameD3D11Copies
        confirmedNoCopyEnginePackets = [bool]$ConfirmNoCopyEnginePackets
        confirmedMediaFoundationActivity = [bool]$ConfirmMediaFoundationActivity
        confirmedNoLostEvents = [bool]$ConfirmNoLostEvents
        reviewer = $Reviewer
        requiredViews = @(
            "Generic Events filtered to target PID; inspect every D3D11 Copy* candidate and its dimensions",
            "GPU Hardware Queue filtered to target process/context and Copy engine",
            "Media Foundation activity bounded to the spawned workload lifetime",
            "Trace diagnostics/event-loss tables"
        )
    }
    observationBoundary = [ordered]@{
        mftInternalCopyObservable = $false
        driverPrivateSurfaceObservable = $false
        hardwareDmaAndCacheObservable = $false
        statement = "L3 means no full-frame Copy API call or attributable Copy-engine packet was observed in the reviewed ETW window; it is not proof that private MFT/driver copies do not exist."
    }
}
$resolvedOutput = [System.IO.Path]::GetFullPath($Output)
$parent = Split-Path -Parent $resolvedOutput
if ($parent) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
$result | ConvertTo-Json -Depth 10 | Set-Content `
    -LiteralPath $resolvedOutput -Encoding UTF8
$result | ConvertTo-Json -Depth 10
if ($passed) { exit 0 }
if (-not $automatedScreen) { exit 1 }
if (-not $runtimeL2Valid) { exit 3 }
exit 2
