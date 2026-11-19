param(
    # Isolated builds never replace the installed executable or its dependencies.
    [string]$OutputDirectory = '',
    [ValidatePattern('^[A-Za-z0-9_.-]+\.exe$')]
    [string]$OutputName = 'TetrisVideoRecovery_single.exe',
    [switch]$Benchmark,
    [switch]$RangeBatch,
    [switch]$Aggressive,
    [switch]$FinalStateFast,
    [switch]$Ultra,
    [switch]$Maximum,
    [switch]$FrontGpu,
    [switch]$SafeSampling,
    [switch]$QueueOptimized,
    [switch]$NextGeneration,
    [switch]$PerfectClearGuard,
    [switch]$SplitReviewHost,
    [switch]$ReviewCompanion,
    [switch]$QueueRegression,
    [switch]$BrowserParityDiagnostics,
    [switch]$ConsumerOutput,
    # Keep the installed RTX 50 path as the default. Portable builds add
    # native kernels without changing the selected RTX 50 implementation.
    [ValidateSet('current', 'portable')]
    [string]$CudaProfile = 'current',
    [string]$QueueRegressionSource = ''
)

$ErrorActionPreference = 'Stop'
if ($ConsumerOutput -and -not $RangeBatch) { throw '-ConsumerOutput requires -RangeBatch.' }

$clang = 'C:\Program Files\LLVM\bin\clang++.exe'
if (-not (Test-Path -LiteralPath $clang)) { throw "LLVM/Clang is not installed at $clang" }

$root = $PSScriptRoot
$runtimeRoot = [IO.Path]::GetFullPath((Join-Path $root '..\video-recovery-runtime'))
if ($SplitReviewHost -and -not $NextGeneration) {
    throw '-SplitReviewHost requires -NextGeneration.'
}
if ($ReviewCompanion -and $NextGeneration) {
    throw '-ReviewCompanion must not be combined with -NextGeneration; the companion must not import NVDEC.'
}
if (($SplitReviewHost -or $ReviewCompanion) -and $Benchmark) {
    throw 'Split review roles are GUI builds and cannot be combined with -Benchmark.'
}
if (($SplitReviewHost -or $ReviewCompanion) -and $RangeBatch) {
    throw 'Split review roles cannot be combined with -RangeBatch.'
}
if ($Benchmark -and $RangeBatch) {
    throw '-Benchmark and -RangeBatch are separate executables.'
}
if ($QueueRegression -and ($Benchmark -or $RangeBatch -or $SplitReviewHost -or $ReviewCompanion)) {
    throw '-QueueRegression is a separate diagnostic executable.'
}
if ($SplitReviewHost -and $ReviewCompanion) {
    throw '-SplitReviewHost and -ReviewCompanion are separate executables and cannot be built together.'
}

if ($ReviewCompanion -and $OutputName -eq 'TetrisVideoRecovery_single.exe') {
    $OutputName = 'TetrisVideoRecovery_review.exe'
} elseif ($NextGeneration -and -not $SplitReviewHost -and
          $OutputName -eq 'TetrisVideoRecovery_single.exe') {
    $OutputName = 'TetrisVideoRecovery_gpu_next.exe'
} elseif ($QueueOptimized -and $OutputName -eq 'TetrisVideoRecovery_single.exe') {
    $OutputName = 'TetrisVideoRecovery_gpu_queue_opt.exe'
} elseif ($SafeSampling -and $OutputName -eq 'TetrisVideoRecovery_single.exe') {
    $OutputName = 'TetrisVideoRecovery_gpu_safe.exe'
} elseif ($FrontGpu -and $OutputName -eq 'TetrisVideoRecovery_single.exe') {
    $OutputName = 'TetrisVideoRecovery_gpu_front.exe'
} elseif ($Maximum -and $OutputName -eq 'TetrisVideoRecovery_single.exe') {
    $OutputName = 'TetrisVideoRecovery_gpu_max.exe'
} elseif ($Ultra -and $OutputName -eq 'TetrisVideoRecovery_single.exe') {
    $OutputName = 'TetrisVideoRecovery_gpu_ultra.exe'
} elseif ($FinalStateFast -and $OutputName -eq 'TetrisVideoRecovery_single.exe') {
    $OutputName = 'TetrisVideoRecovery_gpu_fast_final.exe'
}
$ortPackage = Join-Path $root 'third_party\onnxruntime_gpu_1.27.0\package'
$ortInclude = Join-Path $ortPackage 'buildTransitive\native\include'
$ortLib = Join-Path $ortPackage 'runtimes\win-x64\native'
$cudnnBin = Join-Path $runtimeRoot 'bin'
if (-not (Test-Path -LiteralPath (Join-Path $ortInclude 'onnxruntime_cxx_api.h'))) {
    throw "ONNX Runtime GPU headers are missing: $ortInclude"
}
if (-not (Test-Path -LiteralPath (Join-Path $ortLib 'onnxruntime.lib'))) {
    throw "ONNX Runtime GPU import library is missing: $ortLib"
}
if (-not (Test-Path -LiteralPath (Join-Path $cudnnBin 'cudnn64_9.dll'))) {
    throw "cuDNN 9 runtime is missing: $cudnnBin"
}

$bin = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $runtimeRoot 'bin' }
New-Item -ItemType Directory -Force -Path $bin | Out-Null
$exe = Join-Path $bin $OutputName
$flags = @(
    '-std=c++17', '-O3', '-flto', '-fuse-ld=lld',
    '-DTR_D3D11_VIDEO_DEFAULT=1',
    '-DTR_MF_BOARD_ADVANCED_PROCESSING_DEFAULT=1',
    '-DTR_ORT_CUDA_DEFAULT=1',
    '-DTR_ORT_CUDA_QUEUE_DEFAULT=1',
    '-DTR_QUEUE_SPARSE_ROI_DEFAULT=1',
    '-DNOMINMAX', '-DUNICODE', '-D_UNICODE', '-finput-charset=UTF-8', '-fexec-charset=UTF-8'
)
if ($Aggressive) { $flags += '-DTR_AGGRESSIVE_DEFAULTS=1' }
if ($BrowserParityDiagnostics) { $flags += '-DTR_BROWSER_PARITY_DIAGNOSTIC=1' }
if ($ConsumerOutput) { $flags += '-DTR_CONSUMER_OUTPUT=1' }
if ($SplitReviewHost) { $flags += '-DTR_SPLIT_REVIEW_HOST=1' }
if ($ReviewCompanion) { $flags += '-DTR_REVIEW_COMPANION=1' }
if ($FinalStateFast -or $Ultra -or $Maximum -or $FrontGpu -or $SafeSampling -or $QueueOptimized -or $NextGeneration) { $flags += '-DTR_GPU_FINAL_STATE_FAST_DEFAULT=1' }
if ($Ultra -or $Maximum -or $FrontGpu -or $SafeSampling -or $QueueOptimized -or $NextGeneration) { $flags += '-DTR_GPU_ULTRA_DEFAULT=1' }
if ($Maximum -or $FrontGpu -or $SafeSampling -or $QueueOptimized -or $NextGeneration) { $flags += '-DTR_GPU_MAX_DEFAULT=1' }
if ($FrontGpu -or $SafeSampling -or $QueueOptimized -or $NextGeneration) { $flags += '-DTR_GPU_FRONT_ANCHOR_DEFAULT=1' }
if ($SafeSampling -or $QueueOptimized -or $NextGeneration) { $flags += '-DTR_GPU_SAFE_SAMPLING_DEFAULT=1' }
if ($QueueOptimized -or $NextGeneration) { $flags += '-DTR_QUEUE_OPTIMIZED_DEFAULT=1' }
if ($NextGeneration) {
    $flags += '-DTR_FUSED_PREPROCESS_DEFAULT=1'
    $flags += '-DTR_NVDEC_DEFAULT=1'
}
if ($PerfectClearGuard) { $flags += '-DTR_PC_EFFECT_GUARD_DEFAULT=1' }

if ($QueueRegression) {
    $flags += '-municode'
    $sources = @(
        (Join-Path $root '..\queue-decoder-regression.cpp'),
        (Join-Path $root 'src\onnx_model.cpp'),
        (Join-Path $root 'src\queue_onnx_model.cpp'),
        (Join-Path $root 'src\d3d11_board_preprocessor.cpp'),
        (Join-Path $root 'src\d3d11_queue_preprocessor.cpp'),
        (Join-Path $root 'src\nvdec_cuda_preprocessor.cpp'),
        (Join-Path $root 'src\nvdec_video_backend.cpp'),
        (Join-Path $root 'src\recovery.cpp'),
        (Join-Path $root 'src\player_name_ocr.cpp'),
        (Join-Path $root 'src\neural_ranker.cpp'),
        (Join-Path $root 'src\training_import.cpp'),
        (Join-Path $root 'src\tetris_engine.cpp'),
        (Join-Path $root 'src\video_reader.cpp'),
        (Join-Path $root 'src\vision.cpp')
    )
    if ($QueueRegressionSource) { $sources[0] = $QueueRegressionSource }
} elseif ($Benchmark) {
    $sources = @(
        (Join-Path $root 'tests\analysis_benchmark.cpp'),
        (Join-Path $root 'src\onnx_model.cpp'),
        (Join-Path $root 'src\queue_onnx_model.cpp'),
        (Join-Path $root 'src\d3d11_board_preprocessor.cpp'),
        (Join-Path $root 'src\d3d11_queue_preprocessor.cpp'),
        (Join-Path $root 'src\nvdec_cuda_preprocessor.cpp'),
        (Join-Path $root 'src\nvdec_video_backend.cpp'),
        (Join-Path $root 'src\recovery.cpp'),
        (Join-Path $root 'src\player_name_ocr.cpp'),
        (Join-Path $root 'src\neural_ranker.cpp'),
        (Join-Path $root 'src\tetris_engine.cpp'),
        (Join-Path $root 'src\video_reader.cpp'),
        (Join-Path $root 'src\vision.cpp')
    )
} elseif ($RangeBatch) {
    $flags += '-municode'
    $sources = @(
        (Join-Path $root 'range_batch_main.cpp'),
        (Join-Path $root 'src\onnx_model.cpp'),
        (Join-Path $root 'src\scene_detector.cpp'),
        (Join-Path $root 'src\queue_onnx_model.cpp'),
        (Join-Path $root 'src\d3d11_board_preprocessor.cpp'),
        (Join-Path $root 'src\d3d11_queue_preprocessor.cpp'),
        (Join-Path $root 'src\nvdec_cuda_preprocessor.cpp'),
        (Join-Path $root 'src\nvdec_video_backend.cpp'),
        (Join-Path $root 'src\recovery.cpp'),
        (Join-Path $root 'src\player_name_ocr.cpp'),
        (Join-Path $root 'src\neural_ranker.cpp'),
        (Join-Path $root 'src\training_import.cpp'),
        (Join-Path $root 'src\tetris_engine.cpp'),
        (Join-Path $root 'src\video_reader.cpp'),
        (Join-Path $root 'src\vision.cpp')
    )
} else {
    $flags += '-municode'
    $sources = Get-ChildItem -LiteralPath (Join-Path $root 'src') -Filter '*.cpp' |
        Where-Object { $_.Name -notin @('video_reader_wasm.cpp', 'wasm_bridge.cpp', 'wasm_sha256.cpp', 'scene_detector.cpp') } |
        ForEach-Object FullName
    if ($NextGeneration) {
        $sources += (Join-Path $root 'src\scene_detector.cpp')
    }
}

$extraIncludes = @()
$extraLibraries = @()
$extraLinkFlags = @()
if ($NextGeneration) {
    $cudaRoot = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3'
    $nvcc = Join-Path $cudaRoot 'bin\nvcc.exe'
    $msvcCompiler = 'C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe'
    $nvSdkSamples = Join-Path $root 'third_party\video-sdk-samples\Samples'
    $ffmpeg = 'C:\tetris_nvdec_vcpkg\installed\x64-windows'
    foreach ($required in @(
        $nvcc,
        $msvcCompiler,
        (Join-Path $nvSdkSamples 'NvCodec\NvDecoder\NvDecoder.cpp'),
        (Join-Path $ffmpeg 'include\libavformat\avformat.h'),
        (Join-Path $ffmpeg 'lib\avformat.lib')
    )) {
        if (-not (Test-Path -LiteralPath $required)) {
            throw "Next-generation dependency is missing: $required"
        }
    }
    $cudaObjects = @()
    $cudaArchitectures = if ($CudaProfile -eq 'portable') { @('75', '80', '86', '89', '120') } else { @('120') }
    $cudaGenerationFlags = @()
    foreach ($architecture in $cudaArchitectures) {
        $cudaGenerationFlags += @('-gencode', "arch=compute_$architecture,code=sm_$architecture")
    }
    $cudaCache = Join-Path $bin ('cuda-cache\' + $CudaProfile)
    New-Item -ItemType Directory -Force -Path $cudaCache | Out-Null
    foreach ($cudaSourceName in @('nvdec_cuda_convert.cu', 'nvdec_cuda_preprocess.cu')) {
        $cudaSource = Join-Path $root ('src\' + $cudaSourceName)
        $cudaObject = Join-Path $cudaCache ([IO.Path]::GetFileNameWithoutExtension($cudaSourceName) + '.obj')
        # The installed CUDA toolkit can emit PTX newer than the display driver's
        # JIT understands. Ship native Blackwell SASS for the RTX 5060 instead.
        $kernelHeader = Join-Path $root 'include\nvdec_cuda_kernels.hpp'
        $fingerprintPath = $cudaObject + '.json'
        $fingerprint = [ordered]@{
            architectures = $cudaArchitectures
            flags = @('-std=c++17', '-O3', '-DTR_NVDEC_DEFAULT=1') + $cudaGenerationFlags
            source = (Get-FileHash -LiteralPath $cudaSource -Algorithm SHA256).Hash
            header = (Get-FileHash -LiteralPath $kernelHeader -Algorithm SHA256).Hash
            compiler = (Get-FileHash -LiteralPath $nvcc -Algorithm SHA256).Hash
            hostCompiler = (Get-FileHash -LiteralPath $msvcCompiler -Algorithm SHA256).Hash
        } | ConvertTo-Json -Compress
        $rebuildCuda = -not (Test-Path -LiteralPath $cudaObject) -or
            -not (Test-Path -LiteralPath $fingerprintPath) -or
            (Get-Content -LiteralPath $fingerprintPath -Raw).Trim() -ne $fingerprint
        if ($rebuildCuda) {
            & $nvcc -std=c++17 -O3 @cudaGenerationFlags `
                -DTR_NVDEC_DEFAULT=1 -I (Join-Path $root 'include') `
                -ccbin (Split-Path $msvcCompiler) `
                -c $cudaSource `
                -o $cudaObject
            if ($LASTEXITCODE -ne 0) {
                throw "NVDEC CUDA source build failed: $cudaSourceName"
            }
            [IO.File]::WriteAllText($fingerprintPath, $fingerprint)
        }
        $cudaObjects += $cudaObject
    }
    $sources += (Join-Path $nvSdkSamples 'NvCodec\NvDecoder\NvDecoder.cpp')
    $sources += $cudaObjects
    $extraIncludes += @(
        '-I', (Join-Path $cudaRoot 'include'),
        '-I', (Join-Path $nvSdkSamples 'NvCodec'),
        '-I', (Join-Path $nvSdkSamples 'Utils'),
        '-I', (Join-Path $ffmpeg 'include')
    )
    $extraLibraries += @(
        '-L', (Join-Path $cudaRoot 'lib\x64'),
        '-L', (Join-Path $nvSdkSamples 'NvCodec\Lib\x64'),
        '-L', (Join-Path $ffmpeg 'lib')
    )
    $extraLinkFlags += @(
        '-lcuda', '-lcudart', '-lnvcuvid',
        '-lavformat', '-lavcodec', '-lavutil', '-lws2_32', '-lsecur32'
    )
}

& $clang @flags -I (Join-Path $root 'include') -I $ortInclude @extraIncludes @sources -o $exe `
    -L $ortLib -lonnxruntime `
    @extraLibraries @extraLinkFlags `
    -luser32 -lgdi32 -lgdiplus -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lbcrypt `
    -lmfplat -lmfreadwrite -lmfuuid -lmfplay -lole32 -ld3d11 -ldxgi
if ($LASTEXITCODE -ne 0) { throw "CUDA build failed" }

# The split exists specifically to keep NVDEC/FFmpeg and MFPlay/EVR out of the
# same process. Treat that binary boundary as a build invariant so a future
# source or linker change cannot silently reintroduce the D3D9 crash.
if ($SplitReviewHost -or $ReviewCompanion) {
    $llvmReadObj = Join-Path (Split-Path -Parent $clang) 'llvm-readobj.exe'
    if (-not (Test-Path -LiteralPath $llvmReadObj -PathType Leaf)) {
        throw "Import-audit tool is missing: $llvmReadObj"
    }
    $importText = (& $llvmReadObj --coff-imports $exe 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to audit imports for split-review executable: $exe"
    }
    if ($SplitReviewHost) {
        if ($importText -match '(?im)^\s*Name:\s*MFPlay\.DLL\s*$') {
            throw 'Unsafe split-review host: MFPlay.DLL is still imported.'
        }
        if ($importText -notmatch '(?im)^\s*Name:\s*nvcuvid\.dll\s*$') {
            throw 'Invalid split-review host: nvcuvid.dll import is missing.'
        }
    }
    if ($ReviewCompanion) {
        if ($importText -notmatch '(?im)^\s*Name:\s*MFReadWrite\.DLL\s*$') {
            throw 'Invalid review companion: the SourceReader preview import is missing.'
        }
        if ($importText -match '(?im)^\s*Name:\s*MFPlay\.DLL\s*$') {
            throw 'Invalid review companion: obsolete MFPlay/EVR preview is still imported.'
        }
        if ($importText -match '(?im)^\s*Name:\s*(?:nvcuvid|nvcuda|avformat[^\s]*|avcodec[^\s]*|avutil[^\s]*)\.dll\s*$') {
            throw 'Unsafe review companion: an NVDEC/CUDA Video/FFmpeg DLL is imported.'
        }
    }
}

foreach ($dependencyName in @(
    'onnxruntime.dll',
    'onnxruntime_providers_shared.dll',
    'onnxruntime_providers_cuda.dll'
)) {
    $source = Join-Path $ortLib $dependencyName
    $destination = Join-Path $bin $dependencyName
    if (-not (Test-Path -LiteralPath $destination) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -ne
        (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash) {
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
}
foreach ($source in Get-ChildItem -LiteralPath $cudnnBin -Filter '*.dll' -File) {
    $destination = Join-Path $bin $source.Name
    if (-not (Test-Path -LiteralPath $destination) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $source.FullName).Hash -ne
        (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash) {
        Copy-Item -LiteralPath $source.FullName -Destination $destination -Force
    }
}
if ($NextGeneration) {
    $ffmpegBin = 'C:\tetris_nvdec_vcpkg\installed\x64-windows\bin'
    foreach ($source in Get-ChildItem -LiteralPath $ffmpegBin -Filter '*.dll' -File) {
        $destination = Join-Path $bin $source.Name
        if (-not (Test-Path -LiteralPath $destination) -or
            (Get-FileHash -Algorithm SHA256 -LiteralPath $source.FullName).Hash -ne
            (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash) {
            Copy-Item -LiteralPath $source.FullName -Destination $destination -Force
        }
    }
}

Write-Host "Built $exe"
Write-Host 'CUDA Toolkit 13.3 must remain installed; its bin\x64 directory supplies cuBLAS and cuFFT.'
