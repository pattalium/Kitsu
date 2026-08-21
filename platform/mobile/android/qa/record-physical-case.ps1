[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$EvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-z0-9][a-z0-9._-]{0,63}$')]
    [string]$CaseId,

    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 160)]
    [string]$Title,

    [Parameter(Mandatory = $true)]
    [ValidateSet('PASS', 'FAIL', 'BLOCKED')]
    [string]$Status,

    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 2000)]
    [string]$ExpectedResult,

    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 4000)]
    [string]$ObservedResult,

    [Parameter(Mandatory = $true)]
    [string]$StartedAtUtc,

    [Parameter(Mandatory = $true)]
    [string]$CompletedAtUtc,

    [ValidateSet('none', '1', '2', '3', '4')]
    [string]$DefectSeverity = 'none',

    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')]
    [string]$DefectId,

    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')]
    [string[]]$ResolvedDefectId = @(),

    [ValidatePattern('^[0-9]{8}T[0-9]{9}Z-[0-9a-f]{12}$')]
    [string]$ReliabilityRunId,

    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string[]]$SanitizedAttachmentPath = @(),

    [switch]$ConfirmAttachmentsSanitized
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-common.ps1')

$evidenceRoot = Assert-KitsuEvidenceOutsideRepository `
    -EvidenceDirectory (Resolve-Path -LiteralPath $EvidenceDirectory).Path
Assert-KitsuEvidenceTreeNoReparsePoints -EvidenceDirectory $evidenceRoot
$acceptancePath = Join-Path $evidenceRoot 'acceptance-record.json'
$acceptance = Read-KitsuJson -LiteralPath $acceptancePath
if ($acceptance.schema -ne $script:KitsuAcceptanceSchema -or $acceptance.status -ne 'IN_PROGRESS') {
    throw 'The evidence root is not an in-progress Kitsu physical acceptance record.'
}
$acceptanceHash = Get-KitsuSha256 -LiteralPath $acceptancePath

$started = [DateTimeOffset]::MinValue
$completed = [DateTimeOffset]::MinValue
if (-not [DateTimeOffset]::TryParse($StartedAtUtc, [ref]$started) -or
    -not [DateTimeOffset]::TryParse($CompletedAtUtc, [ref]$completed) -or
    $started.Offset -ne [TimeSpan]::Zero -or $completed.Offset -ne [TimeSpan]::Zero -or
    $completed -lt $started) {
    throw 'StartedAtUtc and CompletedAtUtc must be ordered UTC timestamps with an explicit Z/+00:00 offset.'
}
$acceptanceStarted = ConvertTo-KitsuUtcDateTimeOffset -Value $acceptance.started_at_utc
if ($started.ToUniversalTime() -lt $acceptanceStarted -or
    $completed.ToUniversalTime() -gt [DateTimeOffset]::UtcNow) {
    throw 'Case chronology must begin after acceptance initialization and may not be future-dated.'
}
if ($CaseId -eq 'reliability-review' -and [string]::IsNullOrWhiteSpace($ReliabilityRunId)) {
    throw 'The reliability-review case must name the exact reliability run ID it reviewed.'
}
if ($CaseId -ne 'reliability-review' -and -not [string]::IsNullOrWhiteSpace($ReliabilityRunId)) {
    throw 'Only the reliability-review case may link a reliability run ID.'
}
if ($Status -eq 'FAIL' -and ($DefectSeverity -eq 'none' -or [string]::IsNullOrWhiteSpace($DefectId))) {
    throw 'A failed case must carry a stable defect ID and severity.'
}
if ($Status -ne 'FAIL' -and $DefectSeverity -ne 'none') {
    throw 'Only failed cases may declare a defect severity.'
}
if ($Status -ne 'FAIL' -and -not [string]::IsNullOrWhiteSpace($DefectId)) {
    throw 'Only failed cases may open a defect ID; use ResolvedDefectId on a later PASS.'
}

foreach ($text in @($Title, $ExpectedResult, $ObservedResult)) {
    Assert-KitsuNoSecrets -Text $text -Label "case $CaseId"
}
foreach ($resolved in $ResolvedDefectId) {
    Assert-KitsuSafeIdentifier -Value $resolved -Label 'resolved defect ID'
}
if ($SanitizedAttachmentPath.Count -gt 0 -and -not $ConfirmAttachmentsSanitized) {
    throw 'Attachments require -ConfirmAttachmentsSanitized after operator review.'
}

$reliabilityBinding = $null
if ($CaseId -eq 'reliability-review') {
    $reliabilityRoot = Assert-KitsuPathWithin -BaseDirectory $evidenceRoot `
        -CandidatePath (Join-Path $evidenceRoot 'reliability')
    $runDirectory = Assert-KitsuPathWithin -BaseDirectory $evidenceRoot `
        -CandidatePath (Join-Path $reliabilityRoot $ReliabilityRunId)
    if (-not (Test-Path -LiteralPath $runDirectory -PathType Container)) {
        throw 'The linked reliability run directory does not exist.'
    }
    $samplesPath = Join-Path $runDirectory 'samples.jsonl'
    $completionPath = Join-Path $runDirectory 'completion.json'
    if (-not (Test-Path -LiteralPath $samplesPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $completionPath -PathType Leaf)) {
        throw 'The linked reliability run has no complete sample and completion evidence.'
    }
    $completionRecord = Read-KitsuJson -LiteralPath $completionPath
    $samplesHash = Get-KitsuSha256 -LiteralPath $samplesPath
    if ($completionRecord.schema -ne $script:KitsuReliabilityCompletionSchema -or
        [string]$completionRecord.run_id -cne $ReliabilityRunId -or
        [string]$completionRecord.acceptance_record_sha256 -cne $acceptanceHash -or
        [string]$completionRecord.samples_sha256 -cne $samplesHash -or
        $completionRecord.status -ne 'COMPLETED') {
        throw 'The linked reliability run completion identity or sample digest is invalid.'
    }
    $reliabilityCompleted = ConvertTo-KitsuUtcDateTimeOffset `
        -Value $completionRecord.completed_at_utc
    if ($completed.ToUniversalTime() -le $reliabilityCompleted) {
        throw 'The reliability review must complete after the linked reliability run.'
    }
    $reliabilityBinding = [ordered]@{
        run_id = $ReliabilityRunId
        samples_sha256 = $samplesHash
        completion_sha256 = Get-KitsuSha256 -LiteralPath $completionPath
    }
}

$attachmentPlans = New-Object System.Collections.Generic.List[object]
$seenNames = @{}
for ($index = 0; $index -lt $SanitizedAttachmentPath.Count; $index += 1) {
    $source = (Resolve-Path -LiteralPath $SanitizedAttachmentPath[$index]).Path
    $item = Get-Item -LiteralPath $source
    $attachmentInfo = Get-KitsuSanitizedAttachmentInfo -LiteralPath $source
    $safeName = $item.Name -replace '[^A-Za-z0-9._-]', '_'
    if ([string]::IsNullOrWhiteSpace($safeName) -or $seenNames.ContainsKey($safeName)) {
        throw 'Attachment names must remain unique after safe-name normalization.'
    }
    $seenNames[$safeName] = $true
    $attachmentPlans.Add([pscustomobject]@{
        source = $source
        name = ('{0:D2}-{1}' -f ($index + 1), $safeName)
        bytes = $item.Length
        sha256 = Get-KitsuSha256 -LiteralPath $source
        extension = [string]$attachmentInfo.extension
        media_type = [string]$attachmentInfo.media_type
    })
}

$attemptId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' +
    [Guid]::NewGuid().ToString('N').Substring(0, 12)
$caseRoot = Assert-KitsuPathWithin -BaseDirectory $evidenceRoot -CandidatePath (Join-Path $evidenceRoot 'cases')
$caseDirectory = Assert-KitsuPathWithin -BaseDirectory $evidenceRoot -CandidatePath (Join-Path $caseRoot $CaseId)
$attemptDirectory = Assert-KitsuPathWithin -BaseDirectory $evidenceRoot `
    -CandidatePath (Join-Path $caseDirectory $attemptId)
$pendingName = '.pending-' + $attemptId + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 12)
$pendingDirectory = Assert-KitsuPathWithin -BaseDirectory $evidenceRoot `
    -CandidatePath (Join-Path $caseDirectory $pendingName)
New-Item -ItemType Directory -Path $caseDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $pendingDirectory | Out-Null

$attachmentRecords = New-Object System.Collections.Generic.List[object]
$committed = $false
try {
    if ($attachmentPlans.Count -gt 0) {
        $attachmentDirectory = Join-Path $pendingDirectory 'attachments'
        New-Item -ItemType Directory -Path $attachmentDirectory | Out-Null
        foreach ($plan in $attachmentPlans) {
            $destination = Assert-KitsuPathWithin -BaseDirectory $pendingDirectory `
                -CandidatePath (Join-Path $attachmentDirectory $plan.name)
            Copy-KitsuNewFile -SourcePath $plan.source -DestinationPath $destination
            $attachmentRecords.Add([ordered]@{
                file_name = $plan.name
                bytes = $plan.bytes
                sha256 = $plan.sha256
                media_type = $plan.media_type
                operator_confirmed_sanitized = $true
            })
        }
    }

    $record = [ordered]@{
        schema = $script:KitsuCaseSchema
        case_id = $CaseId
        attempt_id = $attemptId
        acceptance_record_sha256 = $acceptanceHash
        title = $Title
        status = $Status
        started_at_utc = $started.ToUniversalTime().ToString('o')
        completed_at_utc = $completed.ToUniversalTime().ToString('o')
        expected_result = $ExpectedResult
        observed_result = $ObservedResult
        reliability_run_id = if ($CaseId -eq 'reliability-review') { $ReliabilityRunId } else { $null }
        reliability_evidence = $reliabilityBinding
        defect = if ($Status -eq 'FAIL') {
            [ordered]@{ id = $DefectId; severity = [int]$DefectSeverity }
        } else { $null }
        resolved_defect_ids = @($ResolvedDefectId)
        attachments = $attachmentRecords.ToArray()
    }
    $pendingRecordPath = Join-Path $pendingDirectory 'case-result.json'
    Write-KitsuNewJson -LiteralPath $pendingRecordPath -InputObject $record
    [System.IO.Directory]::Move($pendingDirectory, $attemptDirectory)
    $committed = $true
} finally {
    if (-not $committed -and (Test-Path -LiteralPath $pendingDirectory -PathType Container)) {
        Remove-Item -LiteralPath $pendingDirectory -Recurse -Force
    }
}
$recordPath = Join-Path $attemptDirectory 'case-result.json'

Write-Host "Recorded $Status for $CaseId as write-once attempt $attemptId"
Write-Output $recordPath
