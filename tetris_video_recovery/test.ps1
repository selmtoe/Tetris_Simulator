$ErrorActionPreference = 'Stop'

$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
if (-not (Test-Path -LiteralPath $clang)) { throw "LLVM/Clang is not installed at $clang" }
$root = $PSScriptRoot
$ortRoot = Join-Path $root 'third_party\onnxruntime\package\onnxruntime-win-x64-1.23.0'
$ortInclude = Join-Path $ortRoot 'include'
$ortLib = Join-Path $ortRoot 'lib'
$testBinDir = Join-Path ([System.IO.Path]::GetTempPath()) ('tetris_recovery_tests_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $testBinDir | Out-Null
$testBin = Join-Path $testBinDir 'onnx_smoke.exe'

& $clang -std=c++17 -O2 -I (Join-Path $root 'include') -I $ortInclude `
  (Join-Path $root 'tests\onnx_smoke.cpp') `
  (Join-Path $root 'src\onnx_model.cpp') `
  (Join-Path $root 'src\vision.cpp') -o $testBin -L $ortLib -lonnxruntime
if ($LASTEXITCODE -ne 0) { throw 'ONNX smoke test build failed' }
$env:Path = "$(Join-Path $root 'bin');$env:Path"
Copy-Item -LiteralPath (Join-Path $ortLib 'onnxruntime.dll') -Destination $testBinDir -Force
Copy-Item -LiteralPath (Join-Path $ortLib 'onnxruntime_providers_shared.dll') -Destination $testBinDir -Force
& $testBin (Join-Path $root 'models\tetris.onnx')
if ($LASTEXITCODE -ne 0) { throw 'ONNX smoke test failed' }

$engineTest = Join-Path $testBinDir 'engine_test.exe'
& $clang -std=c++17 -O2 -I (Join-Path $root 'include') `
  (Join-Path $root 'tests\engine_test.cpp') `
  (Join-Path $root 'src\tetris_engine.cpp') -o $engineTest
if ($LASTEXITCODE -ne 0) { throw 'Engine test build failed' }
& $engineTest
if ($LASTEXITCODE -ne 0) { throw 'Engine test failed' }

$outputTest = Join-Path $testBinDir 'recovery_output_test.exe'
& $clang -std=c++17 -O2 -DNOMINMAX -DUNICODE -D_UNICODE `
  -I (Join-Path $root 'include') -I $ortInclude `
  (Join-Path $root 'tests\recovery_output_test.cpp') `
  (Join-Path $root 'src\recovery.cpp') `
  (Join-Path $root 'src\tetris_engine.cpp') `
  (Join-Path $root 'src\onnx_model.cpp') `
  (Join-Path $root 'src\vision.cpp') `
  (Join-Path $root 'src\video_reader.cpp') -o $outputTest `
  -L $ortLib -lonnxruntime `
  -luser32 -lgdi32 -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lbcrypt `
  -lmfplat -lmfreadwrite -lmfuuid -lole32
if ($LASTEXITCODE -ne 0) { throw 'Recovery output test build failed' }
$outputTestDir = Join-Path ([System.IO.Path]::GetTempPath()) ('tetris_recovery_output_' + [Guid]::NewGuid().ToString('N'))
& $outputTest (Join-Path $root 'tests\engine_test.cpp') $outputTestDir
if ($LASTEXITCODE -ne 0) { throw 'Recovery output test failed' }

Write-Host 'All recovery tests passed.'
