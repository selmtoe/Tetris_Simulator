param(
    [string]$Empp = $env:EMPP
)

$ErrorActionPreference = 'Stop'

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourceRoot = Join-Path $root 'third_party\sfinder-cpp-master\src'
$apiSource = Join-Path $root 'simulator\pc-solver\pc_solver_api.cpp'
$outputJs = Join-Path $root 'simulator\pc-solver\sfinder-pc.js'

if ([string]::IsNullOrWhiteSpace($Empp)) {
    $candidate = Get-Command 'em++' -ErrorAction SilentlyContinue
    if ($candidate) {
        $Empp = $candidate.Source
    } elseif ($env:EMSDK) {
        foreach ($fileName in @('em++.exe', 'em++.bat')) {
            $sdkCandidate = Join-Path $env:EMSDK "upstream\emscripten\$fileName"
            if (Test-Path -LiteralPath $sdkCandidate) {
                $Empp = $sdkCandidate
                break
            }
        }
    }
}

if ([string]::IsNullOrWhiteSpace($Empp) -or -not (Test-Path -LiteralPath $Empp)) {
    throw 'Emscripten em++ was not found. Activate emsdk first, or pass -Empp <path-to-em++>.'
}

$sources = @(
    (Join-Path $sourceRoot 'core\bits.cpp'),
    (Join-Path $sourceRoot 'core\field.cpp'),
    (Join-Path $sourceRoot 'core\moves.cpp'),
    (Join-Path $sourceRoot 'core\piece.cpp'),
    (Join-Path $sourceRoot 'core\srs.cpp'),
    (Join-Path $sourceRoot 'finder\perfect.cpp'),
    $apiSource
)

$arguments = @(
    '-std=c++17', '-O3', '-DNDEBUG',
    "-I$sourceRoot",
    '-s', 'MODULARIZE=1',
    '-s', 'EXPORT_NAME=createSfinderPcModule',
    '-s', 'EXPORT_ES6=0',
    '-s', 'ENVIRONMENT=web,worker,node',
    '-s', 'FILESYSTEM=0',
    '-s', 'ALLOW_MEMORY_GROWTH=1',
    '-s', 'STACK_SIZE=1048576',
    '-s', "EXPORTED_FUNCTIONS=['_malloc','_free','_sfinder_find_pc']",
    '-s', "EXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU16','HEAP32']",
    '-o', $outputJs
) + $sources

& $Empp @arguments
if ($LASTEXITCODE -ne 0) {
    throw "WASM build failed with exit code $LASTEXITCODE."
}

$outputWasm = [System.IO.Path]::ChangeExtension($outputJs, '.wasm')
if (-not (Test-Path -LiteralPath $outputJs) -or -not (Test-Path -LiteralPath $outputWasm)) {
    throw 'Emscripten completed without producing both sfinder-pc.js and sfinder-pc.wasm.'
}

Write-Host "Built $outputJs"
Write-Host "Built $outputWasm"
