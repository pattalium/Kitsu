[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$EvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ApkPath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$AdbPath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ApkSignerPath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$AaptPath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$SourceProvenancePath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$FirmwareReleasePath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$FirmwareBundlePath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')]
    [string]$OperatorId,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9_-]{1,64}$')]
    [string]$ExpectedDeviceUid,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$')]
    [string]$ExpectedGatewayId,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')]
    [string]$ExpectedGatewayReleaseId,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')]
    [string]$ExpectedBackendReleaseId,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')]
    [string]$ExpectedFirmwareReleaseId,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?$')]
    [string]$ExpectedFirmwareVersion,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9.-]{1,253}$')]
    [string]$ExpectedGatewayHost,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^(?=.{1,253}$)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}$')]
    [string]$ExpectedGatewayServerName,

    [Parameter(Mandatory = $true)]
    [ValidateScript({
        $uri = $null
        if (-not [Uri]::TryCreate($_, [UriKind]::Absolute, [ref]$uri) -or
            $uri.UserInfo -or $uri.Query -or $uri.Fragment -or
            $uri.AbsolutePath -ne '/health/live') { return $false }
        if ($uri.Scheme -eq 'https') { return $true }
        return $uri.Scheme -eq 'http' -and
            $uri.Host.ToLowerInvariant() -in @('127.0.0.1', 'localhost', '::1')
    })]
    [string]$ExpectedGatewayHealthUri,

    [ValidateRange(1, 30)]
    [int]$GatewayProbeTimeoutSeconds = 5,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 65535)]
    [int]$ExpectedBootstrapPort,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 65535)]
    [int]$ExpectedSteadyMtlsPort,

    [Parameter(Mandatory = $true)]
    [ValidateSet('public', 'private')]
    [string]$GatewayExposure,

    [Parameter(Mandatory = $true)]
    [ValidateScript({
        $uri = $null
        [Uri]::TryCreate($_, [UriKind]::Absolute, [ref]$uri) -and
            $uri.Scheme -eq 'https' -and -not $uri.UserInfo -and
            $uri.AbsolutePath -eq '/' -and -not $uri.Query -and -not $uri.Fragment
    })]
    [string]$BackendBaseUrl,

    [ValidatePattern('^[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+$')]
    [string]$ExpectedPackageName = 'app.kitsu.mobile',

    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.+_-]{0,63}$')]
    [string]$ExpectedVersionName = '1.1.5',

    [ValidateRange(1, 2147483647)]
    [int]$ExpectedVersionCode = 11,

    [ValidateRange(1, 1000)]
    [int]$ExpectedMinimumApi = 26,

    [ValidateRange(1, 1000)]
    [int]$ExpectedTargetApi = 35,

    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$ExpectedSigningCertificateSha256 =
        'a5a3cddb0d2c103630c6e622ac7f2051085a4c082db37aefdbadfc75d0a2d7fc',

    [switch]$ExcludeMeshCorePhysicalProof
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-common.ps1')

function Invoke-KitsuTool {
    param(
        [Parameter(Mandatory = $true)][string]$LiteralPath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )
    $output = @(& $LiteralPath @Arguments 2>&1 | ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    if (-not $AllowFailure -and $exitCode -ne 0) {
        throw "Tool failed with exit code ${exitCode}: $([System.IO.Path]::GetFileName($LiteralPath))"
    }
    return [pscustomobject]@{ output = $output; exit_code = $exitCode }
}

function Invoke-KitsuAdb {
    param(
        [Parameter(Mandatory = $true)][string]$Serial,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )
    return Invoke-KitsuTool -LiteralPath $resolvedAdb -Arguments (@('-s', $Serial) + $Arguments) `
        -AllowFailure:$AllowFailure
}

function Get-KitsuJsonProperty {
    param(
        [Parameter(Mandatory = $true)][object]$InputObject,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

$resolvedEvidence = Assert-KitsuEvidenceOutsideRepository -EvidenceDirectory $EvidenceDirectory
$resolvedApk = (Resolve-Path -LiteralPath $ApkPath).Path
$resolvedAdb = (Resolve-Path -LiteralPath $AdbPath).Path
$resolvedApkSigner = (Resolve-Path -LiteralPath $ApkSignerPath).Path
$resolvedAapt = (Resolve-Path -LiteralPath $AaptPath).Path
$resolvedProvenance = (Resolve-Path -LiteralPath $SourceProvenancePath).Path
$resolvedFirmwareRelease = (Resolve-Path -LiteralPath $FirmwareReleasePath).Path
$resolvedFirmwareBundle = (Resolve-Path -LiteralPath $FirmwareBundlePath).Path

if ($ExpectedBootstrapPort -eq $ExpectedSteadyMtlsPort) {
    throw 'Bootstrap and steady mTLS ports must be different.'
}
if ($ExpectedGatewayHost.Contains('://') -or $ExpectedGatewayHost.Contains('/') -or
    $ExpectedGatewayServerName.Contains('://') -or $ExpectedGatewayServerName.Contains('/')) {
    throw 'Gateway host and server name must be bare routing/TLS names, not URLs.'
}
if ([Uri]::CheckHostName($ExpectedGatewayHost) -eq [UriHostNameType]::Unknown -or
    ($GatewayExposure -eq 'public' -and
        [Uri]::CheckHostName($ExpectedGatewayHost) -ne [UriHostNameType]::Dns)) {
    throw 'Gateway route host is invalid; public gateways require a DNS host name.'
}
$gatewayHealth = [Uri]$ExpectedGatewayHealthUri
$gatewayHealthHost = $gatewayHealth.Host.ToLowerInvariant()
$loopbackHealthHosts = @('127.0.0.1', 'localhost', '::1')
if ($loopbackHealthHosts -contains $gatewayHealthHost) {
    $gatewayHealthTransport = 'loopback-tunnel'
} elseif ($gatewayHealth.Scheme -eq 'https' -and
    $gatewayHealth.DnsSafeHost -ieq $ExpectedGatewayHost) {
    $gatewayHealthTransport = 'direct'
} else {
    throw 'Gateway health URI must use the frozen gateway route or an explicit loopback operations tunnel.'
}
$backendBase = [Uri]$BackendBaseUrl
$backendHealth = [Uri]($backendBase.GetLeftPart([UriPartial]::Authority) + '/health/live')
if (-not (Test-KitsuBackendHealthUriAccepted -CandidateUri $backendHealth.AbsoluteUri `
        -FrozenHealthUri $backendHealth.AbsoluteUri -FrozenBaseUrl $backendBase.AbsoluteUri)) {
    throw 'Unable to derive the exact backend /health/live URI from the frozen backend base URL.'
}

if (Test-Path -LiteralPath $resolvedEvidence) {
    if (-not (Test-Path -LiteralPath $resolvedEvidence -PathType Container)) {
        throw "Evidence path is not a directory: $resolvedEvidence"
    }
    if (@(Get-ChildItem -LiteralPath $resolvedEvidence -Force).Count -ne 0) {
        throw 'Evidence directory must be new or empty; the harness never overwrites evidence files.'
    }
} else {
    New-Item -ItemType Directory -Path $resolvedEvidence | Out-Null
}

$adbDevices = Invoke-KitsuTool -LiteralPath $resolvedAdb -Arguments @('devices', '-l')
$authorized = @(
    $adbDevices.output | ForEach-Object {
        if ($_ -match '^([^\s]+)\s+device(?:\s|$)') { $Matches[1] }
    }
)
if ($authorized.Count -ne 1) {
    throw "Exactly one authorized physical Android device is required; found $($authorized.Count)."
}
$serial = [string]$authorized[0]
if ($serial -match '^emulator-' -or $serial -match '^localhost:') {
    throw 'Emulators and local virtual devices are forbidden for physical acceptance evidence.'
}

$properties = [ordered]@{}
foreach ($name in @(
    'ro.product.manufacturer', 'ro.product.model', 'ro.product.device',
    'ro.build.fingerprint', 'ro.build.version.sdk', 'ro.build.version.release',
    'ro.hardware', 'ro.kernel.qemu', 'ro.boot.qemu'
)) {
    $value = Invoke-KitsuAdb -Serial $serial -Arguments @('shell', 'getprop', $name)
    $properties[$name] = (($value.output -join '').Trim())
}
if ($properties['ro.kernel.qemu'] -eq '1' -or $properties['ro.boot.qemu'] -eq '1' -or
    $properties['ro.hardware'] -match '^(goldfish|ranchu)$') {
    throw 'The connected target reports virtual/emulated hardware and cannot be used for physical acceptance.'
}
$apiLevel = 0
if (-not [int]::TryParse([string]$properties['ro.build.version.sdk'], [ref]$apiLevel) -or
    $apiLevel -lt 26) {
    throw 'The connected physical Android device must run API 26 or newer.'
}

$apkSigner = Invoke-KitsuTool -LiteralPath $resolvedApkSigner `
    -Arguments @('verify', '--verbose', '--print-certs', $resolvedApk)
$apkSignerText = $apkSigner.output -join "`n"
if ($apkSignerText -notmatch '(?m)^Verifies\s*$' -or
    $apkSignerText -notmatch '(?m)^Number of signers:\s*1\s*$' -or
    $apkSignerText -notmatch '(?m)^Verified using v2 scheme \(APK Signature Scheme v2\):\s*true\s*$' -or
    $apkSignerText -notmatch '(?m)^Verified using v3 scheme \(APK Signature Scheme v3\):\s*true\s*$' -or
    $apkSignerText -notmatch '(?m)^Signer #1 certificate SHA-256 digest:\s*([0-9a-fA-F]{64})\s*$') {
    throw 'APK signature verification did not return one unambiguous signer.'
}
$actualCertificate = $Matches[1].ToLowerInvariant()
if ($actualCertificate -ne $ExpectedSigningCertificateSha256.ToLowerInvariant()) {
    throw 'APK signing certificate does not match the frozen release identity.'
}

$aapt = Invoke-KitsuTool -LiteralPath $resolvedAapt -Arguments @('dump', 'badging', $resolvedApk)
$aaptText = $aapt.output -join "`n"
if ($aaptText -notmatch "(?m)^package:\s+name='([^']+)'\s+versionCode='([0-9]+)'\s+versionName='([^']+)'") {
    throw 'Unable to parse package identity from aapt badging.'
}
$actualPackage = $Matches[1]
$actualVersionCode = [int]$Matches[2]
$actualVersionName = $Matches[3]
$minimumApi = $null
$targetApi = $null
if ($aaptText -match "(?m)^sdkVersion:'([0-9]+)'\s*$") { $minimumApi = [int]$Matches[1] }
if ($aaptText -match "(?m)^targetSdkVersion:'([0-9]+)'\s*$") { $targetApi = [int]$Matches[1] }
if ($actualPackage -ne $ExpectedPackageName -or $actualVersionCode -ne $ExpectedVersionCode -or
    $actualVersionName -ne $ExpectedVersionName -or $minimumApi -ne $ExpectedMinimumApi -or
    $targetApi -ne $ExpectedTargetApi) {
    throw 'APK package/version does not match the frozen release identity.'
}

$sourceProvenance = Read-KitsuJson -LiteralPath $resolvedProvenance
if ($sourceProvenance.schema -ne 'kitsu.android-source-provenance.v1' -or
    $sourceProvenance.application_id -ne $ExpectedPackageName -or
    [int]$sourceProvenance.version_code -ne $ExpectedVersionCode -or
    $sourceProvenance.version_name -ne $ExpectedVersionName -or
    [string]$sourceProvenance.tree_sha256 -notmatch '^[0-9a-f]{64}$') {
    throw 'Android source provenance does not match the frozen APK identity.'
}

$firmwareRelease = Read-KitsuJson -LiteralPath $resolvedFirmwareRelease
$firmwareReleaseId = [string](Get-KitsuJsonProperty -InputObject $firmwareRelease -Name 'release_id')
$firmwareVersion = [string](Get-KitsuJsonProperty -InputObject $firmwareRelease -Name 'firmware_version')
if ($firmwareReleaseId -ne $ExpectedFirmwareReleaseId -or
    $firmwareVersion -ne $ExpectedFirmwareVersion) {
    throw 'Firmware release metadata does not match the frozen physical-test identity.'
}

$installedDump = Invoke-KitsuAdb -Serial $serial `
    -Arguments @('shell', 'dumpsys', 'package', $ExpectedPackageName) -AllowFailure
$installedText = $installedDump.output -join "`n"
$installedVersionCode = $null
$installedVersionName = $null
if ($installedText -match '(?m)^\s*versionCode=([0-9]+)') {
    $installedVersionCode = [int]$Matches[1]
}
if ($installedText -match '(?m)^\s*versionName=([^\s]+)') {
    $installedVersionName = $Matches[1]
}

$screenSize = Invoke-KitsuAdb -Serial $serial -Arguments @('shell', 'wm', 'size') -AllowFailure
$screenDensity = Invoke-KitsuAdb -Serial $serial -Arguments @('shell', 'wm', 'density') -AllowFailure
$fontScale = Invoke-KitsuAdb -Serial $serial `
    -Arguments @('shell', 'settings', 'get', 'system', 'font_scale') -AllowFailure
$deviceIdle = Invoke-KitsuAdb -Serial $serial -Arguments @('shell', 'dumpsys', 'deviceidle') -AllowFailure
$isBatteryWhitelisted = ($deviceIdle.output -join "`n") -match
    ('(?m)^\s*' + [Regex]::Escape($ExpectedPackageName) + '\s*$')

$gatewayCaptureObservation = Invoke-KitsuGatewayHealthProbe `
    -Uri $gatewayHealth.AbsoluteUri -TimeoutSeconds $GatewayProbeTimeoutSeconds `
    -ExpectedGatewayId $ExpectedGatewayId -ExpectedExposure $GatewayExposure
if (-not (Test-KitsuGatewayHealthObservationAccepted -Observation $gatewayCaptureObservation `
        -ExpectedGatewayId $ExpectedGatewayId -ExpectedExposure $GatewayExposure)) {
    throw 'Capture requires a healthy 2xx response from the exact frozen gateway identity and deployment scope.'
}
$gatewayHealthVerifiedAt = [DateTimeOffset]::UtcNow

$started = [DateTimeOffset]::UtcNow
$record = [ordered]@{
    schema = $script:KitsuAcceptanceSchema
    status = 'IN_PROGRESS'
    started_at_utc = $started.ToString('o')
    operator_id = $OperatorId
    scope = [ordered]@{
        meshcore_physical_proof = -not [bool]$ExcludeMeshCorePhysicalProof
        gateway_exposure = $GatewayExposure
        statement = 'Emulator, unit, and host tests do not replace this physical acceptance run.'
    }
    apk = [ordered]@{
        file_name = [System.IO.Path]::GetFileName($resolvedApk)
        bytes = (Get-Item -LiteralPath $resolvedApk).Length
        sha256 = Get-KitsuSha256 -LiteralPath $resolvedApk
        package_name = $actualPackage
        version_code = $actualVersionCode
        version_name = $actualVersionName
        minimum_api = $minimumApi
        target_api = $targetApi
        signing_certificate_sha256 = $actualCertificate
        verified_signature_schemes = @('v2', 'v3')
        artifact_last_write_at_utc = (Get-Item -LiteralPath $resolvedApk).LastWriteTimeUtc.ToString('o')
    }
    source_provenance = [ordered]@{
        file_name = [System.IO.Path]::GetFileName($resolvedProvenance)
        sha256 = Get-KitsuSha256 -LiteralPath $resolvedProvenance
        tree_sha256 = [string]$sourceProvenance.tree_sha256
        generated_at_utc = [string]$sourceProvenance.generated_at_utc
    }
    firmware = [ordered]@{
        release_file_name = [System.IO.Path]::GetFileName($resolvedFirmwareRelease)
        release_file_sha256 = Get-KitsuSha256 -LiteralPath $resolvedFirmwareRelease
        bundle_file_name = [System.IO.Path]::GetFileName($resolvedFirmwareBundle)
        bundle_bytes = (Get-Item -LiteralPath $resolvedFirmwareBundle).Length
        bundle_sha256 = Get-KitsuSha256 -LiteralPath $resolvedFirmwareBundle
        release_id = $firmwareReleaseId
        version = $firmwareVersion
    }
    backend = [ordered]@{
        base_url = $backendBase.AbsoluteUri
        health_uri = $backendHealth.AbsoluteUri
        release_id = $ExpectedBackendReleaseId
    }
    gateway = [ordered]@{
        gateway_id = $ExpectedGatewayId
        release_id = $ExpectedGatewayReleaseId
        exposure = $GatewayExposure
        route_host = $ExpectedGatewayHost
        tls_server_name = $ExpectedGatewayServerName
        health_uri = $gatewayHealth.AbsoluteUri
        health_transport = $gatewayHealthTransport
        capture_health_verification = [ordered]@{
            verified = $true
            verified_at_utc = $gatewayHealthVerifiedAt.ToString('o')
            status_code = [int]$gatewayCaptureObservation.status_code
            latency_ms = [double]$gatewayCaptureObservation.latency_ms
        }
        bootstrap_port = $ExpectedBootstrapPort
        steady_mtls_port = $ExpectedSteadyMtlsPort
    }
    heltec = [ordered]@{
        expected_device_uid = $ExpectedDeviceUid
        note = 'The exact UID and gateway binding must be confirmed from authenticated device/backend evidence.'
    }
    android_device = [ordered]@{
        adb_serial = $serial
        physical_device_checks_passed = $true
        properties = $properties
        bluetooth_chipset = 'not_collected'
        screen = [ordered]@{
            size = ($screenSize.output -join ' ').Trim()
            density = ($screenDensity.output -join ' ').Trim()
            font_scale = ($fontScale.output -join '').Trim()
        }
        kitsu_battery_optimization_whitelisted = [bool]$isBatteryWhitelisted
        installed_app = [ordered]@{
            detected = $null -ne $installedVersionCode
            version_code = $installedVersionCode
            version_name = $installedVersionName
        }
    }
    reliability_policy = [ordered]@{
        minimum_duration_seconds = $script:KitsuMinimumReliabilitySeconds
        minimum_sample_coverage_ratio = $script:KitsuMinimumReliabilitySampleCoverageRatio
        minimum_health_ratio = $script:KitsuMinimumReliabilityHealthRatio
        maximum_consecutive_unhealthy_samples = $script:KitsuMaximumConsecutiveUnhealthySamples
        minimum_terminal_healthy_samples = $script:KitsuMinimumTerminalHealthySamples
        terminal_recovery_required = $true
        linked_manual_review_required = $true
    }
    evidence_policy = [ordered]@{
        harness_uses_create_new_files = $true
        tamper_proof = $false
        prohibited = @(
            'passwords', 'Wi-Fi passphrases', 'OAuth tokens', 'claim tokens',
            'controller roots', 'private keys', 'raw BLE envelopes', 'authorization headers'
        )
        next_step = 'Use record-physical-case.ps1 for every attempt and preserve the evidence in access-controlled external storage.'
    }
}

$recordPath = Join-Path $resolvedEvidence 'acceptance-record.json'
Write-KitsuNewJson -LiteralPath $recordPath -InputObject $record

Write-Host "Evidence initialized at $resolvedEvidence"
Write-Host "APK SHA-256: $($record.apk.sha256)"
Write-Host "Android serial: $serial"
Write-Host 'Status remains IN_PROGRESS until the strict finalizer validates every required case and a real 24-hour run.'
