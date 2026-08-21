[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$EvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$AdbPath,

    [ValidateRange(24, 720)]
    [int]$DurationHours = 24,

    [ValidateRange(30, 300)]
    [int]$SampleIntervalSeconds = 60,

    [ValidateRange(1, 30)]
    [int]$ProbeTimeoutSeconds = 5,

    [ValidatePattern('^[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+$')]
    [string]$PackageName = 'app.kitsu.mobile',

    [switch]$ProbeGatewayTcp
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-common.ps1')

function Invoke-KitsuAdbSafe {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    try {
        $output = @(& $resolvedAdb -s $serial @Arguments 2>&1 | ForEach-Object { $_.ToString() })
        return [pscustomobject]@{
            ok = $LASTEXITCODE -eq 0
            text = ($output -join "`n").Trim()
        }
    } catch {
        return [pscustomobject]@{ ok = $false; text = '' }
    }
}

function Test-KitsuHttpHealth {
    param([Parameter(Mandatory = $true)][string]$Uri)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $statusCode = $null
    $reachable = $false
    try {
        $response = Invoke-WebRequest -Uri $Uri -Method Get -TimeoutSec $ProbeTimeoutSeconds `
            -UseBasicParsing -MaximumRedirection 0
        $statusCode = [int]$response.StatusCode
        $reachable = $statusCode -ge 200 -and $statusCode -lt 300
    } catch {
        if ($null -ne $_.Exception.Response -and $null -ne $_.Exception.Response.StatusCode) {
            $statusCode = [int]$_.Exception.Response.StatusCode
        }
    } finally {
        $timer.Stop()
    }
    return [ordered]@{
        reachable = $reachable
        status_code = $statusCode
        latency_ms = [Math]::Round($timer.Elapsed.TotalMilliseconds, 1)
    }
}

function Test-KitsuTcpPort {
    param(
        [Parameter(Mandatory = $true)][string]$HostName,
        [Parameter(Mandatory = $true)][int]$Port
    )
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $client = New-Object System.Net.Sockets.TcpClient
    $waitHandle = $null
    $open = $false
    try {
        $pending = $client.BeginConnect($HostName, $Port, $null, $null)
        $waitHandle = $pending.AsyncWaitHandle
        if ($waitHandle.WaitOne($ProbeTimeoutSeconds * 1000)) {
            $client.EndConnect($pending)
            $open = $client.Connected
        }
    } catch {
        $open = $false
    } finally {
        $client.Dispose()
        if ($null -ne $waitHandle) { $waitHandle.Dispose() }
        $timer.Stop()
    }
    return [ordered]@{
        open = $open
        latency_ms = [Math]::Round($timer.Elapsed.TotalMilliseconds, 1)
    }
}

$evidenceRoot = Assert-KitsuEvidenceOutsideRepository `
    -EvidenceDirectory (Resolve-Path -LiteralPath $EvidenceDirectory).Path
Assert-KitsuEvidenceTreeNoReparsePoints -EvidenceDirectory $evidenceRoot
$resolvedAdb = (Resolve-Path -LiteralPath $AdbPath).Path
$acceptancePath = Join-Path $evidenceRoot 'acceptance-record.json'
$acceptance = Read-KitsuJson -LiteralPath $acceptancePath
$acceptanceHash = Get-KitsuSha256 -LiteralPath $acceptancePath
if ($acceptance.schema -ne $script:KitsuAcceptanceSchema -or $acceptance.status -ne 'IN_PROGRESS') {
    throw 'The evidence root is not an in-progress Kitsu physical acceptance record.'
}
if ((ConvertTo-KitsuUtcDateTimeOffset -Value $acceptance.started_at_utc) -gt
    [DateTimeOffset]::UtcNow) {
    throw 'The frozen acceptance record is future-dated.'
}
if ($PackageName -ne [string]$acceptance.apk.package_name) {
    throw 'Reliability package name does not match the frozen APK identity.'
}
$BackendHealthUri = [string]$acceptance.backend.health_uri
$backendHealth = [Uri]$BackendHealthUri
$backendBase = [Uri][string]$acceptance.backend.base_url
if (-not (Test-KitsuBackendHealthUriAccepted -CandidateUri $BackendHealthUri `
        -FrozenHealthUri ([string]$acceptance.backend.health_uri) `
        -FrozenBaseUrl $backendBase.AbsoluteUri)) {
    throw 'Frozen backend health URI must be the exact HTTPS /health/live endpoint.'
}
$GatewayHealthUri = [string]$acceptance.gateway.health_uri
$gatewayHealth = [Uri]$GatewayHealthUri
if ($gatewayHealth.AbsolutePath -cne '/health/live' -or $gatewayHealth.Query -or
    $gatewayHealth.Fragment -or $gatewayHealth.UserInfo) {
    throw 'Frozen gateway health URI is invalid.'
}
$loopbackHealthHosts = @('127.0.0.1', 'localhost', '::1')
if ([string]$acceptance.gateway.health_transport -ceq 'loopback-tunnel') {
    if ($gatewayHealth.Scheme -notin @('http', 'https') -or
        $loopbackHealthHosts -notcontains $gatewayHealth.Host.ToLowerInvariant()) {
        throw 'Frozen gateway health URI is not the declared loopback operations tunnel.'
    }
} elseif ([string]$acceptance.gateway.health_transport -ceq 'direct') {
    if ($gatewayHealth.Scheme -cne 'https' -or
        $gatewayHealth.DnsSafeHost -ine [string]$acceptance.gateway.route_host) {
        throw 'Frozen gateway health URI does not use the frozen gateway route.'
    }
} else {
    throw 'Frozen gateway health transport is invalid.'
}

$deviceLines = @(& $resolvedAdb devices -l 2>&1 | ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'adb devices failed.' }
$authorized = @(
    $deviceLines | ForEach-Object {
        if ($_ -match '^([^\s]+)\s+device(?:\s|$)') { $Matches[1] }
    }
)
if ($authorized.Count -ne 1 -or $authorized[0] -ne [string]$acceptance.android_device.adb_serial) {
    throw 'Exactly the frozen physical Android device must be authorized before starting reliability monitoring.'
}
$serial = [string]$authorized[0]
if ($serial -match '^emulator-' -or $serial -match '^localhost:') {
    throw 'Virtual Android devices cannot produce physical reliability evidence.'
}
$qemu = Invoke-KitsuAdbSafe -Arguments @('shell', 'getprop', 'ro.kernel.qemu')
$hardware = Invoke-KitsuAdbSafe -Arguments @('shell', 'getprop', 'ro.hardware')
if (-not $qemu.ok -or -not $hardware.ok -or [string]::IsNullOrWhiteSpace($hardware.text)) {
    throw 'Physical-device properties could not be revalidated before reliability monitoring.'
}
if ($qemu.text -eq '1' -or $hardware.text -match '^(goldfish|ranchu)$') {
    throw 'The connected target reports virtual/emulated hardware.'
}

$preflightState = Invoke-KitsuAdbSafe -Arguments @('get-state')
$preflightPid = Invoke-KitsuAdbSafe -Arguments @('shell', 'pidof', $PackageName)
$preflightBackend = Test-KitsuHttpHealth -Uri $BackendHealthUri
$preflightGateway = Invoke-KitsuGatewayHealthProbe -Uri $GatewayHealthUri `
    -TimeoutSeconds $ProbeTimeoutSeconds `
    -ExpectedGatewayId ([string]$acceptance.gateway.gateway_id) `
    -ExpectedExposure ([string]$acceptance.gateway.exposure)
if (-not $preflightState.ok -or $preflightState.text -cne 'device' -or
    -not $preflightPid.ok -or $preflightPid.text -notmatch '^[0-9]+(?:\s+[0-9]+)*$' -or
    -not [bool]$preflightBackend.reachable -or
    -not [bool]$preflightGateway.reachable -or
    -not [bool]$preflightGateway.identity_verified -or
    [string]$preflightGateway.reported_status -cne 'ok' -or
    $preflightGateway.backend_online -isnot [bool] -or
    -not [bool]$preflightGateway.backend_online) {
    throw 'Reliability preflight requires the frozen physical device/app, backend, and exact gateway identity to be healthy.'
}

$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' +
    [Guid]::NewGuid().ToString('N').Substring(0, 12)
$reliabilityRoot = Assert-KitsuPathWithin -BaseDirectory $evidenceRoot `
    -CandidatePath (Join-Path $evidenceRoot 'reliability')
$runDirectory = Assert-KitsuPathWithin -BaseDirectory $evidenceRoot `
    -CandidatePath (Join-Path $reliabilityRoot $runId)
New-Item -ItemType Directory -Path $runDirectory | Out-Null
$startPath = Join-Path $runDirectory 'start.json'
$samplesPath = Join-Path $runDirectory 'samples.jsonl'
$completionPath = Join-Path $runDirectory 'completion.json'
$started = [DateTimeOffset]::UtcNow
$requiredSeconds = $DurationHours * 60 * 60
$startRecord = [ordered]@{
    schema = $script:KitsuReliabilityStartSchema
    run_id = $runId
    acceptance_record_sha256 = $acceptanceHash
    started_at_utc = $started.ToString('o')
    required_duration_seconds = $requiredSeconds
    sample_interval_seconds = $SampleIntervalSeconds
    android_serial = $serial
    package_name = $PackageName
    backend_health_uri = $BackendHealthUri
    backend_release_id = [string]$acceptance.backend.release_id
    gateway_health_uri = $GatewayHealthUri
    gateway_health_transport = [string]$acceptance.gateway.health_transport
    gateway_id = [string]$acceptance.gateway.gateway_id
    gateway_release_id = [string]$acceptance.gateway.release_id
    gateway_exposure = [string]$acceptance.gateway.exposure
    gateway_route_host = [string]$acceptance.gateway.route_host
    gateway_tls_server_name = [string]$acceptance.gateway.tls_server_name
    gateway_tcp_probe = [bool]$ProbeGatewayTcp
    gateway_host = if ($ProbeGatewayTcp) { [string]$acceptance.gateway.route_host } else { $null }
    gateway_bootstrap_port = if ($ProbeGatewayTcp) { [int]$acceptance.gateway.bootstrap_port } else { $null }
    gateway_steady_mtls_port = if ($ProbeGatewayTcp) { [int]$acceptance.gateway.steady_mtls_port } else { $null }
    minimum_sample_coverage_ratio = [double]$acceptance.reliability_policy.minimum_sample_coverage_ratio
    minimum_health_ratio = [double]$acceptance.reliability_policy.minimum_health_ratio
    maximum_consecutive_unhealthy_samples = [int]$acceptance.reliability_policy.maximum_consecutive_unhealthy_samples
    minimum_terminal_healthy_samples = [int]$acceptance.reliability_policy.minimum_terminal_healthy_samples
    terminal_recovery_required = [bool]$acceptance.reliability_policy.terminal_recovery_required
    limitations = @(
        'Health responses are never stored.',
        'No authentication header, token, Wi-Fi identifier, passphrase, or raw BLE frame is collected.',
        'Operator case evidence is still required for BLE/Wi-Fi transitions, Doze, reboots, and recovery.'
    )
}
Write-KitsuNewJson -LiteralPath $startPath -InputObject $startRecord
Write-KitsuNewUtf8Text -LiteralPath $samplesPath -Text ''

$encoding = New-Object System.Text.UTF8Encoding($false)
$clock = [Diagnostics.Stopwatch]::StartNew()
$sequence = 0
$completedNormally = $false
$monitorFailure = $null
try {
    while ($clock.Elapsed.TotalSeconds -lt $requiredSeconds) {
        $sequence += 1
        $observed = [DateTimeOffset]::UtcNow
        $state = Invoke-KitsuAdbSafe -Arguments @('get-state')
        $deviceOnline = $state.ok -and $state.text -eq 'device'
        $pid = $null
        $batteryLevel = $null
        $batteryTemperatureC = $null
        $bluetoothEnabled = $null
        $wifiEnabled = $null
        if ($deviceOnline) {
            $pidResult = Invoke-KitsuAdbSafe -Arguments @('shell', 'pidof', $PackageName)
            if ($pidResult.ok -and $pidResult.text -match '^[0-9]+(?:\s+[0-9]+)*$') {
                $pid = $pidResult.text
            }
            $battery = Invoke-KitsuAdbSafe -Arguments @('shell', 'dumpsys', 'battery')
            if ($battery.text -match '(?m)^\s*level:\s*([0-9]+)\s*$') {
                $batteryLevel = [int]$Matches[1]
            }
            if ($battery.text -match '(?m)^\s*temperature:\s*([0-9]+)\s*$') {
                $batteryTemperatureC = [Math]::Round(([int]$Matches[1]) / 10.0, 1)
            }
            $bluetooth = Invoke-KitsuAdbSafe `
                -Arguments @('shell', 'settings', 'get', 'global', 'bluetooth_on')
            if ($bluetooth.text -match '^[01]$') { $bluetoothEnabled = $bluetooth.text -eq '1' }
            $wifi = Invoke-KitsuAdbSafe -Arguments @('shell', 'settings', 'get', 'global', 'wifi_on')
            if ($wifi.text -match '^[01]$') { $wifiEnabled = $wifi.text -eq '1' }
        }
        $tcp = $null
        if ($ProbeGatewayTcp -and (($sequence - 1) % 5 -eq 0)) {
            $tcp = [ordered]@{
                bootstrap = Test-KitsuTcpPort -HostName ([string]$acceptance.gateway.route_host) `
                    -Port ([int]$acceptance.gateway.bootstrap_port)
                steady_mtls = Test-KitsuTcpPort -HostName ([string]$acceptance.gateway.route_host) `
                    -Port ([int]$acceptance.gateway.steady_mtls_port)
            }
        }
        $sample = [ordered]@{
            schema = $script:KitsuReliabilitySampleSchema
            run_id = $runId
            sequence = $sequence
            observed_at_utc = $observed.ToString('o')
            monotonic_elapsed_seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
            android = [ordered]@{
                adb_online = $deviceOnline
                app_process_ids = $pid
                battery_level_percent = $batteryLevel
                battery_temperature_c = $batteryTemperatureC
                bluetooth_adapter_enabled = $bluetoothEnabled
                wifi_adapter_enabled = $wifiEnabled
            }
            backend_health = Test-KitsuHttpHealth -Uri $BackendHealthUri
            gateway_health = Invoke-KitsuGatewayHealthProbe -Uri $GatewayHealthUri `
                -TimeoutSeconds $ProbeTimeoutSeconds `
                -ExpectedGatewayId ([string]$acceptance.gateway.gateway_id) `
                -ExpectedExposure ([string]$acceptance.gateway.exposure)
            gateway_tcp = $tcp
        }
        $json = ConvertTo-KitsuJson -InputObject $sample
        [System.IO.File]::AppendAllText($samplesPath, $json + [Environment]::NewLine, $encoding)

        $remaining = $requiredSeconds - $clock.Elapsed.TotalSeconds
        if ($remaining -gt 0) {
            Start-Sleep -Seconds ([Math]::Min($SampleIntervalSeconds, [Math]::Ceiling($remaining)))
        }
    }
    $completedNormally = $true
} catch {
    $monitorFailure = $_.Exception.GetType().Name
} finally {
    $clock.Stop()
    $finished = [DateTimeOffset]::UtcNow
    $status = if ($completedNormally -and
        $clock.Elapsed.TotalSeconds -ge $script:KitsuMinimumReliabilitySeconds -and
        ($finished - $started).TotalSeconds -ge $script:KitsuMinimumReliabilitySeconds) {
        'COMPLETED'
    } else { 'INTERRUPTED' }
    $completion = [ordered]@{
        schema = $script:KitsuReliabilityCompletionSchema
        run_id = $runId
        acceptance_record_sha256 = $acceptanceHash
        status = $status
        completed_at_utc = $finished.ToString('o')
        monotonic_elapsed_seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
        wall_elapsed_seconds = [Math]::Round(($finished - $started).TotalSeconds, 3)
        sample_count = $sequence
        samples_sha256 = Get-KitsuSha256 -LiteralPath $samplesPath
        interruption_type = $monitorFailure
    }
    Write-KitsuNewJson -LiteralPath $completionPath -InputObject $completion
}

if (-not $completedNormally) {
    throw "Reliability monitoring did not complete; evidence was preserved at $runDirectory"
}
$validation = Test-KitsuReliabilityRun -RunDirectory $runDirectory `
    -AcceptanceRecordPath $acceptancePath
if (-not $validation.valid) {
    throw ('Reliability duration completed but failed closed: ' + (@($validation.errors) -join '; '))
}
Write-Host "Reliability run completed at $runDirectory"
