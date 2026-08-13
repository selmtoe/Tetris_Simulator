$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'bin\TetrisVideoRecovery.exe'
$buildScript = Join-Path $PSScriptRoot 'build.ps1'
$sources = @(
  Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'src') -Filter '*.cpp' -File
  Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'include') -Filter '*.hpp' -File
)
$needsBuild = -not (Test-Path -LiteralPath $exe)
if (-not $needsBuild) {
  $builtAt = (Get-Item -LiteralPath $exe).LastWriteTimeUtc
  $needsBuild = [bool]($sources | Where-Object { $_.LastWriteTimeUtc -gt $builtAt } | Select-Object -First 1)
}
if ($needsBuild) { & $buildScript }
Start-Process -FilePath $exe
