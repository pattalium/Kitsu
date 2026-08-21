Set-StrictMode -Version Latest

$script:KitsuAcceptanceSchema = 'kitsu.android-physical-acceptance.v3'
$script:KitsuCaseSchema = 'kitsu.android-physical-case.v2'
$script:KitsuReliabilityStartSchema = 'kitsu.android-reliability-start.v2'
$script:KitsuReliabilitySampleSchema = 'kitsu.android-reliability-sample.v2'
$script:KitsuReliabilityCompletionSchema = 'kitsu.android-reliability-completion.v2'
$script:KitsuMinimumReliabilitySeconds = 24 * 60 * 60
$script:KitsuMinimumReliabilitySampleCoverageRatio = 0.90
$script:KitsuMinimumReliabilityHealthRatio = 0.99
$script:KitsuMaximumConsecutiveUnhealthySamples = 5
$script:KitsuMinimumTerminalHealthySamples = 5
$script:KitsuGatewayProtocolVersion = 1
$script:KitsuRepositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..\..\..\..')
)

# Evidence is deliberately more restrictive than application logs. These
# patterns catch labelled credentials and private-key material without
# rejecting ordinary statements such as "the credential field was absent".
$script:KitsuSecretPatterns = @(
    '(?im)-----BEGIN (?:[A-Z0-9]+ )?PRIVATE KEY-----',
    '(?im)\bAuthorization\s*:\s*(?:Bearer|Basic)\s+\S+',
    '(?im)\bBearer\s+[A-Za-z0-9._~+/=-]{16,}',
    '(?im)\b(?:claim_token|access_token|refresh_token|id_token|client_secret|controller_root|private_key|wifi_passphrase|wifi_password|raw_ble_envelope|authorization_header)\b["'']?\s*[:=]\s*["'']?(?!null\b|redacted\b|absent\b|not-recorded\b|not_recorded\b)[^\s,}]+',
    '(?im)\b(?:password|passphrase)\b["'']?\s*[:=]\s*["'']?(?!null\b|redacted\b|absent\b|not-recorded\b|not_recorded\b)[^\s,}]+'
)

function Get-KitsuFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Test-KitsuPathLexicallyWithin {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$CandidatePath
    )
    $base = (Get-KitsuFullPath -Path $BaseDirectory).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
    $candidate = Get-KitsuFullPath -Path $CandidatePath
    $comparison = if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        [StringComparison]::OrdinalIgnoreCase
    } else {
        [StringComparison]::Ordinal
    }
    $prefix = $base + [System.IO.Path]::DirectorySeparatorChar
    return $candidate.Equals($base, $comparison) -or $candidate.StartsWith($prefix, $comparison)
}

function Assert-KitsuNoReparsePoints {
    param([Parameter(Mandatory = $true)][string]$Path)

    $current = Get-KitsuFullPath -Path $Path
    while (-not (Test-Path -LiteralPath $current)) {
        $parent = [System.IO.Path]::GetDirectoryName($current)
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $current) { break }
        $current = $parent
    }
    while (Test-Path -LiteralPath $current) {
        $item = Get-Item -Force -LiteralPath $current
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse points, junctions, and symbolic links are forbidden in evidence paths: $current"
        }
        $parent = [System.IO.Path]::GetDirectoryName($current)
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $current) { break }
        $current = $parent
    }
}

function Assert-KitsuEvidenceOutsideRepository {
    param([Parameter(Mandatory = $true)][string]$EvidenceDirectory)

    $evidence = Get-KitsuFullPath -Path $EvidenceDirectory
    $repository = Get-KitsuFullPath -Path $script:KitsuRepositoryRoot
    if ((Test-KitsuPathLexicallyWithin -BaseDirectory $repository -CandidatePath $evidence) -or
        (Test-KitsuPathLexicallyWithin -BaseDirectory $evidence -CandidatePath $repository)) {
        throw 'Physical acceptance evidence must be stored outside and disjoint from the source repository.'
    }
    Assert-KitsuNoReparsePoints -Path $evidence
    return $evidence
}

function Assert-KitsuEvidenceTreeNoReparsePoints {
    param([Parameter(Mandatory = $true)][string]$EvidenceDirectory)

    Assert-KitsuNoReparsePoints -Path $EvidenceDirectory
    if (-not (Test-Path -LiteralPath $EvidenceDirectory -PathType Container)) { return }
    $pending = New-Object System.Collections.Generic.Queue[string]
    $pending.Enqueue((Get-KitsuFullPath -Path $EvidenceDirectory))
    while ($pending.Count -gt 0) {
        $directory = $pending.Dequeue()
        foreach ($child in @(Get-ChildItem -Force -LiteralPath $directory)) {
            if (($child.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Evidence trees may not contain reparse points, junctions, or symbolic links: $($child.FullName)"
            }
            if ($child.PSIsContainer) { $pending.Enqueue($child.FullName) }
        }
    }
}

function Assert-KitsuPathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$CandidatePath
    )
    $candidate = Get-KitsuFullPath -Path $CandidatePath
    if (-not (Test-KitsuPathLexicallyWithin -BaseDirectory $BaseDirectory -CandidatePath $candidate)) {
        throw "Path escapes the evidence directory: $candidate"
    }
    Assert-KitsuNoReparsePoints -Path $BaseDirectory
    Assert-KitsuNoReparsePoints -Path $candidate
    return $candidate
}

function Get-KitsuSha256 {
    param([Parameter(Mandatory = $true)][string]$LiteralPath)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $LiteralPath).Hash.ToLowerInvariant()
}

function ConvertTo-KitsuUtcDateTimeOffset {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Value
    )
    if ($null -eq $Value) { throw 'UTC timestamp is missing.' }
    if ($Value -is [DateTimeOffset]) {
        return ([DateTimeOffset]$Value).ToUniversalTime()
    }
    if ($Value -is [DateTime]) {
        return ([DateTimeOffset]([DateTime]$Value).ToUniversalTime())
    }
    $parsed = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParse(
        [string]$Value,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AssumeUniversal -bor
            [Globalization.DateTimeStyles]::AdjustToUniversal,
        [ref]$parsed
    )) {
        throw 'UTC timestamp is malformed.'
    }
    return $parsed.ToUniversalTime()
}

function Test-KitsuJsonNumber {
    param([AllowNull()][object]$Value)
    return $Value -is [byte] -or $Value -is [sbyte] -or
        $Value -is [int16] -or $Value -is [uint16] -or
        $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64] -or
        $Value -is [single] -or $Value -is [double] -or $Value -is [decimal]
}

function Assert-KitsuSafeIdentifier {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$Label,
        [string]$Pattern = '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$'
    )
    if ($Value -notmatch $Pattern) {
        throw "$Label has an unsafe format."
    }
}

function Assert-KitsuNoSecrets {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text,
        [string]$Label = 'evidence'
    )
    foreach ($pattern in $script:KitsuSecretPatterns) {
        if ($Text -match $pattern) {
            throw "$Label appears to contain prohibited credential or private-key material."
        }
    }
}

function ConvertTo-KitsuJson {
    param([Parameter(Mandatory = $true)][object]$InputObject)
    $json = $InputObject | ConvertTo-Json -Depth 20 -Compress
    Assert-KitsuNoSecrets -Text $json -Label 'JSON evidence'
    return $json
}

function Write-KitsuNewUtf8Text {
    param(
        [Parameter(Mandatory = $true)][string]$LiteralPath,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text,
        [switch]$SkipSecretScan
    )
    if (-not $SkipSecretScan) {
        Assert-KitsuNoSecrets -Text $Text -Label ([System.IO.Path]::GetFileName($LiteralPath))
    }
    $parent = Split-Path -Parent $LiteralPath
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "Parent directory does not exist: $parent"
    }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    $stream = [System.IO.File]::Open(
        $LiteralPath,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::Read
    )
    try {
        $writer = New-Object System.IO.StreamWriter($stream, $encoding)
        try {
            $writer.Write($Text)
            $writer.Flush()
        } finally {
            $writer.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Write-KitsuNewJson {
    param(
        [Parameter(Mandatory = $true)][string]$LiteralPath,
        [Parameter(Mandatory = $true)][object]$InputObject
    )
    $json = ConvertTo-KitsuJson -InputObject $InputObject
    Write-KitsuNewUtf8Text -LiteralPath $LiteralPath -Text ($json + [Environment]::NewLine)
}

function Read-KitsuJson {
    param([Parameter(Mandatory = $true)][string]$LiteralPath)
    if (-not (Test-Path -LiteralPath $LiteralPath -PathType Leaf)) {
        throw "Required JSON file is missing: $LiteralPath"
    }
    $raw = Get-Content -Raw -LiteralPath $LiteralPath
    Assert-KitsuNoSecrets -Text $raw -Label ([System.IO.Path]::GetFileName($LiteralPath))
    try {
        return $raw | ConvertFrom-Json
    } catch {
        throw "Invalid JSON file: $LiteralPath"
    }
}

function Copy-KitsuNewFile {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )
    $source = [System.IO.File]::OpenRead($SourcePath)
    try {
        $destination = [System.IO.File]::Open(
            $DestinationPath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::Read
        )
        try {
            $source.CopyTo($destination)
            $destination.Flush()
        } finally {
            $destination.Dispose()
        }
    } finally {
        $source.Dispose()
    }
}

function Get-KitsuSanitizedAttachmentInfo {
    param([Parameter(Mandatory = $true)][string]$LiteralPath)

    Assert-KitsuNoReparsePoints -Path $LiteralPath
    $item = Get-Item -Force -LiteralPath $LiteralPath
    if (-not $item.PSIsContainer -and $item.Length -le 100MB) {
        $extension = [System.IO.Path]::GetExtension($item.Name).ToLowerInvariant()
    } else {
        throw "Attachment must be a regular file no larger than 100 MiB: $($item.Name)"
    }
    $textExtensions = @('.txt', '.log', '.json', '.jsonl', '.csv', '.xml', '.md')
    if ($textExtensions -contains $extension) {
        if ($item.Length -gt 5MB) {
            throw "Text attachment exceeds the 5 MiB secret-scan limit: $($item.Name)"
        }
        try {
            $bytes = [System.IO.File]::ReadAllBytes($item.FullName)
            $encoding = New-Object System.Text.UTF8Encoding($false, $true)
            $text = $encoding.GetString($bytes)
        } catch {
            throw "Text attachment is not strict UTF-8: $($item.Name)"
        }
        if ($text.Contains([char]0)) {
            throw "Text attachment contains binary NUL data: $($item.Name)"
        }
        Assert-KitsuNoSecrets -Text $text -Label $item.Name
        return [pscustomobject]@{ extension = $extension; media_type = 'text/utf-8' }
    }

    $allowedBinary = @('.png', '.jpg', '.jpeg', '.gif', '.webp', '.mp4', '.webm')
    if ($allowedBinary -notcontains $extension) {
        throw "Attachment type is not in the sanitized evidence allow-list: $($item.Name)"
    }
    $prefixLength = [Math]::Min(16, [int]$item.Length)
    $prefix = New-Object byte[] $prefixLength
    $stream = [System.IO.File]::OpenRead($item.FullName)
    try {
        [void]$stream.Read($prefix, 0, $prefix.Length)
    } finally {
        $stream.Dispose()
    }
    $isPng = $prefix.Length -ge 8 -and
        ($prefix[0..7] -join ',') -ceq '137,80,78,71,13,10,26,10'
    $isJpeg = $prefix.Length -ge 3 -and $prefix[0] -eq 0xFF -and
        $prefix[1] -eq 0xD8 -and $prefix[2] -eq 0xFF
    $isGif = $prefix.Length -ge 6 -and
        [System.Text.Encoding]::ASCII.GetString($prefix, 0, 6) -in @('GIF87a', 'GIF89a')
    $isWebp = $prefix.Length -ge 12 -and
        [System.Text.Encoding]::ASCII.GetString($prefix, 0, 4) -ceq 'RIFF' -and
        [System.Text.Encoding]::ASCII.GetString($prefix, 8, 4) -ceq 'WEBP'
    $isMp4 = $prefix.Length -ge 12 -and
        [System.Text.Encoding]::ASCII.GetString($prefix, 4, 4) -ceq 'ftyp'
    $isWebm = $prefix.Length -ge 4 -and
        ($prefix[0..3] -join ',') -ceq '26,69,223,163'
    $valid = switch ($extension) {
        '.png' { $isPng }
        '.jpg' { $isJpeg }
        '.jpeg' { $isJpeg }
        '.gif' { $isGif }
        '.webp' { $isWebp }
        '.mp4' { $isMp4 }
        '.webm' { $isWebm }
        default { $false }
    }
    if (-not $valid) {
        throw "Attachment content does not match its allowed media extension: $($item.Name)"
    }
    return [pscustomobject]@{
        extension = $extension
        media_type = switch ($extension) {
            '.png' { 'image/png' }
            '.jpg' { 'image/jpeg' }
            '.jpeg' { 'image/jpeg' }
            '.gif' { 'image/gif' }
            '.webp' { 'image/webp' }
            '.mp4' { 'video/mp4' }
            '.webm' { 'video/webm' }
        }
    }
}

function ConvertTo-KitsuGatewayHealthObservation {
    param(
        [Parameter(Mandatory = $true)][int]$StatusCode,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Content,
        [Parameter(Mandatory = $true)][double]$LatencyMilliseconds,
        [Parameter(Mandatory = $true)][string]$ExpectedGatewayId,
        [Parameter(Mandatory = $true)][ValidateSet('public', 'private')][string]$ExpectedExposure
    )

    $gatewayId = $null
    $deploymentScope = $null
    $protocol = $null
    $reportedStatus = $null
    $backendOnline = $null
    $identityVerified = $false
    if ($StatusCode -ge 200 -and $StatusCode -lt 300) {
        try {
            $document = $Content | ConvertFrom-Json
            $gatewayId = [string]$document.gateway_id
            $deploymentScope = [string]$document.deployment_scope
            if (Test-KitsuJsonNumber -Value $document.protocol) {
                $protocol = [int]$document.protocol
            }
            $reportedStatus = [string]$document.status
            if ($document.backend_online -is [bool]) {
                $backendOnline = [bool]$document.backend_online
            }
            $identityVerified = (
                $gatewayId -ceq $ExpectedGatewayId -and
                $deploymentScope -ceq $ExpectedExposure -and
                $null -ne $protocol -and
                $protocol -eq $script:KitsuGatewayProtocolVersion
            )
        } catch {
            $identityVerified = $false
        }
    }
    return [ordered]@{
        reachable = $StatusCode -ge 200 -and $StatusCode -lt 300
        status_code = $StatusCode
        latency_ms = [Math]::Round($LatencyMilliseconds, 1)
        identity_verified = $identityVerified
        gateway_id = $gatewayId
        deployment_scope = $deploymentScope
        protocol = $protocol
        reported_status = $reportedStatus
        backend_online = $backendOnline
    }
}

function Test-KitsuGatewayHealthObservationAccepted {
    param(
        [Parameter(Mandatory = $true)][object]$Observation,
        [Parameter(Mandatory = $true)][string]$ExpectedGatewayId,
        [Parameter(Mandatory = $true)][ValidateSet('public', 'private')][string]$ExpectedExposure
    )

    try {
        $statusCode = 0
        return $Observation.reachable -is [bool] -and
            [bool]$Observation.reachable -and
            (Test-KitsuJsonNumber -Value $Observation.status_code) -and
            [int]::TryParse([string]$Observation.status_code, [ref]$statusCode) -and
            $statusCode -ge 200 -and $statusCode -lt 300 -and
            $Observation.identity_verified -is [bool] -and
            [bool]$Observation.identity_verified -and
            [string]$Observation.gateway_id -ceq $ExpectedGatewayId -and
            [string]$Observation.deployment_scope -ceq $ExpectedExposure -and
            (Test-KitsuJsonNumber -Value $Observation.protocol) -and
            [int]$Observation.protocol -eq $script:KitsuGatewayProtocolVersion -and
            [string]$Observation.reported_status -ceq 'ok' -and
            $Observation.backend_online -is [bool] -and
            [bool]$Observation.backend_online
    } catch {
        return $false
    }
}

function Invoke-KitsuGatewayHealthProbe {
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][ValidateRange(1, 30)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$ExpectedGatewayId,
        [Parameter(Mandatory = $true)][ValidateSet('public', 'private')][string]$ExpectedExposure
    )

    $timer = [Diagnostics.Stopwatch]::StartNew()
    $statusCode = 0
    $content = ''
    try {
        $response = Invoke-WebRequest -Uri $Uri -Method Get -TimeoutSec $TimeoutSeconds `
            -UseBasicParsing -MaximumRedirection 0
        $statusCode = [int]$response.StatusCode
        $content = [string]$response.Content
    } catch {
        if ($null -ne $_.Exception.Response -and $null -ne $_.Exception.Response.StatusCode) {
            $statusCode = [int]$_.Exception.Response.StatusCode
        }
    } finally {
        $timer.Stop()
    }
    $observation = ConvertTo-KitsuGatewayHealthObservation -StatusCode $statusCode `
        -Content $content -LatencyMilliseconds $timer.Elapsed.TotalMilliseconds `
        -ExpectedGatewayId $ExpectedGatewayId -ExpectedExposure $ExpectedExposure
    $content = $null
    return $observation
}

function Test-KitsuBackendHealthUriAccepted {
    param(
        [Parameter(Mandatory = $true)][string]$CandidateUri,
        [Parameter(Mandatory = $true)][string]$FrozenHealthUri,
        [Parameter(Mandatory = $true)][string]$FrozenBaseUrl
    )

    try {
        $candidate = [Uri]$CandidateUri
        $frozen = [Uri]$FrozenHealthUri
        $base = [Uri]$FrozenBaseUrl
        return $candidate.AbsoluteUri -ceq $frozen.AbsoluteUri -and
            $candidate.Scheme -ceq 'https' -and
            $candidate.Scheme -ceq $base.Scheme -and
            $candidate.Authority -ceq $base.Authority -and
            $candidate.AbsolutePath -ceq '/health/live' -and
            -not $candidate.UserInfo -and -not $candidate.Query -and -not $candidate.Fragment
    } catch {
        return $false
    }
}

function Test-KitsuReliabilityRun {
    param(
        [Parameter(Mandatory = $true)][string]$RunDirectory,
        [Parameter(Mandatory = $true)][string]$AcceptanceRecordPath
    )

    $errors = New-Object System.Collections.Generic.List[string]
    $startPath = Join-Path $RunDirectory 'start.json'
    $samplesPath = Join-Path $RunDirectory 'samples.jsonl'
    $completionPath = Join-Path $RunDirectory 'completion.json'
    try {
        $acceptance = Read-KitsuJson -LiteralPath $AcceptanceRecordPath
        $start = Read-KitsuJson -LiteralPath $startPath
        $completion = Read-KitsuJson -LiteralPath $completionPath
    } catch {
        $errors.Add($_.Exception.Message)
        return [pscustomobject]@{
            valid = $false
            errors = $errors.ToArray()
            sample_count = 0
            elapsed_seconds = 0
            health = [ordered]@{}
        }
    }

    $acceptanceHash = Get-KitsuSha256 -LiteralPath $AcceptanceRecordPath
    $runId = Split-Path -Leaf $RunDirectory
    $validationNow = [DateTimeOffset]::UtcNow
    if ($acceptance.schema -ne $script:KitsuAcceptanceSchema -or
        $acceptance.status -ne 'IN_PROGRESS') {
        $errors.Add('reliability run is not bound to an in-progress physical acceptance record')
    }
    try {
        $acceptanceStarted = ConvertTo-KitsuUtcDateTimeOffset -Value $acceptance.started_at_utc
        if ($acceptanceStarted -gt $validationNow) {
            $errors.Add('acceptance initialization is future-dated')
        }
    } catch {
        $acceptanceStarted = [DateTimeOffset]::MaxValue
        $errors.Add('acceptance initialization timestamp is malformed')
    }
    if ($start.schema -ne $script:KitsuReliabilityStartSchema) {
        $errors.Add('unexpected reliability start schema')
    }
    if ($completion.schema -ne $script:KitsuReliabilityCompletionSchema) {
        $errors.Add('unexpected reliability completion schema')
    }
    if ([string]$start.run_id -cne $runId -or
        [string]$completion.run_id -cne $runId) {
        $errors.Add('reliability run ID does not match its write-once directory')
    }
    if ([string]$start.acceptance_record_sha256 -cne $acceptanceHash -or
        [string]$completion.acceptance_record_sha256 -cne $acceptanceHash) {
        $errors.Add('reliability run is not bound to the frozen acceptance record digest')
    }
    if ([string]$start.android_serial -cne [string]$acceptance.android_device.adb_serial -or
        [string]$start.package_name -cne [string]$acceptance.apk.package_name -or
        [string]$start.backend_health_uri -cne [string]$acceptance.backend.health_uri -or
        [string]$start.backend_release_id -cne [string]$acceptance.backend.release_id -or
        [string]$start.gateway_release_id -cne [string]$acceptance.gateway.release_id -or
        [string]$start.gateway_id -cne [string]$acceptance.gateway.gateway_id -or
        [string]$start.gateway_exposure -cne [string]$acceptance.gateway.exposure -or
        [string]$start.gateway_route_host -cne [string]$acceptance.gateway.route_host -or
        [string]$start.gateway_tls_server_name -cne [string]$acceptance.gateway.tls_server_name -or
        [string]$start.gateway_health_uri -cne [string]$acceptance.gateway.health_uri -or
        [string]$start.gateway_health_transport -cne [string]$acceptance.gateway.health_transport) {
        $errors.Add('reliability start identity differs from the frozen acceptance record')
    }
    if (-not (Test-KitsuJsonNumber -Value $acceptance.reliability_policy.minimum_duration_seconds) -or
        -not (Test-KitsuJsonNumber -Value $acceptance.reliability_policy.minimum_sample_coverage_ratio) -or
        -not (Test-KitsuJsonNumber -Value $acceptance.reliability_policy.minimum_health_ratio) -or
        -not (Test-KitsuJsonNumber -Value $acceptance.reliability_policy.maximum_consecutive_unhealthy_samples) -or
        -not (Test-KitsuJsonNumber -Value $acceptance.reliability_policy.minimum_terminal_healthy_samples) -or
        -not (Test-KitsuJsonNumber -Value $start.minimum_sample_coverage_ratio) -or
        -not (Test-KitsuJsonNumber -Value $start.minimum_health_ratio) -or
        -not (Test-KitsuJsonNumber -Value $start.maximum_consecutive_unhealthy_samples) -or
        -not (Test-KitsuJsonNumber -Value $start.minimum_terminal_healthy_samples) -or
        [int]$acceptance.reliability_policy.minimum_duration_seconds -ne
            $script:KitsuMinimumReliabilitySeconds -or
        [double]$acceptance.reliability_policy.minimum_sample_coverage_ratio -ne
            $script:KitsuMinimumReliabilitySampleCoverageRatio -or
        [double]$acceptance.reliability_policy.minimum_health_ratio -ne
            $script:KitsuMinimumReliabilityHealthRatio -or
        [int]$acceptance.reliability_policy.maximum_consecutive_unhealthy_samples -ne
            $script:KitsuMaximumConsecutiveUnhealthySamples -or
        [int]$acceptance.reliability_policy.minimum_terminal_healthy_samples -ne
            $script:KitsuMinimumTerminalHealthySamples -or
        $acceptance.reliability_policy.terminal_recovery_required -isnot [bool] -or
        -not [bool]$acceptance.reliability_policy.terminal_recovery_required -or
        $acceptance.reliability_policy.linked_manual_review_required -isnot [bool] -or
        -not [bool]$acceptance.reliability_policy.linked_manual_review_required -or
        [double]$start.minimum_sample_coverage_ratio -ne
            [double]$acceptance.reliability_policy.minimum_sample_coverage_ratio -or
        [double]$start.minimum_health_ratio -ne
            [double]$acceptance.reliability_policy.minimum_health_ratio -or
        [int]$start.maximum_consecutive_unhealthy_samples -ne
            [int]$acceptance.reliability_policy.maximum_consecutive_unhealthy_samples -or
        [int]$start.minimum_terminal_healthy_samples -ne
            [int]$acceptance.reliability_policy.minimum_terminal_healthy_samples -or
        $start.terminal_recovery_required -isnot [bool] -or
        -not [bool]$start.terminal_recovery_required) {
        $errors.Add('reliability policy is missing, weakened, or differs from the frozen thresholds')
    }
    if (-not (Test-KitsuBackendHealthUriAccepted `
            -CandidateUri ([string]$start.backend_health_uri) `
            -FrozenHealthUri ([string]$acceptance.backend.health_uri) `
            -FrozenBaseUrl ([string]$acceptance.backend.base_url))) {
        $errors.Add('reliability backend health URI is not the exact frozen HTTPS /health/live endpoint')
    }
    try {
        $gatewayHealth = [Uri][string]$start.gateway_health_uri
        if ($gatewayHealth.AbsolutePath -cne '/health/live' -or
            $gatewayHealth.Query -or $gatewayHealth.Fragment -or $gatewayHealth.UserInfo) {
            throw 'invalid gateway health endpoint'
        }
        $transport = [string]$start.gateway_health_transport
        $loopbackHosts = @('127.0.0.1', 'localhost', '::1')
        if ($transport -ceq 'loopback-tunnel') {
            if ($gatewayHealth.Scheme -notin @('http', 'https') -or
                $loopbackHosts -notcontains $gatewayHealth.Host.ToLowerInvariant()) {
                throw 'invalid loopback gateway health tunnel'
            }
        } elseif ($transport -ceq 'direct') {
            if ($gatewayHealth.Scheme -cne 'https' -or
                $gatewayHealth.DnsSafeHost -ine [string]$acceptance.gateway.route_host) {
                throw 'invalid direct gateway health route'
            }
        } else {
            throw 'unknown gateway health transport'
        }
    } catch {
        $errors.Add('reliability gateway health URI is not bound to the frozen gateway route')
    }
    if ($completion.status -ne 'COMPLETED') {
        $errors.Add('reliability run is not complete')
    }
    if ($null -ne $completion.interruption_type -and
        -not [string]::IsNullOrWhiteSpace([string]$completion.interruption_type)) {
        $errors.Add('completed reliability run carries an interruption marker')
    }

    $interval = 0
    if (-not (Test-KitsuJsonNumber -Value $start.sample_interval_seconds) -or
        -not [int]::TryParse([string]$start.sample_interval_seconds, [ref]$interval) -or
        $interval -lt 30 -or $interval -gt 300) {
        $errors.Add('invalid reliability sample interval')
    }
    $requiredSeconds = 0
    if (-not (Test-KitsuJsonNumber -Value $start.required_duration_seconds) -or
        -not [int]::TryParse([string]$start.required_duration_seconds, [ref]$requiredSeconds) -or
        $requiredSeconds -lt $script:KitsuMinimumReliabilitySeconds) {
        $errors.Add('required reliability duration is below 24 hours')
        $requiredSeconds = $script:KitsuMinimumReliabilitySeconds
    }
    $monotonicElapsed = 0.0
    if (-not (Test-KitsuJsonNumber -Value $completion.monotonic_elapsed_seconds) -or
        -not [double]::TryParse(
        [string]$completion.monotonic_elapsed_seconds,
        [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref]$monotonicElapsed
    ) -or [double]::IsNaN($monotonicElapsed) -or [double]::IsInfinity($monotonicElapsed) -or
        $monotonicElapsed -lt $requiredSeconds) {
        $errors.Add('monotonic reliability duration is below the required duration')
    }
    $wallElapsed = 0.0
    if (-not (Test-KitsuJsonNumber -Value $completion.wall_elapsed_seconds) -or
        -not [double]::TryParse(
        [string]$completion.wall_elapsed_seconds,
        [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref]$wallElapsed
    ) -or [double]::IsNaN($wallElapsed) -or [double]::IsInfinity($wallElapsed) -or
        $wallElapsed -lt $requiredSeconds) {
        $errors.Add('wall-clock reliability duration is below the required duration')
    }
    try {
        $started = ConvertTo-KitsuUtcDateTimeOffset -Value $start.started_at_utc
        $completed = ConvertTo-KitsuUtcDateTimeOffset -Value $completion.completed_at_utc
    } catch {
        $started = [DateTimeOffset]::MinValue
        $completed = [DateTimeOffset]::MinValue
        $errors.Add('reliability start or completion timestamp is malformed')
    }
    if ($started -lt $acceptanceStarted) {
        $errors.Add('reliability run begins before acceptance initialization')
    }
    if ($started -gt $validationNow -or $completed -gt $validationNow) {
        $errors.Add('reliability run is future-dated')
    }
    $utcElapsed = ($completed - $started).TotalSeconds
    if ($utcElapsed -lt $requiredSeconds) {
        $errors.Add('UTC reliability duration is below 24 hours or malformed')
    }
    $durationTolerance = [Math]::Max(300, $interval * 5)
    if ([Math]::Abs($utcElapsed - $wallElapsed) -gt $durationTolerance -or
        [Math]::Abs($utcElapsed - $monotonicElapsed) -gt $durationTolerance) {
        $errors.Add('UTC, wall, and monotonic reliability durations disagree')
    }
    try {
        $fileElapsed = ((Get-Item -LiteralPath $completionPath).LastWriteTimeUtc -
            (Get-Item -LiteralPath $startPath).LastWriteTimeUtc).TotalSeconds
        if ($fileElapsed -lt ($requiredSeconds - [Math]::Max(120, $interval * 2))) {
            $errors.Add('filesystem chronology does not corroborate a real-duration reliability run')
        }
    } catch {
        $errors.Add('filesystem chronology for the reliability run is unavailable')
    }

    if (-not (Test-Path -LiteralPath $samplesPath -PathType Leaf)) {
        $errors.Add('samples.jsonl is missing')
        return [pscustomobject]@{
            valid = $false
            errors = $errors.ToArray()
            sample_count = 0
            elapsed_seconds = $monotonicElapsed
            health = [ordered]@{}
        }
    }
    if ((Get-KitsuSha256 -LiteralPath $samplesPath) -cne [string]$completion.samples_sha256) {
        $errors.Add('samples.jsonl digest does not match completion.json')
    }
    $lines = @(Get-Content -LiteralPath $samplesPath | Where-Object { $_.Trim().Length -gt 0 })
    $declaredCount = 0
    if (-not (Test-KitsuJsonNumber -Value $completion.sample_count) -or
        -not [int]::TryParse([string]$completion.sample_count, [ref]$declaredCount) -or
        $declaredCount -ne $lines.Count) {
        $errors.Add('reliability sample count does not match completion.json')
    }
    $expectedMinimum = if ($interval -gt 0) {
        [Math]::Floor(
            $requiredSeconds / $interval * $script:KitsuMinimumReliabilitySampleCoverageRatio
        )
    } else { [int]::MaxValue }
    if ($lines.Count -lt $expectedMinimum) {
        $errors.Add('fewer than 90 percent of required reliability samples were recorded')
    }

    $gatewayTcpProbeEnabled = $start.gateway_tcp_probe -is [bool] -and
        [bool]$start.gateway_tcp_probe
    if ($start.gateway_tcp_probe -isnot [bool]) {
        $errors.Add('gateway TCP probe policy is missing or malformed')
    }
    $channelNames = @('android', 'app', 'backend', 'gateway')
    if ($gatewayTcpProbeEnabled) { $channelNames += 'gateway_tcp' }
    $healthyCounts = @{}
    $observedCounts = @{}
    $currentUnhealthy = @{}
    $maximumUnhealthy = @{}
    $firstHealthy = @{}
    $terminalHealth = @{}
    foreach ($channel in $channelNames) {
        $healthyCounts[$channel] = 0
        $observedCounts[$channel] = 0
        $currentUnhealthy[$channel] = 0
        $maximumUnhealthy[$channel] = 0
        $firstHealthy[$channel] = $null
        $terminalHealth[$channel] = New-Object System.Collections.Generic.List[bool]
    }

    $previousTimestamp = [DateTimeOffset]::MinValue
    $previousMonotonic = -1.0
    for ($index = 0; $index -lt $lines.Count; $index += 1) {
        try {
            Assert-KitsuNoSecrets -Text $lines[$index] -Label "reliability sample $($index + 1)"
            $sample = $lines[$index] | ConvertFrom-Json
            if ($sample.schema -cne $script:KitsuReliabilitySampleSchema -or
                [string]$sample.run_id -cne $runId -or
                -not (Test-KitsuJsonNumber -Value $sample.sequence) -or
                [int]$sample.sequence -ne ($index + 1)) {
                throw 'schema, run ID, or sequence mismatch'
            }
            $timestamp = ConvertTo-KitsuUtcDateTimeOffset -Value $sample.observed_at_utc
            if ($timestamp -le $previousTimestamp) {
                throw 'timestamps are not strictly increasing'
            }
            if ($previousTimestamp -ne [DateTimeOffset]::MinValue -and $interval -gt 0 -and
                ($timestamp - $previousTimestamp).TotalSeconds -gt ($interval * 5)) {
                throw 'sample gap exceeds five intervals'
            }
            $sampleMonotonic = 0.0
            if (-not (Test-KitsuJsonNumber -Value $sample.monotonic_elapsed_seconds) -or
                -not [double]::TryParse(
                [string]$sample.monotonic_elapsed_seconds,
                [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$sampleMonotonic
            ) -or [double]::IsNaN($sampleMonotonic) -or [double]::IsInfinity($sampleMonotonic) -or
                $sampleMonotonic -lt 0 -or $sampleMonotonic -le $previousMonotonic) {
                throw 'sample monotonic time is invalid or not increasing'
            }
            if ([Math]::Abs(($timestamp - $started).TotalSeconds - $sampleMonotonic) -gt
                [Math]::Max(300, $interval * 5)) {
                throw 'sample UTC and monotonic time disagree'
            }
            if ($sample.android.adb_online -isnot [bool] -or
                $sample.backend_health.reachable -isnot [bool] -or
                $sample.gateway_health.reachable -isnot [bool] -or
                $sample.gateway_health.identity_verified -isnot [bool]) {
                throw 'health booleans are missing or malformed'
            }
            if (-not (Test-KitsuJsonNumber -Value $sample.backend_health.latency_ms) -or
                -not (Test-KitsuJsonNumber -Value $sample.gateway_health.latency_ms) -or
                [double]$sample.backend_health.latency_ms -lt 0 -or
                [double]$sample.gateway_health.latency_ms -lt 0) {
                throw 'health latency fields are missing or malformed'
            }
            $androidHealthy = [bool]$sample.android.adb_online
            $appHealthy = $androidHealthy -and
                [string]$sample.android.app_process_ids -match '^[0-9]+(?:\s+[0-9]+)*$'
            $backendStatus = 0
            $backendHealthy = [bool]$sample.backend_health.reachable -and
                (Test-KitsuJsonNumber -Value $sample.backend_health.status_code) -and
                [int]::TryParse([string]$sample.backend_health.status_code, [ref]$backendStatus) -and
                $backendStatus -ge 200 -and $backendStatus -lt 300
            $gatewayStatus = 0
            $gatewayHealthy = [bool]$sample.gateway_health.reachable -and
                (Test-KitsuJsonNumber -Value $sample.gateway_health.status_code) -and
                [int]::TryParse([string]$sample.gateway_health.status_code, [ref]$gatewayStatus) -and
                $gatewayStatus -ge 200 -and $gatewayStatus -lt 300 -and
                [bool]$sample.gateway_health.identity_verified -and
                [string]$sample.gateway_health.gateway_id -ceq [string]$acceptance.gateway.gateway_id -and
                [string]$sample.gateway_health.deployment_scope -ceq [string]$acceptance.gateway.exposure -and
                (Test-KitsuJsonNumber -Value $sample.gateway_health.protocol) -and
                [int]$sample.gateway_health.protocol -eq $script:KitsuGatewayProtocolVersion -and
                [string]$sample.gateway_health.reported_status -ceq 'ok' -and
                $sample.gateway_health.backend_online -is [bool] -and
                [bool]$sample.gateway_health.backend_online
            $healthByChannel = [ordered]@{
                android = $androidHealthy
                app = $appHealthy
                backend = $backendHealthy
                gateway = $gatewayHealthy
            }
            if ($gatewayTcpProbeEnabled -and (($index) % 5 -eq 0)) {
                if ($sample.gateway_tcp.bootstrap.open -isnot [bool] -or
                    $sample.gateway_tcp.steady_mtls.open -isnot [bool]) {
                    throw 'gateway TCP probe is missing or malformed'
                }
                $healthByChannel.gateway_tcp = [bool]$sample.gateway_tcp.bootstrap.open -and
                    [bool]$sample.gateway_tcp.steady_mtls.open
            }
            foreach ($channel in $healthByChannel.Keys) {
                $healthy = [bool]$healthByChannel[$channel]
                $observedCounts[$channel] += 1
                if ($healthy) {
                    $healthyCounts[$channel] += 1
                    $currentUnhealthy[$channel] = 0
                } else {
                    $currentUnhealthy[$channel] += 1
                    $maximumUnhealthy[$channel] = [Math]::Max(
                        $maximumUnhealthy[$channel], $currentUnhealthy[$channel]
                    )
                }
                if ($null -eq $firstHealthy[$channel]) { $firstHealthy[$channel] = $healthy }
                $terminalHealth[$channel].Add($healthy)
                if ($terminalHealth[$channel].Count -gt $script:KitsuMinimumTerminalHealthySamples) {
                    $terminalHealth[$channel].RemoveAt(0)
                }
            }
            $previousTimestamp = $timestamp
            $previousMonotonic = $sampleMonotonic
        } catch {
            $errors.Add("invalid reliability sample $($index + 1): $($_.Exception.Message)")
            break
        }
    }
    if ($lines.Count -gt 0 -and $interval -gt 0) {
        if (($previousTimestamp - $completed).Duration().TotalSeconds -gt ($interval * 5) -or
            $previousMonotonic -lt ($requiredSeconds - ($interval * 5))) {
            $errors.Add('reliability samples do not cover the completed duration')
        }
    }

    $healthSummary = [ordered]@{}
    foreach ($channel in $channelNames) {
        $observed = [int]$observedCounts[$channel]
        $ratio = if ($observed -gt 0) {
            [double]$healthyCounts[$channel] / [double]$observed
        } else { 0.0 }
        $healthSummary[$channel] = [ordered]@{
            healthy_samples = [int]$healthyCounts[$channel]
            observed_samples = $observed
            healthy_ratio = [Math]::Round($ratio, 6)
            maximum_consecutive_unhealthy_samples = [int]$maximumUnhealthy[$channel]
            terminal_healthy_samples = @($terminalHealth[$channel] | Where-Object { $_ }).Count
            required_terminal_healthy_samples = $script:KitsuMinimumTerminalHealthySamples
        }
        if ($ratio -lt $script:KitsuMinimumReliabilityHealthRatio) {
            $errors.Add("$channel reliability health is below 99 percent")
        }
        if ([int]$maximumUnhealthy[$channel] -gt $script:KitsuMaximumConsecutiveUnhealthySamples) {
            $errors.Add("$channel reliability outage exceeds five consecutive samples")
        }
        if ($firstHealthy[$channel] -ne $true) {
            $errors.Add("$channel reliability did not start healthy")
        }
        if ($terminalHealth[$channel].Count -lt $script:KitsuMinimumTerminalHealthySamples -or
            @($terminalHealth[$channel] | Where-Object { -not $_ }).Count -gt 0) {
            $errors.Add("$channel reliability did not end with five healthy observations")
        }
    }

    return [pscustomobject]@{
        valid = $errors.Count -eq 0
        errors = $errors.ToArray()
        sample_count = $lines.Count
        elapsed_seconds = $monotonicElapsed
        health = $healthSummary
    }
}
