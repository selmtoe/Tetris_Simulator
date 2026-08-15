param(
    [string]$Empp = $env:EMPP
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$web = Join-Path $root 'web'

if ([string]::IsNullOrWhiteSpace($Empp)) {
    $candidate = Get-Command 'em++' -ErrorAction SilentlyContinue
    if ($candidate) {
        $Empp = $candidate.Source
    } elseif ($env:EMSDK) {
        foreach ($name in @('em++.exe', 'em++.bat')) {
            $path = Join-Path $env:EMSDK "upstream\emscripten\$name"
            if (Test-Path -LiteralPath $path) { $Empp = $path; break }
        }
    }
}
if ([string]::IsNullOrWhiteSpace($Empp) -or -not (Test-Path -LiteralPath $Empp)) {
    throw 'Emscripten em++ was not found. Activate emsdk first, or pass -Empp <path-to-em++>.'
}

$sources = @(
    (Join-Path $root 'src\recovery.cpp'),
    (Join-Path $root 'src\tetris_engine.cpp'),
    (Join-Path $root 'src\vision.cpp'),
    (Join-Path $root 'src\wasm_bridge.cpp'),
    (Join-Path $root 'src\wasm_sha256.cpp'),
    (Join-Path $root 'src\video_reader_wasm.cpp')
)
$output = Join-Path $web 'tetris_recovery.js'
$exports = "['_malloc','_free','_tr_runtime_init','_tr_runtime_reset','_tr_frame_upload','_tr_queue_observe_and_add','_tr_prepare_board_requests','_tr_board_features','_tr_board_finish','_tr_recover','_tr_sample_interval','_tr_onnx_samples','_tr_review_snapshot','_tr_review_candidates','_tr_review_queue_edit','_tr_review_queue_restore','_tr_review_reanalyze','_tr_review_apply_candidate','_tr_review_restore_automatic','_tr_write_output','_tr_output_path','_tr_output_url','_tr_last_error']"
$runtime = "['FS','HEAPU8','HEAPU16','HEAP32','HEAPF32','HEAPF64','UTF8ToString','lengthBytesUTF8','stringToUTF8']"
$arguments = @(
    '-std=c++17', '-O3', '-DNDEBUG', "-I$($root)\include",
    '-s', 'MODULARIZE=1', '-s', 'EXPORT_ES6=1', '-s', 'EXPORT_NAME=createTetrisRecoveryModule',
    '-s', 'ENVIRONMENT=web', '-s', 'FILESYSTEM=1', '-s', 'ALLOW_MEMORY_GROWTH=1',
    '-s', 'STACK_SIZE=2097152', '-s', 'NO_EXIT_RUNTIME=1', '-s', "EXPORTED_FUNCTIONS=$exports",
    '-s', "EXPORTED_RUNTIME_METHODS=$runtime", '-o', $output
) + $sources

& $Empp @arguments
if ($LASTEXITCODE -ne 0) { throw "Emscripten build failed with exit code $LASTEXITCODE." }
if (-not (Test-Path -LiteralPath $output) -or -not (Test-Path -LiteralPath ([System.IO.Path]::ChangeExtension($output, '.wasm')))) {
    throw 'Emscripten completed without producing the JS/WASM pair.'
}
Write-Host "Built $output"
