param(
    [ValidateSet('debug', 'release')]
    [string]$Profile = 'release'
)

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$manifest = Join-Path $repo 'simulator\cold-clear-wasm\Cargo.toml'
$targetDir = Join-Path $repo 'simulator\cold-clear-wasm\target\wasm32-unknown-unknown'
$profileDir = if ($Profile -eq 'release') { 'release' } else { 'debug' }
$source = Join-Path $targetDir (Join-Path $profileDir 'simulator_cold_clear_wasm.wasm')
$destination = Join-Path $repo 'simulator\workers\cold-clear.wasm'

$cargoArgs = @('build', '--manifest-path', $manifest, '--target', 'wasm32-unknown-unknown')
if ($Profile -eq 'release') { $cargoArgs += '--release' }
& cargo @cargoArgs
if ($LASTEXITCODE -ne 0) { throw "Cold Clear WASM build failed ($LASTEXITCODE)." }
if (-not (Test-Path -LiteralPath $source)) { throw "Cargo completed without producing $source." }
Copy-Item -LiteralPath $source -Destination $destination -Force
Write-Output ("Wrote {0} ({1} bytes)" -f $destination, (Get-Item -LiteralPath $destination).Length)
