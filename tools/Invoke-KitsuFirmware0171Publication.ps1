[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Sign', 'Deploy')]
    [string] $Action,

    [Parameter(Mandatory = $true)]
    [string] $StageDir,

    [Parameter(Mandatory = $true)]
    [string] $SshPrivateKey,

    [Parameter(Mandatory = $true)]
    [string] $KnownHosts,

    [Parameter(Mandatory = $true)]
    [string] $HostAddress,

    [Parameter(Mandatory = $true)]
    [string] $UserName,
    [string] $Python = 'python',
    [string] $OpenSsl = 'C:\Program Files\Git\usr\bin\openssl.exe',
    [string] $Ssh = 'C:\Windows\System32\OpenSSH\ssh.exe',
    [string] $Scp = 'C:\Windows\System32\OpenSSH\scp.exe',
    [string] $Tar = 'tar.exe',
    [switch] $InteractiveSudo
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ExpectedPlanSchema = 'kitsu.firmware-publication-plan.v1'
$ExpectedVersion = '0.17.1'
$ExpectedReleaseId = 'kitsu-0.17.1-reflashable-1'
$ExpectedPublicKeySha256 = '711ad6b564e129cbd31b8edca52f4977c03daf0410490f62c6fba4484f65366c'
$ExpectedValidatorSha256 = 'b580726556b989a03251acd63aa800193aa2091592ad097c0ba6fbcd380d306a'

function Resolve-Program([string] $Value, [string] $Label) {
    if (Test-Path -LiteralPath $Value -PathType Leaf) {
        return (Resolve-Path -LiteralPath $Value).ProviderPath
    }
    $command = Get-Command -Name $Value -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $command) { throw "Missing $Label executable: $Value" }
    return $command.Source
}

function Get-Sha256([string] $Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Invoke-External(
    [string] $Program,
    [object[]] $ArgumentList,
    [string] $FailureMessage
) {
    & $Program @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage (exit $LASTEXITCODE)."
    }
}

foreach ($pair in @(
    @($StageDir, 'publication stage directory', $true),
    @($SshPrivateKey, 'SSH private key', $false),
    @($KnownHosts, 'pinned SSH known-hosts file', $false)
)) {
    $kind = if ($pair[2]) { 'Container' } else { 'Leaf' }
    if (-not (Test-Path -LiteralPath $pair[0] -PathType $kind)) {
        throw "Missing $($pair[1]): $($pair[0])"
    }
}
if ($HostAddress -cnotmatch '^[0-9A-Za-z][0-9A-Za-z.-]{0,252}$') {
    throw 'HostAddress contains unsafe characters.'
}
if ($UserName -cnotmatch '^[a-z_][a-z0-9_-]{0,31}$') {
    throw 'UserName contains unsafe characters.'
}
$StageDir = (Resolve-Path -LiteralPath $StageDir).ProviderPath
$SshPrivateKey = (Resolve-Path -LiteralPath $SshPrivateKey).ProviderPath
$KnownHosts = (Resolve-Path -LiteralPath $KnownHosts).ProviderPath
$Python = Resolve-Program $Python 'Python'
$OpenSsl = Resolve-Program $OpenSsl 'OpenSSL'
$Ssh = Resolve-Program $Ssh 'SSH'
$Scp = Resolve-Program $Scp 'SCP'

$planPath = Join-Path $StageDir 'publication-plan.json'
$validator = Join-Path $StageDir 'tools\validate-stage.py'
$manifest = Join-Path $StageDir 'update\latest.json'
$signature = Join-Path $StageDir 'update\latest.json.sig'
$signatureCandidate = "$signature.next"
$publicKey = Join-Path $StageDir 'update\update-ed25519-public.pem'
foreach ($path in @($planPath, $validator, $manifest, $publicKey)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Publication stage is incomplete: $path"
    }
}
if ((Get-Sha256 $validator) -cne $ExpectedValidatorSha256) {
    throw 'Publication-stage validator identity changed.'
}

$plan = Get-Content -LiteralPath $planPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($plan.schema -cne $ExpectedPlanSchema -or
    $plan.firmware_version -cne $ExpectedVersion -or
    $plan.release_id -cne $ExpectedReleaseId) {
    throw 'Publication plan is not the expected Kitsu 0.17.1 release.'
}
$manifestBytes = [long] $plan.manifest.bytes
$manifestSha256 = [string] $plan.manifest.sha256
$flashReleaseId = [string] $plan.flash_release_id
if ($manifestBytes -lt 2 -or $manifestBytes -gt 65536 -or
    $manifestSha256 -cnotmatch '^[0-9a-f]{64}$' -or
    $flashReleaseId -cnotmatch '^[0-9A-Za-z][0-9A-Za-z._-]{0,127}$') {
    throw 'Publication plan contains an unsafe manifest or release identity.'
}
if ((Get-Item -LiteralPath $manifest).Length -ne $manifestBytes -or
    (Get-Sha256 $manifest) -cne $manifestSha256) {
    throw 'Canonical manifest identity differs from publication-plan.json.'
}
if ((Get-Sha256 $publicKey) -cne $ExpectedPublicKeySha256) {
    throw 'Update authority public-key file changed.'
}

$transport = @(
    '-F', 'NUL', '-i', $SshPrivateKey,
    '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes', '-o', 'IdentityAgent=none',
    '-o', 'CertificateFile=none', '-o', 'PasswordAuthentication=no',
    '-o', 'KbdInteractiveAuthentication=no', '-o', 'PreferredAuthentications=publickey',
    '-o', 'StrictHostKeyChecking=yes', '-o', "UserKnownHostsFile=$KnownHosts",
    '-o', 'GlobalKnownHostsFile=NUL', '-o', 'HostKeyAlgorithms=ssh-ed25519',
    '-o', 'UpdateHostKeys=no', '-o', 'GSSAPIAuthentication=no',
    '-o', 'HostbasedAuthentication=no', '-o', 'PermitLocalCommand=no'
)
$sshBase = @('-p', '22') + $transport + @('-l', $UserName, $HostAddress)
$scpBase = @('-S', $Ssh, '-P', '22') + $transport
$remoteTarget = "${UserName}@${HostAddress}"

function Invoke-StageValidation([bool] $RequireSignature) {
    $arguments = @($validator, 'validate-stage', '--stage', $StageDir, '--openssl', $OpenSsl)
    if ($RequireSignature) { $arguments += '--require-signature' }
    Invoke-External $Python $arguments 'Publication-stage validation failed'
}

if ($Action -eq 'Sign') {
    if (Test-Path -LiteralPath $signature) {
        throw "Final signature already exists: $signature"
    }
    if (Test-Path -LiteralPath $signatureCandidate) {
        throw "Candidate signature already exists: $signatureCandidate"
    }
    Invoke-StageValidation $false

    $remoteManifest = "/home/$UserName/kitsu-firmware-0171-$manifestSha256.json"
    $remoteSignature = "$remoteManifest.sig"
    $signatureCreated = $false
    try {
        $uniqueCheck = "test ! -e '$remoteManifest' && test ! -e '$remoteSignature'"
        Invoke-External $Ssh ($sshBase + @($uniqueCheck)) 'Remote signing-path check failed'
        Invoke-External $Scp ($scpBase + @($manifest, "${remoteTarget}:$remoteManifest")) 'Manifest upload failed'

        $remoteCommand = 'test "$(stat -c ''%s'' -- ''{0}'')" -eq {1} && printf ''%s  %s\n'' ''{2}'' ''{0}'' | sha256sum -c - && /home/{3}/.local/libexec/kitsu/kitsu-sign-update-manifest ''{0}'' ''{4}'' && test "$(stat -c ''%s'' -- ''{4}'')" -eq 64 && openssl pkeyutl -verify -pubin -inkey /home/{3}/.config/kitsu/update-ed25519-public.pem -rawin -in ''{0}'' -sigfile ''{4}'' >/dev/null' -f $remoteManifest, $manifestBytes, $manifestSha256, $UserName, $remoteSignature
        Invoke-External $Ssh ($sshBase + @($remoteCommand)) 'Protected manifest signing failed'
        Invoke-External $Scp ($scpBase + @("${remoteTarget}:$remoteSignature", $signatureCandidate)) 'Signature download failed'

        if ((Get-Item -LiteralPath $signatureCandidate).Length -ne 64) {
            throw 'Downloaded signature is not exactly 64 bytes.'
        }
        Invoke-External $OpenSsl @(
            'pkeyutl', '-verify', '-pubin', '-inkey', $publicKey,
            '-rawin', '-in', $manifest, '-sigfile', $signatureCandidate
        ) 'Downloaded Ed25519 signature did not verify'
        Move-Item -LiteralPath $signatureCandidate -Destination $signature
        $signatureCreated = $true
        Invoke-StageValidation $true

        [pscustomobject]@{
            status = 'SIGNED'
            firmware_version = $ExpectedVersion
            release_id = $ExpectedReleaseId
            manifest_bytes = $manifestBytes
            manifest_sha256 = $manifestSha256
            signature_bytes = (Get-Item -LiteralPath $signature).Length
            signature_sha256 = Get-Sha256 $signature
            password_used = $false
        } | Format-List
    }
    catch {
        if ($signatureCreated -and (Test-Path -LiteralPath $signature -PathType Leaf)) {
            Remove-Item -LiteralPath $signature -Force
        }
        throw
    }
    finally {
        $cleanup = "rm -f -- '$remoteManifest' '$remoteSignature'"
        & $Ssh @sshBase $cleanup 2>$null | Out-Null
        if (Test-Path -LiteralPath $signatureCandidate) {
            Remove-Item -LiteralPath $signatureCandidate -Force
        }
    }
    exit 0
}

if (-not (Test-Path -LiteralPath $signature -PathType Leaf)) {
    throw 'Sign the publication stage before deployment.'
}
Invoke-StageValidation $true
$Tar = Resolve-Program $Tar 'tar'
$deployScript = Join-Path $PSScriptRoot 'deploy_kitsu_firmware_atomic.sh'
if (-not (Test-Path -LiteralPath $deployScript -PathType Leaf)) {
    throw "Missing atomic deployment script: $deployScript"
}
$deployScript = (Resolve-Path -LiteralPath $deployScript).ProviderPath
$deploySha256 = Get-Sha256 $deployScript
$deployBytes = (Get-Item -LiteralPath $deployScript).Length
$archive = Join-Path ([IO.Path]::GetTempPath()) "kitsu-firmware-0171-$manifestSha256.tar.gz"
if (Test-Path -LiteralPath $archive) {
    throw "Temporary publication archive already exists: $archive"
}
$remoteArchive = "/home/$UserName/kitsu-firmware-0171-$manifestSha256.tar.gz"
$remoteDeploy = "/home/$UserName/kitsu-firmware-0171-$manifestSha256-deploy.sh"
try {
    Invoke-External $Tar @('-czf', $archive, '-C', $StageDir, '.') 'Publication archive creation failed'
    $archiveSha256 = Get-Sha256 $archive
    $archiveBytes = (Get-Item -LiteralPath $archive).Length

    $uniqueCheck = "test ! -e '$remoteArchive' && test ! -e '$remoteDeploy'"
    Invoke-External $Ssh ($sshBase + @($uniqueCheck)) 'Remote deployment-path check failed'
    Invoke-External $Scp ($scpBase + @($deployScript, "${remoteTarget}:$remoteDeploy")) 'Atomic deployer upload failed'
    Invoke-External $Scp ($scpBase + @($archive, "${remoteTarget}:$remoteArchive")) 'Publication archive upload failed'

    $sudoCommand = if ($InteractiveSudo) { 'sudo /bin/bash' } else { 'sudo -n /bin/bash' }
    $deploySshBase = if ($InteractiveSudo) { @('-tt') + $sshBase } else { $sshBase }
    $remoteCommand = 'test "$(stat -c ''%s'' -- ''{0}'')" -eq {1} && printf ''%s  %s\n'' ''{2}'' ''{0}'' | sha256sum -c - && test "$(stat -c ''%s'' -- ''{3}'')" -eq {4} && printf ''%s  %s\n'' ''{5}'' ''{3}'' | sha256sum -c - && {6} ''{3}'' --archive ''{0}'' --archive-sha256 ''{2}''' -f $remoteArchive, $archiveBytes, $archiveSha256, $remoteDeploy, $deployBytes, $deploySha256, $sudoCommand
    Invoke-External $Ssh ($deploySshBase + @($remoteCommand)) 'Atomic firmware deployment failed'

    [pscustomobject]@{
        status = 'DEPLOYED'
        firmware_version = $ExpectedVersion
        release_id = $ExpectedReleaseId
        flash_release_id = $flashReleaseId
        manifest_sha256 = $manifestSha256
        signature_sha256 = Get-Sha256 $signature
        archive_sha256 = $archiveSha256
        interactive_sudo = $InteractiveSudo.IsPresent
    } | Format-List
}
finally {
    $cleanup = "rm -f -- '$remoteArchive' '$remoteDeploy'"
    & $Ssh @sshBase $cleanup 2>$null | Out-Null
    if (Test-Path -LiteralPath $archive) {
        Remove-Item -LiteralPath $archive -Force
    }
}
