param(
    [string]$VideoPath = '',
    [switch]$DetailedLog,
    [switch]$MachineReadable,
    [switch]$ForceReanalysis,
    [switch]$ForceCpu,
    # Internal test seams. Normal users and TetrisStudy.exe do not pass these.
    [string]$BatchExecutable = '',
    [string]$OutputDirectory = '',
    [string]$SceneModelPath = '',
    [string]$BoardModelPath = '',
    [string]$SettingsPath = '',
    # Opt-in RAM disk staging. Large, disposable intermediates stay off the SSD.
    # Analysis results (batch output, lab import source) always stay on the system disk.
    [switch]$UseRamDisk,
    [string]$RamDiskRoot = '',
    [int]$RamDiskMinimumFreeGB = 0
)

$ErrorActionPreference = 'Stop'
$runtime = Join-Path $PSScriptRoot 'video-recovery-runtime'
$analysisCacheSchema = 'tetris-stream-analysis-cache/v1'
$analysisContractVersion = 'ppt-full-stream-analysis/2026-08-30-v3-hold-leading-next'

# RAM disk helper (detect / capacity / cleanup). Missing helper = legacy behaviour.
$ramDiskHelper = Join-Path $PSScriptRoot 'ppt-workdir.ps1'
if (Test-Path -LiteralPath $ramDiskHelper -PathType Leaf) { . $ramDiskHelper }
$script:pptRamWork = $null
$script:pptRamSavedTemp = $null

function Get-FileIdentity {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$IncludeHash,
        [switch]$IncludeSampleHash
    )
    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $item = Get-Item -LiteralPath $resolved -ErrorAction Stop
    $identity = [ordered]@{
        path = $resolved
        bytes = [long]$item.Length
        last_write_utc_ticks = [long]$item.LastWriteTimeUtc.Ticks
    }
    if ($IncludeHash) {
        $identity.sha256 = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash
    }
    if ($IncludeSampleHash) {
        $blockBytes = 1MB
        $offsets = @(0, [Math]::Max(0, [long](($item.Length - $blockBytes) / 2)),
            [Math]::Max(0, [long]($item.Length - $blockBytes))) | Select-Object -Unique
        $sample = New-Object IO.MemoryStream
        $writer = New-Object IO.BinaryWriter($sample)
        $stream = [IO.File]::Open($resolved, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::ReadWrite)
        try {
            $buffer = New-Object byte[] $blockBytes
            foreach ($offset in $offsets) {
                $writer.Write([long]$offset)
                $stream.Position = [long]$offset
                $read = $stream.Read($buffer, 0, $buffer.Length)
                $writer.Write($buffer, 0, $read)
            }
            $writer.Flush()
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                $identity.sample_sha256 = [BitConverter]::ToString(
                    $sha.ComputeHash($sample.ToArray())).Replace('-', '')
            } finally {
                $sha.Dispose()
            }
        } finally {
            $stream.Dispose()
            $writer.Dispose()
            $sample.Dispose()
        }
    }
    return $identity
}

function Test-FileIdentity {
    param($Saved, $Current, [switch]$RequireHash)
    if ($null -eq $Saved -or $null -eq $Current) { return $false }
    if (-not ([string]$Saved.path -ieq [string]$Current.path)) { return $false }
    if ([long]$Saved.bytes -ne [long]$Current.bytes) { return $false }
    if ([long]$Saved.last_write_utc_ticks -ne [long]$Current.last_write_utc_ticks) {
        return $false
    }
    if ($RequireHash -and -not ([string]$Saved.sha256 -eq [string]$Current.sha256)) {
        return $false
    }
    if ($null -ne $Current.sample_sha256 -and
        -not ([string]$Saved.sample_sha256 -eq [string]$Current.sample_sha256)) {
        return $false
    }
    return $true
}

function Resolve-ConfiguredDependency {
    param([string]$Settings, [string]$Key)
    foreach ($line in Get-Content -LiteralPath $Settings -Encoding UTF8) {
        if ($line -match ('^\s*' + [Regex]::Escape($Key) + '\s*=\s*(.+?)\s*$')) {
            $value = $Matches[1].Trim()
            if ([string]::IsNullOrWhiteSpace($value)) { return $null }
            if (-not [IO.Path]::IsPathRooted($value)) {
                $value = Join-Path (Split-Path -Parent $Settings) $value
            }
            return [IO.Path]::GetFullPath($value)
        }
    }
    return $null
}

function Get-AuxiliaryModelIdentities {
    param([string]$BoardModel, [string]$Settings)
    $paths = New-Object System.Collections.Generic.List[string]
    $paths.Add((Join-Path (Split-Path -Parent $BoardModel) 'next_hold.onnx'))
    foreach ($key in @('accurate_fallback_ranker', 'onnx_model', 'dictionary')) {
        $configured = Resolve-ConfiguredDependency -Settings $Settings -Key $key
        if (-not [string]::IsNullOrWhiteSpace($configured)) { $paths.Add($configured) }
    }
    $identities = @()
    foreach ($path in @($paths | Select-Object -Unique | Sort-Object)) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $identities += ,(Get-FileIdentity -Path $path -IncludeHash)
        } else {
            $identities += ,[ordered]@{ path = [IO.Path]::GetFullPath($path); missing = $true }
        }
    }
    return @($identities)
}

function Get-InferenceEnvironment {
    $values = [ordered]@{}
    foreach ($item in @(Get-ChildItem Env: | Where-Object { $_.Name -like 'TR_*' } |
            Sort-Object Name)) {
        $values[$item.Name] = [string]$item.Value
    }
    return $values
}

function Test-IdentityList {
    param($Saved, $Current)
    $savedJson = @($Saved) | ConvertTo-Json -Depth 6 -Compress
    $currentJson = @($Current) | ConvertTo-Json -Depth 6 -Compress
    return $savedJson -eq $currentJson
}

function Get-AnalysisInputs {
    param(
        [string]$Source,
        [string]$Batch,
        [string]$SceneModel,
        [string]$BoardModel,
        [string]$Settings
    )
    return [ordered]@{
        source = Get-FileIdentity -Path $Source -IncludeSampleHash
        batch_executable = Get-FileIdentity -Path $Batch -IncludeHash
        scene_model = Get-FileIdentity -Path $SceneModel -IncludeHash
        board_model = Get-FileIdentity -Path $BoardModel -IncludeHash
        settings = Get-FileIdentity -Path $Settings -IncludeHash
        auxiliary_models = @(Get-AuxiliaryModelIdentities `
            -BoardModel $BoardModel -Settings $Settings)
        environment = Get-InferenceEnvironment
    }
}

function Test-AnalysisCache {
    param(
        [string]$CachePath,
        [string]$ManifestPath,
        $Inputs
    )
    if ($ForceReanalysis) {
        return [pscustomobject]@{ Hit = $false; Reason = 'forced' }
    }
    if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) {
        return [pscustomobject]@{ Hit = $false; Reason = 'version-record-missing' }
    }
    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
        return [pscustomobject]@{ Hit = $false; Reason = 'manifest-missing' }
    }
    try {
        $saved = Get-Content -LiteralPath $CachePath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        return [pscustomobject]@{ Hit = $false; Reason = 'version-record-invalid' }
    }
    if ([string]$saved.schema -ne $analysisCacheSchema) {
        return [pscustomobject]@{ Hit = $false; Reason = 'cache-schema-changed' }
    }
    if ([string]$saved.analysis_version -ne $analysisContractVersion) {
        return [pscustomobject]@{ Hit = $false; Reason = 'analysis-version-changed' }
    }
    foreach ($name in @('source', 'batch_executable', 'scene_model', 'board_model', 'settings')) {
        $requireHash = $name -ne 'source'
        if (-not (Test-FileIdentity -Saved $saved.inputs.$name -Current $Inputs.$name `
                -RequireHash:$requireHash)) {
            return [pscustomobject]@{ Hit = $false; Reason = "$name-changed" }
        }
    }
    if (-not (Test-IdentityList -Saved $saved.inputs.auxiliary_models `
            -Current $Inputs.auxiliary_models)) {
        return [pscustomobject]@{ Hit = $false; Reason = 'auxiliary-model-changed' }
    }
    $savedEnvironment = $saved.inputs.environment | ConvertTo-Json -Depth 4 -Compress
    $currentEnvironment = $Inputs.environment | ConvertTo-Json -Depth 4 -Compress
    if ($savedEnvironment -ne $currentEnvironment) {
        return [pscustomobject]@{ Hit = $false; Reason = 'inference-environment-changed' }
    }
    if (-not ([string]$saved.manifest -ieq [IO.Path]::GetFullPath($ManifestPath))) {
        return [pscustomobject]@{ Hit = $false; Reason = 'manifest-path-changed' }
    }
    foreach ($requiredOutput in @($saved.required_outputs)) {
        if ([string]::IsNullOrWhiteSpace([string]$requiredOutput) -or
            -not (Test-Path -LiteralPath ([string]$requiredOutput) -PathType Leaf)) {
            return [pscustomobject]@{ Hit = $false; Reason = 'result-file-missing' }
        }
    }
    return [pscustomobject]@{ Hit = $true; Reason = 'exact-version-match' }
}

function Write-AnalysisCache {
    param(
        [string]$CachePath,
        [string]$ManifestPath,
        $Inputs
    )
    $requiredOutputs = New-Object System.Collections.Generic.List[string]
    $requiredOutputs.Add([IO.Path]::GetFullPath($ManifestPath))
    try {
        $manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($match in @($manifest.matches)) {
            if ([string]$match.status -eq 'failed') { continue }
            $autosave = [string]$match.autosave
            if ([string]::IsNullOrWhiteSpace($autosave)) { continue }
            if (-not [IO.Path]::IsPathRooted($autosave)) {
                $autosave = Join-Path (Split-Path -Parent $ManifestPath) $autosave
            }
            $requiredOutputs.Add([IO.Path]::GetFullPath($autosave))
        }
    } catch {
        throw "Completed analysis manifest could not be indexed for reuse: $ManifestPath"
    }
    $record = [ordered]@{
        schema = $analysisCacheSchema
        analysis_version = $analysisContractVersion
        created_utc = [DateTime]::UtcNow.ToString('o')
        manifest = [IO.Path]::GetFullPath($ManifestPath)
        inputs = $Inputs
        required_outputs = @($requiredOutputs)
    }
    $temporary = "$CachePath.writing"
    $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $CachePath -Force
}

function Open-StudyResult {
    param([string]$ManifestPath)
    if ($env:PPT_NO_OPEN -eq '1' -or
        -not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
        return
    }
    $study = Join-Path $runtime 'bin\TetrisStudy.exe'
    $pendingStudy = Join-Path $runtime 'bin\TetrisStudy.pending.exe'
    if (Test-Path -LiteralPath $pendingStudy -PathType Leaf) {
        $study = $pendingStudy
    }
    if (Test-Path -LiteralPath $study -PathType Leaf) {
        Start-Process -FilePath $study -ArgumentList ('"' + $ManifestPath + '"')
    } else {
        Write-Host "研究プロジェクト: $ManifestPath"
    }
}

function Invalidate-SceneCacheForVersionChange {
    param([string]$Output, [string]$Reason)
    if ($Reason -notin @(
            'forced', 'version-record-missing', 'version-record-invalid',
            'cache-schema-changed', 'analysis-version-changed',
            'source-changed', 'scene_model-changed')) {
        return
    }
    $sceneCache = Join-Path $Output 'segments.matches.json'
    if (-not (Test-Path -LiteralPath $sceneCache -PathType Leaf)) { return }
    $previous = Join-Path $Output 'segments.matches.previous.json'
    Move-Item -LiteralPath $sceneCache -Destination $previous -Force
    Write-Host "動画全体分類キャッシュも更新します（$Reason）"
}

function Write-OverallProgress {
    param(
        [double]$Percent,
        [string]$Status
    )
    $bounded = [Math]::Max(0, [Math]::Min(100, [int][Math]::Round($Percent)))
    Write-Progress -Id 1 -Activity 'ぷよテト配信・全自動解析' `
        -Status $Status -PercentComplete $bounded
    if ($MachineReadable) {
        $safeStatus = $Status -replace "[\r\n]+", ' '
        Write-Output "study_progress=$bounded status=$safeStatus"
    }
}

function Complete-ProgressDisplay {
    Write-Progress -Id 3 -ParentId 2 -Activity '現在の試合' -Completed
    Write-Progress -Id 2 -ParentId 1 -Activity '現在の工程' -Completed
    Write-Progress -Id 1 -Activity 'ぷよテト配信・全自動解析' -Completed
}

function Close-PptRamDiskWork {
    if ($null -eq $script:pptRamWork) { return }
    try {
        if ($null -ne $script:pptRamSavedTemp) {
            Restore-PptTempEnvironment -Saved $script:pptRamSavedTemp
        }
    } catch { }
    try {
        [void](Remove-PptWorkDir -WorkDir $script:pptRamWork)
    } catch { }
    $script:pptRamWork = $null
    $script:pptRamSavedTemp = $null
}

try {
    if ($MachineReadable) {
        [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
        $env:PPT_NO_OPEN = '1'
        $env:PPT_NO_PAUSE = '1'
    }
    if ([string]::IsNullOrWhiteSpace($VideoPath)) {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = 'Select a stream video for automatic analysis'
        $dialog.Filter = 'Video|*.mp4;*.mov;*.m4v;*.mkv;*.avi;*.webm|All files|*.*'
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
            exit 0
        }
        $VideoPath = $dialog.FileName
    }

    $source = (Resolve-Path -LiteralPath $VideoPath -ErrorAction Stop).Path
    $stem = [IO.Path]::GetFileNameWithoutExtension($source)
    $output = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        Join-Path $runtime (Join-Path 'data\batches' $stem)
    } else {
        [IO.Path]::GetFullPath($OutputDirectory)
    }
    $batch = if ([string]::IsNullOrWhiteSpace($BatchExecutable)) {
        Join-Path $runtime 'bin\TetrisVideoRecovery_stream.exe'
    } else {
        (Resolve-Path -LiteralPath $BatchExecutable -ErrorAction Stop).Path
    }
    $sceneModel = if ([string]::IsNullOrWhiteSpace($SceneModelPath)) {
        Join-Path $PSScriptRoot 'ppt-scene-classifier\artifacts\scene-classifier-fp32.onnx'
    } else {
        (Resolve-Path -LiteralPath $SceneModelPath -ErrorAction Stop).Path
    }
    $boardModel = if ([string]::IsNullOrWhiteSpace($BoardModelPath)) {
        Join-Path $runtime 'models\tetris.onnx'
    } else {
        (Resolve-Path -LiteralPath $BoardModelPath -ErrorAction Stop).Path
    }
    $settings = if ([string]::IsNullOrWhiteSpace($SettingsPath)) {
        Join-Path $runtime 'config\tetris_recover.ini'
    } else {
        (Resolve-Path -LiteralPath $SettingsPath -ErrorAction Stop).Path
    }
    $cuda = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin\x64'

    foreach ($required in @($batch, $sceneModel, $boardModel, $settings)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Required file is missing: $required"
        }
    }

    $env:PATH = "$($runtime)\bin;$cuda;$env:PATH"
    [void](New-Item -ItemType Directory -Path $output -Force)
    $manifestPath = Join-Path $output 'batch-results.json'
    $cachePath = Join-Path $output 'analysis-cache.v1.json'
    $analysisInputs = Get-AnalysisInputs -Source $source -Batch $batch `
        -SceneModel $sceneModel -BoardModel $boardModel -Settings $settings
    $cacheStatus = Test-AnalysisCache -CachePath $cachePath `
        -ManifestPath $manifestPath -Inputs $analysisInputs
    if ($cacheStatus.Hit) {
        Write-OverallProgress 100 '前回の完全解析を再利用 100%'
        Write-Host "前回の完全解析を再利用します（解析バージョン一致）: $manifestPath" `
            -ForegroundColor Green
        if ($MachineReadable) {
            Write-Output "study_cache=hit version=$analysisContractVersion"
            Write-Output "study_manifest=$manifestPath"
        }
        Start-Sleep -Milliseconds 150
        Complete-ProgressDisplay
        Open-StudyResult -ManifestPath $manifestPath
        if ($env:PPT_NO_PAUSE -ne '1') {
            [void](Read-Host 'Press Enter to close')
        }
        exit 0
    }
    Write-Host "解析キャッシュ: 再解析（$($cacheStatus.Reason)）"
    if ($MachineReadable) {
        Write-Output "study_cache=miss reason=$($cacheStatus.Reason) version=$analysisContractVersion"
    }
    Invalidate-SceneCacheForVersionChange -Output $output -Reason $cacheStatus.Reason

    if (($UseRamDisk -or -not [string]::IsNullOrWhiteSpace($RamDiskRoot)) -and
        (Get-Command New-PptWorkDir -ErrorAction SilentlyContinue)) {
        try {
            $sourceBytes = [long](Get-Item -LiteralPath $source -ErrorAction Stop).Length
            $requiredBytes = $sourceBytes + 1GB
            if ($RamDiskMinimumFreeGB -gt 0) {
                $requiredBytes = [Math]::Max($requiredBytes, [long]$RamDiskMinimumFreeGB * 1GB)
            }
            $script:pptRamWork = New-PptWorkDir -Purpose 'analysis' `
                -RequiredBytes $requiredBytes -PreferredRoot $RamDiskRoot
            if ($script:pptRamWork.Enabled) {
                $script:pptRamSavedTemp = Set-PptTempEnvironment -TempDir $script:pptRamWork.TempDir
                Write-Host "RAM disk work area: $($script:pptRamWork.Root) ($(Format-PptBytes $script:pptRamWork.FreeBytes) free)"
                if ($MachineReadable) {
                    Write-Output "study_ramdisk=on root=$($script:pptRamWork.Root) free_bytes=$($script:pptRamWork.FreeBytes)"
                }
            } else {
                Write-Host "RAM disk not used ($($script:pptRamWork.Reason)); falling back to the system disk"
                if ($MachineReadable) {
                    Write-Output "study_ramdisk=off reason=$($script:pptRamWork.Reason)"
                }
            }
        } catch {
            Write-Host "RAM disk setup skipped: $($_.Exception.Message)"
            if ($MachineReadable) { Write-Output 'study_ramdisk=off reason=setup-error' }
            $script:pptRamWork = $null
        }
    } elseif ($UseRamDisk -or -not [string]::IsNullOrWhiteSpace($RamDiskRoot)) {
        Write-Host 'RAM disk helper missing (ppt-workdir.ps1); continuing without it'
        if ($MachineReadable) { Write-Output 'study_ramdisk=off reason=helper-missing' }
    }

    $debugLog = Join-Path $output 'analysis-debug.log'
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $logWriter = New-Object System.IO.StreamWriter($debugLog, $false, $utf8)
    $logWriter.AutoFlush = $true

    $sceneFinished = $false
    $matchCount = 0
    $currentMatch = 0
    $currentMatchPercent = 0

    Write-Host "解析対象: $source"
    Write-Host "詳細ログ: $debugLog"
    if ($DetailedLog) {
        Write-Host 'デバッグ表示: ON（ネイティブ生ログも表示します）' -ForegroundColor Yellow
    }
    Write-OverallProgress 0 '準備中 0%'
    Write-Progress -Id 2 -ParentId 1 -Activity '[1/3] 動画全体をAI分類' `
        -Status '0%' -PercentComplete 0

    $previousErrorActionPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5 wraps native stderr lines as ErrorRecord objects.
        # Keep consuming them so the real native exit code and saved log remain available.
        $ErrorActionPreference = 'Continue'
        # 8GB GPU で 3 ワーカーを常時動かすための予算（実測の最適点）
        if ($ForceCpu) { $env:TR_BATCH_CPU_MODE = '1' }
        $env:TR_BATCH_WORKERS = '3'
        $env:TR_BOARD_WORKERS = '2'
        $env:TR_BATCH_WORKER_RAM_MB = '1536'
        $env:TR_BATCH_RAM_RESERVE_MB = '2048'
        $env:TR_BATCH_WORKER_VRAM_MB = '1024'
        $env:TR_BATCH_VRAM_RESERVE_MB = '512'
        $env:TR_BATCH_RELEASE_DELAY_SECONDS = '5'
        & $batch $source --detect $output $sceneModel $boardModel $settings 2>&1 |
            ForEach-Object {
                $line = [string]$_
                $logWriter.WriteLine($line)
                if ($DetailedLog) {
                    Write-Host $line -ForegroundColor DarkGray
                }

                if ($line -match '^scene_cache=miss\b') {
                    Write-Host '[1/3] 動画全体のAI分類を開始'
                    return
                }
                if ($line -match '^scene_cache=hit\b') {
                    $sceneFinished = $true
                    Write-Progress -Id 2 -ParentId 1 -Activity '[1/3] 動画全体をAI分類' `
                        -Status '100%（前回結果を再利用）' -PercentComplete 100
                    Write-OverallProgress 20 '動画分類完了 20%'
                    Write-Host '[1/3] 動画全体のAI分類 100%（前回結果を再利用）' -ForegroundColor Green
                    return
                }
                if ($line -match '^scene_progress=(\d+)\b') {
                    $scenePercent = [Math]::Max(0, [Math]::Min(100, [int]$Matches[1]))
                    Write-Progress -Id 2 -ParentId 1 -Activity '[1/3] 動画全体をAI分類' `
                        -Status "$scenePercent%" -PercentComplete $scenePercent
                    Write-OverallProgress ($scenePercent * 0.20) "動画分類中 $([int]($scenePercent * 0.20))%"
                    return
                }
                if ($line -match '^scene_done=1\s+matches=(\d+)') {
                    $sceneFinished = $true
                    $matchCount = [int]$Matches[1]
                    Write-Progress -Id 2 -ParentId 1 -Activity '[1/3] 動画全体をAI分類' `
                        -Status "100%（$matchCount 試合検出）" -PercentComplete 100
                    Write-OverallProgress 20 '動画分類完了 20%'
                    Write-Host "[1/3] 動画全体のAI分類 100%（$matchCount 試合検出）" -ForegroundColor Green
                    return
                }
                if ($line -match '^match_start=(\d+)/(\d+)\b') {
                    if (-not $sceneFinished) {
                        $sceneFinished = $true
                        Write-Progress -Id 2 -ParentId 1 -Activity '[1/3] 動画全体をAI分類' `
                            -Status '100%' -PercentComplete 100
                        Write-OverallProgress 20 '動画分類完了 20%'
                    }
                    $currentMatch = [int]$Matches[1]
                    $matchCount = [int]$Matches[2]
                    $currentMatchPercent = 0
                    $analysisPercent = (($currentMatch - 1) / [Math]::Max(1, $matchCount)) * 100
                    Write-Progress -Id 2 -ParentId 1 -Activity '[2/3] 全試合の盤面解析' `
                        -Status "$currentMatch / $matchCount 試合目・全体 $([int]$analysisPercent)%" `
                        -PercentComplete ([int]$analysisPercent)
                    Write-Progress -Id 3 -ParentId 2 -Activity "現在の試合 $currentMatch / $matchCount" `
                        -Status '0%' -PercentComplete 0
                    Write-OverallProgress (20 + $analysisPercent * 0.75) `
                        "盤面解析中 $([int](20 + $analysisPercent * 0.75))%"
                    return
                }
                if ($line -match '^match_progress=(\d+)\s+percent=(\d+)\b') {
                    $currentMatch = [int]$Matches[1]
                    $currentMatchPercent = [Math]::Max(0, [Math]::Min(100, [int]$Matches[2]))
                    $analysisPercent = ((($currentMatch - 1) + $currentMatchPercent / 100.0) / `
                        [Math]::Max(1, $matchCount)) * 100
                    Write-Progress -Id 2 -ParentId 1 -Activity '[2/3] 全試合の盤面解析' `
                        -Status "$currentMatch / $matchCount 試合目・全体 $([int]$analysisPercent)%" `
                        -PercentComplete ([int]$analysisPercent)
                    Write-Progress -Id 3 -ParentId 2 -Activity "現在の試合 $currentMatch / $matchCount" `
                        -Status "$currentMatchPercent%" -PercentComplete $currentMatchPercent
                    Write-OverallProgress (20 + $analysisPercent * 0.75) `
                        "盤面解析中 $([int](20 + $analysisPercent * 0.75))%"
                    return
                }
                if ($line -match '^match_done=(\d+)\s+success=(\d+)\s+repairable=(\d+)\b') {
                    $finishedMatch = [int]$Matches[1]
                    $success = [int]$Matches[2]
                    $repairable = [int]$Matches[3]
                    $analysisPercent = ($finishedMatch / [Math]::Max(1, $matchCount)) * 100
                    Write-Progress -Id 3 -ParentId 2 -Activity "現在の試合 $finishedMatch / $matchCount" `
                        -Status '100%' -PercentComplete 100
                    Write-Progress -Id 2 -ParentId 1 -Activity '[2/3] 全試合の盤面解析' `
                        -Status "$finishedMatch / $matchCount 試合完了・全体 $([int]$analysisPercent)%" `
                        -PercentComplete ([int]$analysisPercent)
                    Write-OverallProgress (20 + $analysisPercent * 0.75) `
                        "盤面解析中 $([int](20 + $analysisPercent * 0.75))%"
                    if ($repairable -eq 1) {
                        Write-Host "要修正: $finishedMatch 試合目は合法手が途切れました。結果を保存し、完了後に修正画面を開きます。" `
                            -ForegroundColor Yellow
                    } elseif ($success -ne 1) {
                        Write-Host "警告: $finishedMatch 試合目の解析に失敗しました。詳細はログを確認してください。" `
                            -ForegroundColor Yellow
                    }
                    if ($finishedMatch -ge $matchCount) {
                        Write-Progress -Id 3 -ParentId 2 -Activity '現在の試合' -Completed
                        Write-Progress -Id 2 -ParentId 1 -Activity '[3/3] 結果を保存' `
                            -Status '0%' -PercentComplete 0
                        Write-OverallProgress 95 '結果保存中 95%'
                        Write-Host '[2/3] 全試合の盤面解析 100%' -ForegroundColor Green
                    }
                    return
                }
                if ($line -match '^batch_done=1\s+matches=(\d+)\s+failures=(\d+)') {
                    $completedMatches = [int]$Matches[1]
                    $failures = [int]$Matches[2]
                    Write-Progress -Id 2 -ParentId 1 -Activity '[3/3] 結果を保存' `
                        -Status '100%' -PercentComplete 100
                    Write-OverallProgress 100 '完了 100%'
                    Write-Host "[3/3] 結果保存 100%（$completedMatches 試合、失敗 $failures）" `
                        -ForegroundColor Green
                    return
                }
                if (-not $DetailedLog -and $line -match '(^|\s)([a-z_]+_error=|error=")') {
                    Write-Host "エラー詳細: $line" -ForegroundColor Red
                }
            }
        $nativeExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
        $logWriter.Dispose()
    }
    if ($nativeExitCode -ne 0) {
        Complete-ProgressDisplay
        throw "Automatic analysis failed with exit code $nativeExitCode. Details: $debugLog"
    }

    Write-OverallProgress 100 '完了 100%'
    Start-Sleep -Milliseconds 350
    Complete-ProgressDisplay

    Write-Host ''
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Automatic analysis finished without a result manifest: $manifestPath"
    }
    Write-AnalysisCache -CachePath $cachePath -ManifestPath $manifestPath `
        -Inputs $analysisInputs
    Write-Host "Complete: $manifestPath"
    Write-Host "解析バージョン記録: $cachePath"

    # Winner detection (decide the match winner from the result screen).
    # A failure here must not fail the analysis itself, so it is swallowed.
    if ($env:PPT_NO_WINNER -ne '1') {
        $winnerScript = 'C:\Users\hirom\Documents\AI-Agent-Workspace\detect-winners.py'
        $pythonExe = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe'
        if ((Test-Path -LiteralPath $winnerScript -PathType Leaf) -and
            (Test-Path -LiteralPath $pythonExe -PathType Leaf)) {
            Write-Host ''
            Write-Host 'Detecting match winners (result screen)...'
            $winnerBatch = Split-Path -Leaf $output
            try {
                & $pythonExe -X utf8 $winnerScript --batch $winnerBatch
                if ($LASTEXITCODE -ne 0) {
                    Write-Host "Winner detection failed with exit code $LASTEXITCODE" -ForegroundColor Yellow
                }
            } catch {
                Write-Host "Winner detection skipped: $($_.Exception.Message)" -ForegroundColor Yellow
            }
        }
    }
    if ($MachineReadable) {
        Write-Output "study_cache=stored version=$analysisContractVersion"
        Write-Output "study_manifest=$manifestPath"
    }
    Close-PptRamDiskWork
    Open-StudyResult -ManifestPath $manifestPath
    if ($env:PPT_NO_PAUSE -ne '1') {
        [void](Read-Host 'Press Enter to close')
    }
    exit 0
} catch {
    Write-Host ''
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Close-PptRamDiskWork
    exit 1
}
