[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-common.ps1')

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) { throw $Message }
}

$parseFailures = New-Object System.Collections.Generic.List[string]
foreach ($scriptFile in @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File)) {
    $tokens = $null
    $parseErrors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        $scriptFile.FullName,
        [ref]$tokens,
        [ref]$parseErrors
    )
    foreach ($parseError in @($parseErrors)) {
        $parseFailures.Add("$($scriptFile.Name): $($parseError.Message)")
    }
}
if ($parseFailures.Count -gt 0) {
    throw ($parseFailures -join [Environment]::NewLine)
}

$tempBase = Get-KitsuFullPath -Path ([System.IO.Path]::GetTempPath())
$testRoot = Get-KitsuFullPath -Path (Join-Path $tempBase (
    'kitsu-acceptance-tests-' + [Guid]::NewGuid().ToString('N')
))
[void](Assert-KitsuPathWithin -BaseDirectory $tempBase -CandidatePath $testRoot)
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $acceptance = [ordered]@{
        schema = $script:KitsuAcceptanceSchema
        status = 'IN_PROGRESS'
        started_at_utc = '2026-08-20T00:00:00Z'
        scope = [ordered]@{
            meshcore_physical_proof = $false
            gateway_exposure = 'public'
        }
        apk = [ordered]@{
            package_name = 'app.kitsu.mobile'
            sha256 = ('0' * 64)
            signing_certificate_sha256 = ('1' * 64)
        }
        source_provenance = [ordered]@{ tree_sha256 = ('2' * 64) }
        firmware = [ordered]@{ bundle_sha256 = ('3' * 64); release_id = 'firmware-test' }
        backend = [ordered]@{
            base_url = 'https://api.example.test/'
            health_uri = 'https://api.example.test/health/live'
            release_id = 'backend-test'
        }
        gateway = [ordered]@{
            release_id = 'gateway-test'
            gateway_id = '00112233-4455-6677-8899-aabbccddeeff'
            exposure = 'public'
            route_host = 'gateway.example.test'
            tls_server_name = 'gateway.example.test'
            health_uri = 'https://gateway.example.test/health/live'
            health_transport = 'direct'
            bootstrap_port = 7442
            steady_mtls_port = 7443
        }
        heltec = [ordered]@{ expected_device_uid = 'KTTEST' }
        android_device = [ordered]@{ adb_serial = 'physical-test-device' }
        reliability_policy = [ordered]@{
            minimum_duration_seconds = $script:KitsuMinimumReliabilitySeconds
            minimum_sample_coverage_ratio = $script:KitsuMinimumReliabilitySampleCoverageRatio
            minimum_health_ratio = $script:KitsuMinimumReliabilityHealthRatio
            maximum_consecutive_unhealthy_samples = $script:KitsuMaximumConsecutiveUnhealthySamples
            minimum_terminal_healthy_samples = $script:KitsuMinimumTerminalHealthySamples
            terminal_recovery_required = $true
            linked_manual_review_required = $true
        }
    }
    Write-KitsuNewJson -LiteralPath (Join-Path $testRoot 'acceptance-record.json') `
        -InputObject $acceptance

    $fakeAdb = Join-Path $testRoot 'fake-adb.ps1'
    Write-KitsuNewUtf8Text -LiteralPath $fakeAdb -Text @'
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$ToolArguments)
if ($ToolArguments[0] -eq 'devices') {
    Write-Output 'List of devices attached'
    Write-Output 'emulator-5554 device product:sdk_gphone model:Virtual_Device'
    exit 0
}
exit 1
'@
    $dummyApk = Join-Path $testRoot 'dummy.apk.txt'
    $dummySigner = Join-Path $testRoot 'fake-apksigner.ps1'
    $dummyAapt = Join-Path $testRoot 'fake-aapt.ps1'
    $dummyProvenance = Join-Path $testRoot 'source-provenance.fixture.txt'
    $dummyFirmwareRelease = Join-Path $testRoot 'firmware-release.fixture.txt'
    $dummyFirmwareBundle = Join-Path $testRoot 'firmware-bundle.fixture.txt'
    foreach ($dummy in @(
        $dummyApk, $dummySigner, $dummyAapt, $dummyProvenance,
        $dummyFirmwareRelease, $dummyFirmwareBundle
    )) {
        Write-KitsuNewUtf8Text -LiteralPath $dummy -Text 'fixture'
    }
    $physicalRejectionRoot = Join-Path $testRoot 'emulator-rejection'
    $emulatorRejected = $false
    try {
        & (Join-Path $PSScriptRoot 'capture-physical-acceptance.ps1') `
            -EvidenceDirectory $physicalRejectionRoot -ApkPath $dummyApk -AdbPath $fakeAdb `
            -ApkSignerPath $dummySigner -AaptPath $dummyAapt `
            -SourceProvenancePath $dummyProvenance -FirmwareReleasePath $dummyFirmwareRelease `
            -FirmwareBundlePath $dummyFirmwareBundle -OperatorId test-operator `
            -ExpectedDeviceUid KTTEST `
            -ExpectedGatewayId '00112233-4455-6677-8899-aabbccddeeff' `
            -ExpectedGatewayReleaseId gateway-test -ExpectedBackendReleaseId backend-test `
            -ExpectedFirmwareReleaseId firmware-test -ExpectedFirmwareVersion 0.10.2 `
            -ExpectedGatewayHost gateway.example.test `
            -ExpectedGatewayServerName gateway.example.test `
            -ExpectedGatewayHealthUri 'https://gateway.example.test/health/live' `
            -ExpectedBootstrapPort 7442 `
            -ExpectedSteadyMtlsPort 7443 -GatewayExposure public `
            -BackendBaseUrl 'https://api.example.test/' -ExcludeMeshCorePhysicalProof | Out-Null
    } catch {
        $emulatorRejected = $_.Exception.Message -match 'Emulators|virtual'
    }
    Assert-True -Condition $emulatorRejected `
        -Message 'capture initializer did not reject an adb emulator serial'
    Assert-True -Condition (-not (Test-Path -LiteralPath `
        (Join-Path $physicalRejectionRoot 'acceptance-record.json') -PathType Leaf)) `
        -Message 'capture initializer wrote acceptance evidence for an emulator'

    $gatewayUriRejected = $false
    try {
        & (Join-Path $PSScriptRoot 'capture-physical-acceptance.ps1') `
            -EvidenceDirectory (Join-Path $testRoot 'gateway-uri-rejection') `
            -ApkPath $dummyApk -AdbPath $fakeAdb -ApkSignerPath $dummySigner `
            -AaptPath $dummyAapt -SourceProvenancePath $dummyProvenance `
            -FirmwareReleasePath $dummyFirmwareRelease -FirmwareBundlePath $dummyFirmwareBundle `
            -OperatorId test-operator -ExpectedDeviceUid KTTEST `
            -ExpectedGatewayId '00112233-4455-6677-8899-aabbccddeeff' `
            -ExpectedGatewayReleaseId gateway-test -ExpectedBackendReleaseId backend-test `
            -ExpectedFirmwareReleaseId firmware-test -ExpectedFirmwareVersion 0.10.2 `
            -ExpectedGatewayHost gateway.example.test `
            -ExpectedGatewayServerName gateway.example.test `
            -ExpectedGatewayHealthUri 'https://wrong.example.test/health/live' `
            -ExpectedBootstrapPort 7442 -ExpectedSteadyMtlsPort 7443 `
            -GatewayExposure public -BackendBaseUrl 'https://api.example.test/' `
            -ExcludeMeshCorePhysicalProof | Out-Null
    } catch {
        $gatewayUriRejected = $_.Exception.Message -match 'health URI|gateway route'
    }
    Assert-True -Condition $gatewayUriRejected `
        -Message 'capture initializer accepted a gateway health URI outside the frozen route'

    $repoEvidenceRejected = $false
    try {
        Assert-KitsuEvidenceOutsideRepository `
            -EvidenceDirectory (Join-Path $script:KitsuRepositoryRoot 'physical-evidence') | Out-Null
    } catch {
        $repoEvidenceRejected = $_.Exception.Message -match 'outside|disjoint'
    }
    Assert-True -Condition $repoEvidenceRejected `
        -Message 'evidence location guard accepted a repository-contained evidence path'

    $healthBody = [ordered]@{
        status = 'ok'
        protocol = 1
        gateway_id = '00112233-4455-6677-8899-aabbccddeeff'
        deployment_scope = 'public'
        backend_online = $true
    } | ConvertTo-Json -Compress
    $boundHealth = ConvertTo-KitsuGatewayHealthObservation -StatusCode 200 `
        -Content $healthBody -LatencyMilliseconds 4.2 `
        -ExpectedGatewayId '00112233-4455-6677-8899-aabbccddeeff' `
        -ExpectedExposure public
    Assert-True -Condition ([bool]$boundHealth.identity_verified) `
        -Message 'gateway health parser rejected the exact frozen gateway identity'
    Assert-True -Condition (Test-KitsuGatewayHealthObservationAccepted `
        -Observation $boundHealth `
        -ExpectedGatewayId '00112233-4455-6677-8899-aabbccddeeff' `
        -ExpectedExposure public) `
        -Message 'capture gateway gate rejected an exact healthy gateway observation'
    $wrongHealth = ConvertTo-KitsuGatewayHealthObservation -StatusCode 200 `
        -Content $healthBody -LatencyMilliseconds 4.2 `
        -ExpectedGatewayId 'ffffffff-ffff-ffff-ffff-ffffffffffff' `
        -ExpectedExposure public
    Assert-True -Condition (-not [bool]$wrongHealth.identity_verified) `
        -Message 'gateway health parser accepted a different gateway UUID'
    $stringProtocolBody = $healthBody -replace '"protocol":1', '"protocol":"1"'
    $stringProtocolHealth = ConvertTo-KitsuGatewayHealthObservation -StatusCode 200 `
        -Content $stringProtocolBody -LatencyMilliseconds 4.2 `
        -ExpectedGatewayId '00112233-4455-6677-8899-aabbccddeeff' `
        -ExpectedExposure public
    Assert-True -Condition (-not [bool]$stringProtocolHealth.identity_verified) `
        -Message 'gateway health parser accepted a string-coerced protocol version'
    $degradedHealth = ConvertTo-KitsuGatewayHealthObservation -StatusCode 200 `
        -Content ($healthBody -replace '"status":"ok"', '"status":"degraded"') `
        -LatencyMilliseconds 4.2 `
        -ExpectedGatewayId '00112233-4455-6677-8899-aabbccddeeff' `
        -ExpectedExposure public
    Assert-True -Condition (-not (Test-KitsuGatewayHealthObservationAccepted `
        -Observation $degradedHealth `
        -ExpectedGatewayId '00112233-4455-6677-8899-aabbccddeeff' `
        -ExpectedExposure public)) `
        -Message 'capture gateway gate accepted a degraded gateway status'
    $offlineBackendHealth = ConvertTo-KitsuGatewayHealthObservation -StatusCode 200 `
        -Content ($healthBody -replace '"backend_online":true', '"backend_online":false') `
        -LatencyMilliseconds 4.2 `
        -ExpectedGatewayId '00112233-4455-6677-8899-aabbccddeeff' `
        -ExpectedExposure public
    Assert-True -Condition (-not (Test-KitsuGatewayHealthObservationAccepted `
        -Observation $offlineBackendHealth `
        -ExpectedGatewayId '00112233-4455-6677-8899-aabbccddeeff' `
        -ExpectedExposure public)) `
        -Message 'capture gateway gate accepted backend_online=false'

    Assert-True -Condition (Test-KitsuBackendHealthUriAccepted `
        -CandidateUri 'https://api.example.test/health/live' `
        -FrozenHealthUri 'https://api.example.test/health/live' `
        -FrozenBaseUrl 'https://api.example.test/') `
        -Message 'backend gate rejected its exact frozen /health/live URI'
    Assert-True -Condition (-not (Test-KitsuBackendHealthUriAccepted `
        -CandidateUri 'https://api.example.test/static-always-200' `
        -FrozenHealthUri 'https://api.example.test/health/live' `
        -FrozenBaseUrl 'https://api.example.test/')) `
        -Message 'backend gate accepted an unrelated same-origin 2xx path'

    $junctionTree = Join-Path $testRoot 'junction-tree'
    $junctionTarget = Join-Path $testRoot 'junction-target'
    New-Item -ItemType Directory -Path $junctionTree | Out-Null
    New-Item -ItemType Directory -Path $junctionTarget | Out-Null
    $linkPath = Join-Path $junctionTree 'escape'
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        New-Item -ItemType Junction -Path $linkPath -Target $junctionTarget | Out-Null
    } else {
        New-Item -ItemType SymbolicLink -Path $linkPath -Target $junctionTarget | Out-Null
    }
    $junctionRejected = $false
    try {
        Assert-KitsuEvidenceTreeNoReparsePoints -EvidenceDirectory $junctionTree
    } catch {
        $junctionRejected = $_.Exception.Message -match 'reparse|junction|symbolic'
    }
    Assert-True -Condition $junctionRejected `
        -Message 'evidence tree guard accepted a junction or symbolic-link escape'
    Remove-Item -LiteralPath $linkPath -Force

    $recordScript = Join-Path $PSScriptRoot 'record-physical-case.ps1'

    $preAcceptanceRejected = $false
    try {
        & $recordScript -EvidenceDirectory $testRoot -CaseId 'chronology-negative' `
            -Title 'Chronology rejection' -Status BLOCKED -ExpectedResult 'Reject pre-run evidence.' `
            -ObservedResult 'Attempted pre-run timestamp.' `
            -StartedAtUtc '2026-08-19T23:00:00Z' -CompletedAtUtc '2026-08-19T23:01:00Z' | Out-Null
    } catch {
        $preAcceptanceRejected = $_.Exception.Message -match 'chronology|initialization'
    }
    Assert-True -Condition $preAcceptanceRejected `
        -Message 'case recorder accepted a pre-acceptance chronology'

    $futureRejected = $false
    try {
        & $recordScript -EvidenceDirectory $testRoot -CaseId 'future-negative' `
            -Title 'Future rejection' -Status BLOCKED -ExpectedResult 'Reject future evidence.' `
            -ObservedResult 'Attempted future timestamp.' `
            -StartedAtUtc '2999-01-01T00:00:00Z' -CompletedAtUtc '2999-01-01T00:01:00Z' | Out-Null
    } catch {
        $futureRejected = $_.Exception.Message -match 'chronology|future'
    }
    Assert-True -Condition $futureRejected `
        -Message 'case recorder accepted a future-dated chronology'

    $smallFutureRejected = $false
    $smallFutureStart = [DateTimeOffset]::UtcNow.AddMinutes(1)
    $smallFutureCompleted = $smallFutureStart.AddMinutes(1)
    try {
        & $recordScript -EvidenceDirectory $testRoot -CaseId 'future-pass-negative' `
            -Title 'Small future PASS rejection' -Status PASS `
            -ExpectedResult 'Reject any future PASS.' `
            -ObservedResult 'A near-future PASS was attempted.' `
            -StartedAtUtc $smallFutureStart.ToString('o') `
            -CompletedAtUtc $smallFutureCompleted.ToString('o') | Out-Null
    } catch {
        $smallFutureRejected = $_.Exception.Message -match 'chronology|future'
    }
    Assert-True -Condition $smallFutureRejected `
        -Message 'case recorder accepted a PASS only minutes in the future'

    $renamedBinary = Join-Path $testRoot 'renamed-firmware.png'
    Write-KitsuNewUtf8Text -LiteralPath $renamedBinary -Text 'PK fake archive payload'
    $magicRejected = $false
    try {
        & $recordScript -EvidenceDirectory $testRoot -CaseId 'attachment-negative' `
            -Title 'Attachment rejection' -Status BLOCKED -ExpectedResult 'Reject disguised binary.' `
            -ObservedResult 'A renamed archive was offered.' `
            -StartedAtUtc '2026-08-20T00:10:00Z' -CompletedAtUtc '2026-08-20T00:11:00Z' `
            -SanitizedAttachmentPath $renamedBinary -ConfirmAttachmentsSanitized | Out-Null
    } catch {
        $magicRejected = $_.Exception.Message -match 'content|media|allow-list'
    }
    Assert-True -Condition $magicRejected `
        -Message 'attachment allow-list accepted extension-only disguised content'

    $validPng = Join-Path $testRoot 'sanitized-screen.png'
    [System.IO.File]::WriteAllBytes(
        $validPng,
        [Convert]::FromBase64String(
            'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII='
        )
    )
    $validAttachment = Get-KitsuSanitizedAttachmentInfo -LiteralPath $validPng
    Assert-True -Condition ([string]$validAttachment.media_type -ceq 'image/png') `
        -Message 'attachment allow-list rejected a byte-valid PNG'

    $caseOutput = @(& $recordScript -EvidenceDirectory $testRoot -CaseId 'install-upgrade' `
        -Title 'Signed upgrade' -Status PASS -ExpectedResult 'Upgrade is retained.' `
        -ObservedResult 'Upgrade identity and retained state were verified.' `
        -StartedAtUtc '2026-08-20T01:00:00Z' -CompletedAtUtc '2026-08-20T01:05:00Z' `
        -SanitizedAttachmentPath $validPng -ConfirmAttachmentsSanitized)
    $caseResultPath = [string]$caseOutput[-1]
    Assert-True -Condition (Test-Path -LiteralPath $caseResultPath -PathType Leaf) `
        -Message 'write-once case recorder did not create case-result.json'
    $caseRecord = Read-KitsuJson -LiteralPath $caseResultPath
    Assert-True -Condition ($caseRecord.status -eq 'PASS') -Message 'case result status changed'

    & $recordScript -EvidenceDirectory $testRoot -CaseId 'install-upgrade' `
        -Title 'Tied upgrade result' -Status FAIL -ExpectedResult 'Completion order is unique.' `
        -ObservedResult 'A conflicting result used the same completion timestamp.' `
        -StartedAtUtc '2026-08-20T01:01:00Z' -CompletedAtUtc '2026-08-20T01:05:00Z' `
        -DefectSeverity 3 -DefectId 'TIED-UPGRADE' | Out-Null
    & $recordScript -EvidenceDirectory $testRoot -CaseId 'defect-tie-open' `
        -Title 'Open critical defect' -Status FAIL -ExpectedResult 'Critical defect is opened.' `
        -ObservedResult 'Critical defect was observed.' `
        -StartedAtUtc '2026-08-20T01:15:00Z' -CompletedAtUtc '2026-08-20T01:20:00Z' `
        -DefectSeverity 1 -DefectId 'TIED-CRITICAL' | Out-Null
    & $recordScript -EvidenceDirectory $testRoot -CaseId 'defect-tie-resolve' `
        -Title 'Resolve critical defect' -Status PASS -ExpectedResult 'Resolution is ordered.' `
        -ObservedResult 'Resolution used the opening completion timestamp.' `
        -StartedAtUtc '2026-08-20T01:18:00Z' -CompletedAtUtc '2026-08-20T01:20:00Z' `
        -ResolvedDefectId 'TIED-CRITICAL' | Out-Null

    $unlinkedReviewRejected = $false
    try {
        & $recordScript -EvidenceDirectory $testRoot -CaseId 'reliability-review' `
            -Title 'Reliability review' -Status PASS -ExpectedResult 'Review is linked.' `
            -ObservedResult 'No run ID was supplied.' `
            -StartedAtUtc '2026-08-20T01:10:00Z' -CompletedAtUtc '2026-08-20T01:11:00Z' | Out-Null
    } catch {
        $unlinkedReviewRejected = $_.Exception.Message -match 'run ID|reliability'
    }
    Assert-True -Condition $unlinkedReviewRejected `
        -Message 'case recorder accepted an unlinked reliability-review result'

    $secretRejected = $false
    try {
        & $recordScript -EvidenceDirectory $testRoot -CaseId 'secret-negative' `
            -Title 'Secret rejection' -Status BLOCKED -ExpectedResult 'Evidence is rejected.' `
            -ObservedResult 'access_token: abcdefghijklmnopqrstuvwxyz012345' `
            -StartedAtUtc '2026-08-20T01:00:00Z' -CompletedAtUtc '2026-08-20T01:01:00Z' | Out-Null
    } catch {
        $secretRejected = $true
    }
    Assert-True -Condition $secretRejected -Message 'labelled token material was not rejected'

    $runId = '20260820T020000000Z-001122aabbcc'
    $runDirectory = Join-Path (Join-Path $testRoot 'reliability') $runId
    New-Item -ItemType Directory -Path $runDirectory | Out-Null
    $runStart = [DateTimeOffset]::Parse('2026-08-20T02:00:00Z')
    Write-KitsuNewJson -LiteralPath (Join-Path $runDirectory 'start.json') -InputObject ([ordered]@{
        schema = $script:KitsuReliabilityStartSchema
        run_id = $runId
        acceptance_record_sha256 = Get-KitsuSha256 -LiteralPath (Join-Path $testRoot 'acceptance-record.json')
        started_at_utc = $runStart.ToString('o')
        required_duration_seconds = 86400
        sample_interval_seconds = 60
        android_serial = 'physical-test-device'
        package_name = 'app.kitsu.mobile'
        backend_health_uri = 'https://api.example.test/health/live'
        backend_release_id = 'backend-test'
        gateway_health_uri = 'https://gateway.example.test/health/live'
        gateway_health_transport = 'direct'
        gateway_id = '00112233-4455-6677-8899-aabbccddeeff'
        gateway_release_id = 'gateway-test'
        gateway_exposure = 'public'
        gateway_route_host = 'gateway.example.test'
        gateway_tls_server_name = 'gateway.example.test'
        gateway_tcp_probe = $false
        gateway_host = $null
        gateway_bootstrap_port = $null
        gateway_steady_mtls_port = $null
        minimum_sample_coverage_ratio = $script:KitsuMinimumReliabilitySampleCoverageRatio
        minimum_health_ratio = $script:KitsuMinimumReliabilityHealthRatio
        maximum_consecutive_unhealthy_samples = $script:KitsuMaximumConsecutiveUnhealthySamples
        minimum_terminal_healthy_samples = $script:KitsuMinimumTerminalHealthySamples
        terminal_recovery_required = $true
    })
    $sampleLines = New-Object System.Collections.Generic.List[string]
    for ($index = 0; $index -le 1440; $index += 1) {
        # This fixture starts healthy and makes only its final observation healthy.
        # The old one-sample terminal gate would accept that edge; the five-sample
        # terminal recovery requirement must reject it.
        $edgeHealthy = $index -eq 0 -or $index -eq 1440
        $sampleLines.Add((ConvertTo-KitsuJson -InputObject ([ordered]@{
            schema = $script:KitsuReliabilitySampleSchema
            run_id = $runId
            sequence = $index + 1
            observed_at_utc = $runStart.AddSeconds($index * 60).ToString('o')
            monotonic_elapsed_seconds = [double]($index * 60)
            android = [ordered]@{
                adb_online = $edgeHealthy
                app_process_ids = if ($edgeHealthy) { '1234' } else { $null }
            }
            backend_health = [ordered]@{
                reachable = $edgeHealthy
                status_code = if ($edgeHealthy) { 200 } else { $null }
                latency_ms = 0.0
            }
            gateway_health = [ordered]@{
                reachable = $edgeHealthy
                status_code = if ($edgeHealthy) { 200 } else { $null }
                latency_ms = 0.0
                identity_verified = $edgeHealthy
                gateway_id = if ($edgeHealthy) {
                    '00112233-4455-6677-8899-aabbccddeeff'
                } else { $null }
                deployment_scope = if ($edgeHealthy) { 'public' } else { $null }
                protocol = if ($edgeHealthy) { 1 } else { $null }
                reported_status = if ($edgeHealthy) { 'ok' } else { $null }
                backend_online = if ($edgeHealthy) { $true } else { $null }
            }
            gateway_tcp = $null
        })))
    }
    $samplesPath = Join-Path $runDirectory 'samples.jsonl'
    Write-KitsuNewUtf8Text -LiteralPath $samplesPath `
        -Text (($sampleLines -join [Environment]::NewLine) + [Environment]::NewLine)
    Write-KitsuNewJson -LiteralPath (Join-Path $runDirectory 'completion.json') -InputObject ([ordered]@{
        schema = $script:KitsuReliabilityCompletionSchema
        run_id = $runId
        acceptance_record_sha256 = Get-KitsuSha256 -LiteralPath (Join-Path $testRoot 'acceptance-record.json')
        status = 'COMPLETED'
        completed_at_utc = $runStart.AddHours(24).ToString('o')
        monotonic_elapsed_seconds = 86400
        wall_elapsed_seconds = 86400
        sample_count = 1441
        samples_sha256 = Get-KitsuSha256 -LiteralPath $samplesPath
        interruption_type = $null
    })
    $reviewOutput = @(& $recordScript -EvidenceDirectory $testRoot `
        -CaseId 'reliability-review' -ReliabilityRunId $runId `
        -Title 'Reliability review' -Status PASS `
        -ExpectedResult 'Review is bound to the exact reliability evidence.' `
        -ObservedResult 'The selected run evidence was reviewed.' `
        -StartedAtUtc '2026-08-21T02:01:00Z' -CompletedAtUtc '2026-08-21T02:05:00Z')
    $reviewRecord = Read-KitsuJson -LiteralPath ([string]$reviewOutput[-1])
    Assert-True -Condition (
        [string]$reviewRecord.reliability_evidence.samples_sha256 -ceq
            (Get-KitsuSha256 -LiteralPath $samplesPath) -and
        [string]$reviewRecord.reliability_evidence.completion_sha256 -ceq
            (Get-KitsuSha256 -LiteralPath (Join-Path $runDirectory 'completion.json'))
    ) -Message 'reliability review was not digest-bound to samples and completion evidence'
    $reliability = Test-KitsuReliabilityRun -RunDirectory $runDirectory `
        -AcceptanceRecordPath (Join-Path $testRoot 'acceptance-record.json')
    Assert-True -Condition (-not [bool]$reliability.valid) `
        -Message 'synthetic fully offline reliability evidence passed the validator'
    $reliabilityErrors = @($reliability.errors) -join '; '
    Assert-True -Condition ($reliabilityErrors -match 'filesystem chronology') `
        -Message 'synthetic instant fixture was not rejected by filesystem chronology'
    Assert-True -Condition ($reliabilityErrors -match 'below 99 percent') `
        -Message 'fully offline samples were not rejected by the frozen health threshold'
    Assert-True -Condition ($reliabilityErrors -match 'end with five healthy observations') `
        -Message 'reliability validator accepted a single final healthy observation'

    $abandonedPending = Join-Path (Join-Path (Join-Path $testRoot 'cases') 'install-upgrade') `
        '.pending-20260820T030000000Z-001122aabbcc-deadbeefcafe'
    New-Item -ItemType Directory -Path $abandonedPending | Out-Null

    $finalizerRejectedIncompleteRun = $false
    try {
        & (Join-Path $PSScriptRoot 'finalize-physical-acceptance.ps1') `
            -EvidenceDirectory $testRoot | Out-Null
    } catch {
        $finalizerRejectedIncompleteRun = $true
    }
    Assert-True -Condition $finalizerRejectedIncompleteRun `
        -Message 'finalizer accepted an evidence tree with missing required physical cases'
    $decisions = @(Get-ChildItem -LiteralPath (Join-Path $testRoot 'decisions') `
        -Recurse -Filter decision.json -File)
    Assert-True -Condition ($decisions.Count -eq 1) `
        -Message 'failed finalization did not preserve exactly one new decision'
    $decision = Read-KitsuJson -LiteralPath $decisions[0].FullName
    Assert-True -Condition ($decision.status -eq 'FAIL') `
        -Message 'incomplete evidence did not produce a FAIL decision'
    Assert-True -Condition ((@($decision.unresolved_errors) -join '; ') -notmatch
        'incomplete case attempt|unexpected case attempt directory.*pending') `
        -Message 'abandoned atomic staging directory became a permanent finalizer failure'
    $decisionErrors = @($decision.unresolved_errors) -join '; '
    Assert-True -Condition ($decisionErrors -match
        'case has ambiguous equal completion timestamps: install-upgrade') `
        -Message 'finalizer did not reject tied case completion timestamps'
    Assert-True -Condition ($decisionErrors -match
        'defect lifecycle has ambiguous equal completion timestamps: TIED-CRITICAL') `
        -Message 'finalizer did not reject tied critical-defect lifecycle timestamps'
    Assert-True -Condition (
        [string]$decision.acceptance_record_sha256 -ceq
            (Get-KitsuSha256 -LiteralPath (Join-Path $testRoot 'acceptance-record.json')) -and
        [string]$decision.reliability.samples_sha256 -ceq
            (Get-KitsuSha256 -LiteralPath $samplesPath) -and
        [string]$decision.reliability.completion_sha256 -ceq
            (Get-KitsuSha256 -LiteralPath (Join-Path $runDirectory 'completion.json'))
    ) -Message 'final decision omitted the frozen acceptance or reliability evidence digests'
} finally {
    $validatedRoot = Assert-KitsuPathWithin -BaseDirectory $tempBase -CandidatePath $testRoot
    if (Test-Path -LiteralPath $validatedRoot -PathType Container) {
        Remove-Item -LiteralPath $validatedRoot -Recurse -Force
    }
}

Write-Host 'KITSU_ACCEPTANCE_HARNESS_TEST_PASS'
