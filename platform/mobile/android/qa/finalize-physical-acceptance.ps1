[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$EvidenceDirectory,

    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')]
    [string[]]$AdditionalRequiredCaseId = @()
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-common.ps1')

$evidenceRoot = Assert-KitsuEvidenceOutsideRepository `
    -EvidenceDirectory (Resolve-Path -LiteralPath $EvidenceDirectory).Path
Assert-KitsuEvidenceTreeNoReparsePoints -EvidenceDirectory $evidenceRoot
$errors = New-Object System.Collections.Generic.List[string]
$acceptancePath = Join-Path $evidenceRoot 'acceptance-record.json'
try {
    $acceptance = Read-KitsuJson -LiteralPath $acceptancePath
} catch {
    throw 'The evidence root has no valid acceptance-record.json.'
}
if ($acceptance.schema -ne $script:KitsuAcceptanceSchema -or $acceptance.status -ne 'IN_PROGRESS') {
    throw 'The evidence root is not an in-progress Kitsu physical acceptance record.'
}
$acceptanceHash = Get-KitsuSha256 -LiteralPath $acceptancePath
$acceptanceStarted = ConvertTo-KitsuUtcDateTimeOffset -Value $acceptance.started_at_utc
$finalizerNow = [DateTimeOffset]::UtcNow
if ($acceptanceStarted -gt $finalizerNow) {
    $errors.Add('acceptance initialization is future-dated')
}

$requiredCases = New-Object System.Collections.Generic.List[string]
@(
    'install-upgrade',
    'install-fresh-second-device',
    'portrait-small-accessibility',
    'portrait-large-accessibility',
    'launcher-fox-head',
    'pairing-success',
    'pairing-negative',
    'ble-status-history',
    'ble-care-actions',
    'explicit-disconnect',
    'wifi-provisioning',
    'gateway-catalog',
    'owner-enrollment',
    'gateway-bootstrap-mtls',
    'backend-snapshot-binding',
    'remote-fallback',
    'authentication-lifecycle',
    'reliability-review',
    'reflash-recovery',
    'severity-review'
) | ForEach-Object { $requiredCases.Add($_) }
if ([string]$acceptance.scope.gateway_exposure -eq 'public') {
    $requiredCases.Add('public-gateway-perimeter')
}
if ([bool]$acceptance.scope.meshcore_physical_proof) {
    @(
        'meshcore-advert-map',
        'meshcore-direct-message',
        'meshcore-channel-message',
        'meshcore-repeater-interoperability'
    ) | ForEach-Object { $requiredCases.Add($_) }
}
foreach ($caseId in $AdditionalRequiredCaseId) {
    Assert-KitsuSafeIdentifier -Value $caseId -Label 'additional required case ID' `
        -Pattern '^[a-z0-9][a-z0-9._-]{0,63}$'
    if (-not $requiredCases.Contains($caseId)) { $requiredCases.Add($caseId) }
}

$attemptsByCase = @{}
$allAttempts = New-Object System.Collections.Generic.List[object]
$casesRoot = Join-Path $evidenceRoot 'cases'
if (-not (Test-Path -LiteralPath $casesRoot -PathType Container)) {
    $errors.Add('no physical case evidence was recorded')
} else {
    foreach ($caseDirectory in @(Get-ChildItem -LiteralPath $casesRoot -Directory)) {
        if ($caseDirectory.Name -notmatch '^[a-z0-9][a-z0-9._-]{0,63}$') {
            $errors.Add("unexpected case directory: $($caseDirectory.Name)")
            continue
        }
        foreach ($attemptDirectory in @(Get-ChildItem -LiteralPath $caseDirectory.FullName -Directory)) {
            if ($attemptDirectory.Name -match '^\.pending-[A-Za-z0-9._-]+$') {
                continue
            }
            if ($attemptDirectory.Name -notmatch '^[0-9]{8}T[0-9]{9}Z-[0-9a-f]{12}$') {
                $errors.Add("unexpected case attempt directory: $($caseDirectory.Name)/$($attemptDirectory.Name)")
                continue
            }
            $resultPath = Join-Path $attemptDirectory.FullName 'case-result.json'
            if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
                $errors.Add("incomplete case attempt: $($caseDirectory.Name)/$($attemptDirectory.Name)")
                continue
            }
            try {
                $record = Read-KitsuJson -LiteralPath $resultPath
                if ($record.schema -ne $script:KitsuCaseSchema -or
                    $record.case_id -ne $caseDirectory.Name -or
                    $record.attempt_id -ne $attemptDirectory.Name -or
                    $record.status -notin @('PASS', 'FAIL', 'BLOCKED')) {
                    throw 'record identity mismatch'
                }
                if ([string]$record.acceptance_record_sha256 -cne $acceptanceHash) {
                    throw 'record is not bound to the frozen acceptance digest'
                }
                $started = ConvertTo-KitsuUtcDateTimeOffset -Value $record.started_at_utc
                $completed = ConvertTo-KitsuUtcDateTimeOffset -Value $record.completed_at_utc
                if ($started -lt $acceptanceStarted -or $completed -lt $started -or
                    $completed -gt $finalizerNow) {
                    throw 'record chronology is outside the physical acceptance window'
                }
                foreach ($attachment in @($record.attachments)) {
                    $attachmentPath = Assert-KitsuPathWithin -BaseDirectory $attemptDirectory.FullName `
                        -CandidatePath (Join-Path (Join-Path $attemptDirectory.FullName 'attachments') `
                            ([string]$attachment.file_name))
                    if ([string]$attachment.file_name -notmatch '^[0-9]{2}-[A-Za-z0-9._-]+$' -or
                        -not (Test-Path -LiteralPath $attachmentPath -PathType Leaf) -or
                        (Get-Item -LiteralPath $attachmentPath).Length -ne [long]$attachment.bytes -or
                        (Get-KitsuSha256 -LiteralPath $attachmentPath) -ne [string]$attachment.sha256) {
                        throw 'attachment identity mismatch'
                    }
                    $attachmentInfo = Get-KitsuSanitizedAttachmentInfo -LiteralPath $attachmentPath
                    if ([string]$attachmentInfo.media_type -cne [string]$attachment.media_type -or
                        $attachment.operator_confirmed_sanitized -isnot [bool] -or
                        -not [bool]$attachment.operator_confirmed_sanitized) {
                        throw 'attachment content type or sanitization attestation mismatch'
                    }
                }
                $attempt = [pscustomobject]@{
                    case_id = [string]$record.case_id
                    attempt_id = [string]$record.attempt_id
                    status = [string]$record.status
                    completed = $completed
                    record = $record
                }
                $allAttempts.Add($attempt)
                if (-not $attemptsByCase.ContainsKey($attempt.case_id)) {
                    $attemptsByCase[$attempt.case_id] = New-Object System.Collections.Generic.List[object]
                }
                $attemptsByCase[$attempt.case_id].Add($attempt)
            } catch {
                $errors.Add("invalid case attempt: $($caseDirectory.Name)/$($attemptDirectory.Name)")
            }
        }
    }
}

$ambiguousCompletionByCase = @{}
foreach ($caseId in @($attemptsByCase.Keys)) {
    $tiedCompletionGroups = @(
        $attemptsByCase[$caseId] |
            Group-Object -Property { $_.completed.UtcTicks } |
            Where-Object { $_.Count -gt 1 }
    )
    if ($tiedCompletionGroups.Count -gt 0) {
        $ambiguousCompletionByCase[$caseId] = $true
        $errors.Add("case has ambiguous equal completion timestamps: $caseId")
    }
}

$caseSummary = New-Object System.Collections.Generic.List[object]
foreach ($caseId in $requiredCases) {
    if (-not $attemptsByCase.ContainsKey($caseId)) {
        $errors.Add("required case has no evidence: $caseId")
        $caseSummary.Add([ordered]@{ case_id = $caseId; latest_status = 'MISSING'; attempts = 0 })
        continue
    }
    $ordered = @($attemptsByCase[$caseId] | Sort-Object completed, attempt_id)
    $latest = $ordered[-1]
    if ($latest.status -ne 'PASS') {
        $errors.Add("required case did not end in PASS: $caseId")
    }
    $caseSummary.Add([ordered]@{
        case_id = $caseId
        latest_status = $latest.status
        attempts = $ordered.Count
        latest_completed_at_utc = $latest.completed.ToUniversalTime().ToString('o')
        completion_timestamp_ambiguous = $ambiguousCompletionByCase.ContainsKey($caseId)
    })
}

$openedCriticalDefects = @{}
foreach ($attempt in @($allAttempts | Sort-Object completed)) {
    if ($attempt.status -eq 'FAIL' -and $null -ne $attempt.record.defect -and
        [int]$attempt.record.defect.severity -in @(1, 2)) {
        $defectId = [string]$attempt.record.defect.id
        if ($openedCriticalDefects.ContainsKey($defectId)) {
            $errors.Add("severity-1/2 defect ID was opened more than once: $defectId")
        } else {
            $openedCriticalDefects[$defectId] = $attempt.completed
        }
    }
}
foreach ($defectId in @($openedCriticalDefects.Keys)) {
    $openedAt = [DateTimeOffset]$openedCriticalDefects[$defectId]
    $defectEvents = @($allAttempts | Where-Object {
        ($_.status -eq 'FAIL' -and $null -ne $_.record.defect -and
            [string]$_.record.defect.id -ceq $defectId) -or
        (@($_.record.resolved_defect_ids) -contains $defectId)
    })
    $tiedDefectEvents = @(
        $defectEvents |
            Group-Object -Property { $_.completed.UtcTicks } |
            Where-Object { $_.Count -gt 1 }
    )
    if ($tiedDefectEvents.Count -gt 0) {
        $errors.Add("defect lifecycle has ambiguous equal completion timestamps: $defectId")
    }
    $resolved = @($allAttempts | Where-Object {
        $_.status -eq 'PASS' -and $_.completed -gt $openedAt -and
        @($_.record.resolved_defect_ids) -contains $defectId
    }).Count -gt 0
    if (-not $resolved) {
        $errors.Add("unresolved severity-1/2 defect: $defectId")
    }
}

$reliabilitySummary = [ordered]@{
    status = 'MISSING'
    valid = $false
    acceptance_record_sha256 = $acceptanceHash
    samples_sha256 = $null
    completion_sha256 = $null
    errors = @('no run')
}
$latestRun = $null
$reliabilityCompleted = [DateTimeOffset]::MinValue
$reliabilitySamplesHash = $null
$reliabilityCompletionHash = $null
$reliabilityRoot = Join-Path $evidenceRoot 'reliability'
if (Test-Path -LiteralPath $reliabilityRoot -PathType Container) {
    $runs = @(Get-ChildItem -LiteralPath $reliabilityRoot -Directory | Sort-Object Name)
    if ($runs.Count -gt 0) {
        $latestRun = $runs[-1]
        try {
            $validation = Test-KitsuReliabilityRun -RunDirectory $latestRun.FullName `
                -AcceptanceRecordPath $acceptancePath
        } catch {
            $validation = [pscustomobject]@{
                valid = $false
                errors = @('reliability structure could not be validated')
                sample_count = 0
                elapsed_seconds = 0
                health = [ordered]@{}
            }
        }
        try {
            $reliabilityCompletion = Read-KitsuJson `
                -LiteralPath (Join-Path $latestRun.FullName 'completion.json')
            $reliabilitySamplesHash = Get-KitsuSha256 `
                -LiteralPath (Join-Path $latestRun.FullName 'samples.jsonl')
            $reliabilityCompletionHash = Get-KitsuSha256 `
                -LiteralPath (Join-Path $latestRun.FullName 'completion.json')
            $reliabilityCompleted = ConvertTo-KitsuUtcDateTimeOffset `
                -Value $reliabilityCompletion.completed_at_utc
        } catch {
            $reliabilityCompleted = [DateTimeOffset]::MaxValue
        }
        $reliabilitySummary = [ordered]@{
            run_id = $latestRun.Name
            status = if ($validation.valid) { 'PASS' } else { 'FAIL' }
            valid = [bool]$validation.valid
            acceptance_record_sha256 = $acceptanceHash
            samples_sha256 = $reliabilitySamplesHash
            completion_sha256 = $reliabilityCompletionHash
            sample_count = [int]$validation.sample_count
            elapsed_seconds = [double]$validation.elapsed_seconds
            health = $validation.health
            errors = @($validation.errors)
        }
        if (-not $validation.valid) {
            foreach ($message in @($validation.errors)) {
                $errors.Add("reliability: $message")
            }
        }
    } else {
        $errors.Add('no reliability run was recorded')
    }
} else {
    $errors.Add('no reliability run was recorded')
}

if ($null -ne $latestRun -and $attemptsByCase.ContainsKey('reliability-review')) {
    $reviews = @($attemptsByCase['reliability-review'] | Sort-Object completed, attempt_id)
    $latestReview = $reviews[-1]
    $bindingProperty = $latestReview.record.PSObject.Properties['reliability_evidence']
    $reviewBinding = if ($null -ne $bindingProperty) { $bindingProperty.Value } else { $null }
    if ($latestReview.status -ne 'PASS' -or
        [string]$latestReview.record.reliability_run_id -cne $latestRun.Name -or
        $null -eq $reviewBinding -or
        [string]$reviewBinding.run_id -cne $latestRun.Name -or
        [string]$reviewBinding.samples_sha256 -cne [string]$reliabilitySamplesHash -or
        [string]$reviewBinding.completion_sha256 -cne [string]$reliabilityCompletionHash -or
        $latestReview.completed -le $reliabilityCompleted) {
        $errors.Add('latest reliability-review PASS is not digest-bound to and later than the selected reliability run')
    }
}

$textExtensions = @('.txt', '.log', '.json', '.jsonl', '.csv', '.xml', '.md')
foreach ($file in @(Get-ChildItem -LiteralPath $evidenceRoot -Recurse -File)) {
    if ($textExtensions -notcontains $file.Extension.ToLowerInvariant()) { continue }
    if ($file.Length -gt 20MB) {
        $errors.Add("text evidence exceeds the final secret-scan limit: $($file.Name)")
        continue
    }
    try {
        Assert-KitsuNoSecrets -Text (Get-Content -Raw -LiteralPath $file.FullName) -Label $file.Name
    } catch {
        $errors.Add("prohibited secret-like material detected in: $($file.Name)")
    }
}

$decisionStatus = if ($errors.Count -eq 0) { 'PASS' } else { 'FAIL' }
$decisionId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' +
    [Guid]::NewGuid().ToString('N').Substring(0, 12)
$decisionRoot = Assert-KitsuPathWithin -BaseDirectory $evidenceRoot `
    -CandidatePath (Join-Path $evidenceRoot 'decisions')
$decisionDirectory = Assert-KitsuPathWithin -BaseDirectory $evidenceRoot `
    -CandidatePath (Join-Path $decisionRoot $decisionId)
New-Item -ItemType Directory -Path $decisionDirectory | Out-Null
$decision = [ordered]@{
    schema = 'kitsu.android-physical-acceptance-decision.v1'
    decision_id = $decisionId
    status = $decisionStatus
    decided_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
    acceptance_record_sha256 = $acceptanceHash
    frozen_identity = [ordered]@{
        apk_sha256 = [string]$acceptance.apk.sha256
        apk_signing_certificate_sha256 = [string]$acceptance.apk.signing_certificate_sha256
        source_tree_sha256 = [string]$acceptance.source_provenance.tree_sha256
        firmware_bundle_sha256 = [string]$acceptance.firmware.bundle_sha256
        firmware_release_id = [string]$acceptance.firmware.release_id
        backend_release_id = [string]$acceptance.backend.release_id
        backend_health_uri = [string]$acceptance.backend.health_uri
        gateway_release_id = [string]$acceptance.gateway.release_id
        gateway_id = [string]$acceptance.gateway.gateway_id
        gateway_health_uri = [string]$acceptance.gateway.health_uri
        expected_device_uid = [string]$acceptance.heltec.expected_device_uid
    }
    reliability_policy = $acceptance.reliability_policy
    scope = $acceptance.scope
    required_cases = $caseSummary.ToArray()
    reliability = $reliabilitySummary
    unresolved_errors = $errors.ToArray()
    statement = if ($decisionStatus -eq 'PASS') {
        'All required physical cases and the real-duration reliability gate passed for the frozen identities.'
    } else {
        'This evidence is not a production physical-acceptance pass.'
    }
}
$decisionPath = Join-Path $decisionDirectory 'decision.json'
Write-KitsuNewJson -LiteralPath $decisionPath -InputObject $decision
$sumLine = (Get-KitsuSha256 -LiteralPath $decisionPath) + '  decision.json' + [Environment]::NewLine
Write-KitsuNewUtf8Text -LiteralPath (Join-Path $decisionDirectory 'SHA256SUMS') -Text $sumLine

Write-Host "Physical acceptance decision: $decisionStatus"
Write-Output $decisionPath
if ($decisionStatus -ne 'PASS') {
    throw "Physical acceptance failed with $($errors.Count) unresolved requirement(s); the new decision file was preserved."
}
