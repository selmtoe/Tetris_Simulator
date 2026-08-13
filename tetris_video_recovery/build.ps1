$ErrorActionPreference = 'Stop'

$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
if (-not (Test-Path -LiteralPath $clang)) { throw "LLVM/Clang is not installed at $clang" }

$root = $PSScriptRoot
$ortRoot = Join-Path $root 'third_party\onnxruntime\package\onnxruntime-win-x64-1.23.0'
$ortInclude = Join-Path $ortRoot 'include'
$ortLib = Join-Path $ortRoot 'lib'
if (-not (Test-Path -LiteralPath (Join-Path $ortInclude 'onnxruntime_cxx_api.h'))) {
    throw "ONNX Runtime headers are missing: $ortInclude"
}
if (-not (Test-Path -LiteralPath (Join-Path $ortLib 'onnxruntime.lib'))) {
    throw "ONNX Runtime library is missing: $ortLib"
}

$bin = Join-Path $root 'bin'
New-Item -ItemType Directory -Force -Path $bin | Out-Null
$sources = Get-ChildItem -LiteralPath (Join-Path $root 'src') -Filter '*.cpp' | ForEach-Object FullName
$exe = Join-Path $bin 'TetrisVideoRecovery.exe'

& $clang -std=c++17 -O2 -DNOMINMAX -DUNICODE -D_UNICODE -municode `
  -finput-charset=UTF-8 -fexec-charset=UTF-8 `
  -I (Join-Path $root 'include') -I $ortInclude @sources -o $exe `
  -L $ortLib -lonnxruntime `
  -luser32 -lgdi32 -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lbcrypt `
  -lmfplat -lmfreadwrite -lmfuuid -lmfplay -lole32
if ($LASTEXITCODE -ne 0) { throw "TetrisVideoRecovery build failed" }

Copy-Item -LiteralPath (Join-Path $ortLib 'onnxruntime.dll') -Destination $bin -Force
Copy-Item -LiteralPath (Join-Path $ortLib 'onnxruntime_providers_shared.dll') -Destination $bin -Force
Write-Host "Built $exe"
