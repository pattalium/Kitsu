[CmdletBinding()]
param(
  [string]$OutputDirectory = (Join-Path $PSScriptRoot 'dist')
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$image = 'emscripten/emsdk@sha256:76a44fff907397784decc435115d07fcb9587a4f1504977f39f3745e538e3a1e'
$temporary = Join-Path ([System.IO.Path]::GetTempPath()) ("kitsu-full-wasm-{0}" -f $PID)
$unhashed = Join-Path $temporary 'kitsu-firmware-full.wasm'

if (Test-Path -LiteralPath $temporary) {
  Remove-Item -LiteralPath $temporary -Recurse -Force
}
New-Item -ItemType Directory -Path $temporary | Out-Null

try {
  & docker image inspect $image *> $null
  if ($LASTEXITCODE -ne 0) {
    throw "Pinned Emscripten image is not installed locally: $image"
  }

  & docker run --rm --network none `
    -v "${repository}:/repo:ro" `
    -v "${temporary}:/out" `
    -w /repo `
    $image `
    em++ '@tools/demo_wasm/full/build.rsp' `
    -o /out/kitsu-firmware-full.wasm
  if ($LASTEXITCODE -ne 0) {
    throw "Emscripten full-firmware build failed with exit code $LASTEXITCODE"
  }
  if (-not (Test-Path -LiteralPath $unhashed)) {
    throw 'Emscripten completed without producing the expected WebAssembly file.'
  }

  $sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $unhashed).Hash.ToLowerInvariant()
  $filename = "kitsu-firmware-full.$sha256.wasm"
  New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
  $destination = Join-Path $OutputDirectory $filename
  Copy-Item -LiteralPath $unhashed -Destination $destination -Force

  [pscustomobject]@{
    file = (Resolve-Path $destination).Path
    bytes = (Get-Item -LiteralPath $destination).Length
    sha256 = $sha256
    image = $image
  } | ConvertTo-Json -Compress
} finally {
  if (Test-Path -LiteralPath $temporary) {
    Remove-Item -LiteralPath $temporary -Recurse -Force
  }
}
