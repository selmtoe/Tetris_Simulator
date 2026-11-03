param(
    [string]$VideoPath = '',
    [switch]$DetailedLog,
    [switch]$MachineReadable,
    [switch]$ForceReanalysis,
    [switch]$ForceCpu,
    # 解析前の環境チェック（必要なファイル・DLL・GPU・空き容量）を省略する。上級者向け。
    # 通常は付けない。足りないものがあれば解析を始めずに終了コード 2 で止まる。
    [switch]$SkipEnvironmentCheck,
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
# 解析前の環境チェック（tools\ppt-preflight.ps1）。不足があれば解析を始めずに終了する。
$preflightHelper = Join-Path $PSScriptRoot 'ppt-preflight.ps1'
if (Test-Path -LiteralPath $preflightHelper -PathType Leaf) { . $preflightHelper }
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

function Get-WinnerDetectionState {
    # 勝敗判定をやり直す必要があるかを返す（空文字＝有効な勝敗が入っている）。
    # 再実行の条件は「未作成 / matches が空 / 全試合 winner が null」のいずれか。
    # 1試合でも決まっていれば有効とみなし、毎回の再計算はしない。
    param([string]$Output)
    $path = Join-Path $Output 'winner-detection.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return 'missing' }
    try {
        $payload = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        return 'unreadable'
    }
    $rows = @($payload.matches)
    if ($rows.Count -eq 0) { return 'no-matches' }
    $resolved = @($rows | Where-Object {
            $null -ne $_.winner -and -not [string]::IsNullOrWhiteSpace([string]$_.winner)
        })
    if ($resolved.Count -eq 0) { return 'all-undetermined' }
    return ''
}

function Get-BatchVideoPath {
    # バッチの元動画を返す（無ければ $null）。二重チェックは動画が無いと走らせない。
    # バッチ名に [ ] が入るので、パスの確認は必ず -LiteralPath を使う。
    param([string]$Output)
    $manifestPath = Join-Path $Output 'batch-results.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return $null }
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 |
            ConvertFrom-Json
    } catch {
        return $null
    }
    $source = [string]$manifest.source
    if ([string]::IsNullOrWhiteSpace($source)) { return $null }
    if (-not [IO.Path]::IsPathRooted($source)) {
        $source = Join-Path $Output $source
    }
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        return [IO.Path]::GetFullPath($source)
    }
    return $null
}

function Get-WinnerDetectionRows {
    # winner-detection.json の matches を読む（無い・壊れているときは空）。
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
    try {
        $payload = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        return @()
    }
    return @($payload.matches)
}

function Show-WinnerDetectionSummary {
    # winner-detection.json を読み直して、確定 / 要確認 / 引き分けの件数を出す。
    param([string]$Output, [string]$Label)
    $rows = @(Get-WinnerDetectionRows -Path (Join-Path $Output 'winner-detection.json'))
    if ($rows.Count -eq 0) { return }
    $draw = @($rows | Where-Object { $_.winner -eq 'draw' }).Count
    $undetermined = @($rows | Where-Object {
            $_.winner -ne 'draw' -and [string]::IsNullOrWhiteSpace([string]$_.winner)
        }).Count
    $review = @($rows | Where-Object {
            $_.winner -ne 'draw' -and
            -not [string]::IsNullOrWhiteSpace([string]$_.winner) -and
            $_.status -ne 'ok'
        }).Count
    $ok = $rows.Count - $draw - $undetermined - $review
    $line = "勝敗（$Label）: 確定$ok / 要確認$review / 引き分け$draw（計$($rows.Count)）"
    if ($undetermined -gt 0) { $line = "$line / 判定不能$undetermined" }
    Write-Host $line
}

function Invoke-WinnerSecondOpinion {
    # 迷った試合（draw / 要確認 / 判定不能）だけを動画方式で判定し直す。
    # インラインの票はハードラベルの多数決なので、結果画面の誤読がそのまま勝敗になる。
    # 同じ区間を動画から切り出して確率を平均すると誤読が薄まるため、該当試合だけ
    # ppt-end3-winner.py --only-matches <番号> --merge で差し替える。
    # 動画が無いバッチは何も変えず、その旨を出して続行する（解析全体は止めない）。
    param(
        [string]$Output,
        [string]$PythonExe,
        [string]$End3Script
    )
    if (-not (Test-Path -LiteralPath $End3Script -PathType Leaf)) { return }
    if (-not (Test-Path -LiteralPath $PythonExe -PathType Leaf)) { return }
    # PPT_WINNER_MODE=runs / replay は「動画方式を使わない」指定なので従う。
    if ($env:PPT_WINNER_MODE -in @('runs', 'replay')) { return }
    $winnerJson = Join-Path $Output 'winner-detection.json'
    $rows = @(Get-WinnerDetectionRows -Path $winnerJson)
    if ($rows.Count -eq 0) { return }
    $targets = @($rows | Where-Object { $_.needsSecondOpinion -eq $true } |
            ForEach-Object { [int]$_.match } | Sort-Object -Unique)
    if ($targets.Count -eq 0) { return }

    Write-Host ''
    Write-Host "勝敗の二重チェック: $($targets.Count)試合（$($targets -join ',')）を動画から判定し直します"
    $video = Get-BatchVideoPath -Output $Output
    if ([string]::IsNullOrWhiteSpace([string]$video)) {
        Write-Host '動画が無いため二重チェックを省略しました' -ForegroundColor Yellow
        Show-WinnerDetectionSummary -Output $Output -Label '現状'
        return
    }
    $matchList = $targets -join ','
    try {
        & $PythonExe -X utf8 $End3Script --batch-dir $Output --video $video `
            --workers 6 --only-matches $matchList --merge
        $secondExitCode = $LASTEXITCODE
    } catch {
        Write-Host "二重チェックを実行できませんでした: $($_.Exception.Message)" `
            -ForegroundColor Yellow
        return
    }
    if ($secondExitCode -ne 0) {
        Write-Host "二重チェックは終了コード $secondExitCode で終わりました（判定は元のままです）" `
            -ForegroundColor Yellow
        Show-WinnerDetectionSummary -Output $Output -Label '現状'
        return
    }
    Show-WinnerDetectionSummary -Output $Output -Label '二重チェック後'
}

function Invoke-WinnerDetection {
    # Winner detection (decide the match winner from the result screen).
    # 2026-11-02〜: 終了3クラスモデル（引き分け/P1/P2）を結果画面にだけ適用する。
    #   1. エンジン内で終了3クラスを適用済みなら classification_runs の票だけで決める（最速）
    #   2. 未適用なら ppt-end3-winner.py（動画から結果画面を切り出して3クラス判定）
    #   3. 動画が無い/遅いときは ppt-winner-from-runs.py（分類ログの票）
    #   4. それも無理なら detect-winners.py（旧WinnerCNNの再デコード）
    # PPT_WINNER_MODE=runs / replay で従来方式を強制できる。
    # A failure here must not fail the analysis itself, so it is swallowed.
    param([string]$Output)
    if ($env:PPT_NO_WINNER -eq '1') { return }
    $end3Script = 'C:\Users\hirom\Documents\tetris ai\tools\ppt-end3-winner.py'
    $runsScript = 'C:\Users\hirom\Documents\tetris ai\tools\ppt-winner-from-runs.py'
    $winnerScript = 'C:\Users\hirom\Documents\AI-Agent-Workspace\detect-winners.py'
    $pythonExe = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe'
    $detection = Join-Path $Output 'segments.matches.json'
    $winnerJson = Join-Path $Output 'winner-detection.json'
    $done = $false
    $runsTried = $false

    # エンジン内で終了3クラスモデルを適用した検出かどうか。
    # settings.end_state_model があれば classification_runs の 6=引き分け / 8=P1 / 9=P2。
    $inline = $false
    if (Test-Path -LiteralPath $detection -PathType Leaf) {
        try {
            $detPayload = Get-Content -LiteralPath $detection -Raw -Encoding UTF8 | ConvertFrom-Json
            $inline = [bool]($detPayload.settings.end_state_model)
        } catch {
            $inline = $false
        }
    }

    if (-not $done -and $inline -and $env:PPT_WINNER_MODE -ne 'replay' -and
        (Test-Path -LiteralPath $runsScript -PathType Leaf) -and
        (Test-Path -LiteralPath $detection -PathType Leaf) -and
        (Test-Path -LiteralPath $pythonExe -PathType Leaf)) {
        $runsTried = $true
        Write-Host ''
        Write-Host 'Detecting match winners (inline end-state runs)...'
        try {
            & $pythonExe -X utf8 $runsScript --batch-dir $Output
            $runsExitCode = $LASTEXITCODE
            if ($runsExitCode -eq 0) {
                $done = $true
                # 迷った試合だけ、動画の平均確率で二重チェックする（任意）。
                Invoke-WinnerSecondOpinion -Output $Output -PythonExe $pythonExe `
                    -End3Script $end3Script
            } elseif ($runsExitCode -eq 2) {
                Write-Host 'インラインの3クラス票では決められない試合が多すぎます。動画方式に切り替えます。' `
                    -ForegroundColor Yellow
            } else {
                Write-Host "Winner detection (inline runs) failed with exit code $runsExitCode" `
                    -ForegroundColor Yellow
            }
        } catch {
            Write-Host "Winner detection (inline runs) skipped: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }

    if (-not $done -and
        $env:PPT_WINNER_MODE -notin @('runs', 'replay') -and
        (Test-Path -LiteralPath $end3Script -PathType Leaf) -and
        (Test-Path -LiteralPath $detection -PathType Leaf) -and
        (Test-Path -LiteralPath $pythonExe -PathType Leaf)) {
        Write-Host ''
        Write-Host 'Detecting match winners (end-state 3-class model)...'
        try {
            & $pythonExe -X utf8 $end3Script --batch-dir $Output
            $end3ExitCode = $LASTEXITCODE
            if ($end3ExitCode -eq 0) {
                $done = $true
            } elseif ($end3ExitCode -eq 2) {
                Write-Host '結果画面が取れない試合が多すぎます。分類ログ方式に切り替えます。' `
                    -ForegroundColor Yellow
            } else {
                Write-Host "Winner detection (end3) failed with exit code $end3ExitCode" `
                    -ForegroundColor Yellow
            }
        } catch {
            Write-Host "Winner detection (end3) skipped: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }
    if (-not $done -and -not $runsTried -and
        $env:PPT_WINNER_MODE -ne 'replay' -and
        (Test-Path -LiteralPath $runsScript -PathType Leaf) -and
        (Test-Path -LiteralPath $detection -PathType Leaf) -and
        (Test-Path -LiteralPath $pythonExe -PathType Leaf)) {
        Write-Host ''
        Write-Host 'Detecting match winners (from scene classifications)...'
        try {
            & $pythonExe -X utf8 $runsScript --batch-dir $Output
            $runsExitCode = $LASTEXITCODE
            if ($runsExitCode -eq 0) {
                $done = $true
            } elseif ($runsExitCode -eq 2) {
                Write-Host '分類ログでは決められない試合が多すぎます（8クラスモデル等）。' `
                    -ForegroundColor Yellow
                Write-Host '従来の再デコード方式に切り替えます。' -ForegroundColor Yellow
            } else {
                Write-Host "Winner detection (runs) failed with exit code $runsExitCode" `
                    -ForegroundColor Yellow
            }
        } catch {
            Write-Host "Winner detection (runs) skipped: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }
    if (-not $done -and
        (Test-Path -LiteralPath $winnerScript -PathType Leaf) -and
        (Test-Path -LiteralPath $pythonExe -PathType Leaf)) {
        Write-Host ''
        Write-Host 'Detecting match winners (result screen)...'
        $winnerBatch = Split-Path -Leaf $Output
        # detect-winners.py は runtime\data\batches\<バッチ名> を引く。-OutputDirectory で
        # バッチ外へ出した解析（検証用）は名前では引けないので絶対パスを渡す
        # （Python 側は Path の結合で絶対パス側が優先される）。
        $registeredBatch = Join-Path $runtime (Join-Path 'data\batches' $winnerBatch)
        if (-not (Test-Path -LiteralPath $registeredBatch -PathType Container)) {
            $winnerBatch = $Output
        }
        try {
            & $pythonExe -X utf8 $winnerScript --batch $winnerBatch
            if ($LASTEXITCODE -ne 0) {
                Write-Host "Winner detection failed with exit code $LASTEXITCODE" -ForegroundColor Yellow
            } elseif (-not (Test-Path -LiteralPath $winnerJson -PathType Leaf)) {
                Write-Host "Winner detection finished without $winnerJson" -ForegroundColor Yellow
            }
        } catch {
            Write-Host "Winner detection skipped: $($_.Exception.Message)" -ForegroundColor Yellow
        }
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
        [IO.Path]::GetFullPath($BatchExecutable)
    }
    $sceneModel = if ([string]::IsNullOrWhiteSpace($SceneModelPath)) {
        Join-Path $PSScriptRoot 'ppt-scene-classifier\artifacts\scene-classifier-fp32.onnx'
    } else {
        [IO.Path]::GetFullPath($SceneModelPath)
    }
    $boardModel = if ([string]::IsNullOrWhiteSpace($BoardModelPath)) {
        Join-Path $runtime 'models\tetris.onnx'
    } else {
        [IO.Path]::GetFullPath($BoardModelPath)
    }
    $settings = if ([string]::IsNullOrWhiteSpace($SettingsPath)) {
        Join-Path $runtime 'config\tetris_recover.ini'
    } else {
        [IO.Path]::GetFullPath($SettingsPath)
    }
    $cuda = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin\x64'

    # 解析を始める前に、必要なファイル・DLL・GPU・空き容量を確認する。
    # 1つでも足りなければ解析には入らず、終了コード 2 で止まる
    # （失敗の一言ではなく、足りないものと対処を出す）。
    if (-not $SkipEnvironmentCheck) {
        if ($null -eq (Get-Command Test-PptAnalysisEnvironment -ErrorAction SilentlyContinue)) {
            throw "解析前チェックのスクリプトが見つかりません: $preflightHelper"
        }
        $environment = Test-PptAnalysisEnvironment -ToolsRoot $PSScriptRoot `
            -EnginePath $batch -SceneModelPath $sceneModel `
            -BoardModelPath $boardModel -SettingsPath $settings -OutputPath $output
        if (-not $environment.Ok) {
            Write-Host ''
            Write-Host 'このままでは解析を開始できません。上記を直してから再実行してください。' `
                -ForegroundColor Red
            exit 2
        }
    }

    foreach ($required in @($batch, $sceneModel, $boardModel, $settings)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Required file is missing: $required"
        }
    }

    $env:PATH = "$($runtime)\bin;$cuda;$env:PATH"
    # エンジン内で終了3クラスモデルを適用する（存在するときだけ）。
    # 旧エンジンはこの環境変数を無視するので、差し替え前でも安全。
    $endStateModel = Join-Path $PSScriptRoot 'ppt-lab\models\end3\scene-classifier.onnx'
    if (Test-Path -LiteralPath $endStateModel -PathType Leaf) {
        $env:TR_END_STATE_MODEL = $endStateModel
    } else {
        Remove-Item Env:TR_END_STATE_MODEL -ErrorAction SilentlyContinue
    }
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
        # 解析はやり直さないが、勝敗が欠けたバッチ（この修正より前に作られたもの）は
        # 勝敗ステップだけ実行して埋める。有効な勝敗が既にあれば何もしない。
        if ($env:PPT_NO_WINNER -ne '1') {
            $winnerState = Get-WinnerDetectionState -Output $output
            if (-not [string]::IsNullOrEmpty($winnerState)) {
                Write-Host "勝敗が未確定のため、勝敗判定だけ実行します（$winnerState）" `
                    -ForegroundColor Yellow
                Invoke-WinnerDetection -Output $output
            }
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
        # 実測のピークは 1 ワーカーあたり約 1321MB。1024MB では 29% 過小予算になり、
        # スケジューラが「載る」と誤認して VRAM 不足を招いていた。
        $env:TR_BATCH_WORKER_VRAM_MB = '1400'
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
    # キャッシュヒット時も同じ関数を通すため、処理は Invoke-WinnerDetection に集約している。
    Invoke-WinnerDetection -Output $output
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
