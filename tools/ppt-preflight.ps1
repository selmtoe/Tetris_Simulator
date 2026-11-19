# PPT Lab: 解析前の環境チェック
#
# run-ppt-stream-analysis.ps1 からドットソースして Test-PptAnalysisEnvironment を呼ぶ。
# 解析を始める前に、必要なファイル・DLL・Python・GPU・空き容量を絶対パスで確認する。
# 1つでも足りなければ Ok=$false を返し、呼び出し側が終了コード 2 で止める。
#
# このファイルは UTF-8 BOM 付きで保存すること（BOM が無いと日本語が壊れる）。
# 既存の処理フローを壊さないよう、ここでは解析そのものは一切実行しない。

# 出力先ドライブに必要な空き容量（GB）。足りなくても警告だけで続行する。
$script:PptPreflightMinimumFreeGB = 5

function Get-PptPreflightFullPath {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return '' }
    try {
        return [IO.Path]::GetFullPath($Path)
    } catch {
        return $Path
    }
}

function Write-PptPreflightOk {
    param([string]$Label, [string]$Detail = '')
    if ([string]::IsNullOrWhiteSpace($Detail)) {
        Write-Host ("✅ {0}" -f $Label) -ForegroundColor Green
    } else {
        Write-Host ("✅ {0}  {1}" -f $Label, $Detail) -ForegroundColor Green
    }
}

function Write-PptPreflightWarning {
    param([string]$Message, [string]$Detail = '', [string]$Advice = '')
    Write-Host ("⚠️  {0}" -f $Message) -ForegroundColor Yellow
    if (-not [string]::IsNullOrWhiteSpace($Detail)) {
        Write-Host ("   {0}" -f $Detail) -ForegroundColor DarkYellow
    }
    if (-not [string]::IsNullOrWhiteSpace($Advice)) {
        Write-Host ("   対処: {0}" -f $Advice) -ForegroundColor Yellow
    }
}

function Write-PptPreflightFailure {
    param([string]$Message, [string]$Detail = '', [string[]]$Fix = @())
    Write-Host ("❌ {0}" -f $Message) -ForegroundColor Red
    if (-not [string]::IsNullOrWhiteSpace($Detail)) {
        Write-Host ("   {0}" -f $Detail) -ForegroundColor DarkYellow
    }
    $first = $true
    foreach ($line in @($Fix)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        if ($first) {
            Write-Host ("   対処: {0}" -f $line) -ForegroundColor Yellow
            $first = $false
        } elseif ($line -match '^(手動|参考|場所):') {
            Write-Host ("   {0}" -f $line) -ForegroundColor DarkGray
        } else {
            Write-Host ("         {0}" -f $line) -ForegroundColor DarkYellow
        }
    }
}

# 配布物や README への導線。インストーラーが同梱されていればそれを優先する。
function Get-PptPreflightSetupFix {
    param([string]$ToolsRoot)
    $setup = Join-Path $ToolsRoot 'ppt-setup\install.ps1'
    if (Test-Path -LiteralPath $setup -PathType Leaf) {
        return 'tools\ppt-setup\install.ps1 を実行してください（1クリックで導入できます）'
    }
    return '配布物・バックアップの tools\video-recovery-runtime からコピーしてください'
}

<#
.SYNOPSIS
解析前の環境チェックを行い、結果を色付きで表示する。

.DESCRIPTION
- 解析エンジン / 必須DLL / ffmpeg.exe / モデル / 設定 / Python は必須。
- GPU と空き容量は警告のみ（解析そのものは続行できる）。
- 失敗は Failures、警告は Warnings に日本語で入る。Ok は失敗が0件かどうか。

.PARAMETER ToolsRoot
tools フォルダー（既定はこのスクリプトのフォルダー）。

.PARAMETER EnginePath
実際に使う解析エンジン。空なら runtime\bin\TetrisVideoRecovery_stream.exe。

.PARAMETER SceneModelPath / BoardModelPath / SettingsPath
呼び出し側が明示した解析モデル・設定。空なら既定の場所を使う。

.PARAMETER OutputPath
解析の出力先（空き容量の確認に使う）。

.PARAMETER MinimumFreeGB
出力先ドライブに必要な空き容量。既定は 5GB。
#>
function Test-PptAnalysisEnvironment {
    param(
        [string]$ToolsRoot = '',
        [string]$EnginePath = '',
        [string]$SceneModelPath = '',
        [string]$BoardModelPath = '',
        [string]$SettingsPath = '',
        [string]$OutputPath = '',
        [double]$MinimumFreeGB = 5
    )

    if ([string]::IsNullOrWhiteSpace($ToolsRoot)) { $ToolsRoot = $PSScriptRoot }
    $toolsRoot = Get-PptPreflightFullPath $ToolsRoot
    $runtime = Join-Path $toolsRoot 'video-recovery-runtime'
    $bin = Join-Path $runtime 'bin'

    $failures = New-Object System.Collections.Generic.List[string]
    $warnings = New-Object System.Collections.Generic.List[string]

    Write-Host ''
    Write-Host '=== 解析前チェック ===' -ForegroundColor Cyan

    # ---- 1. 解析エンジン -------------------------------------------------
    $engine = if ([string]::IsNullOrWhiteSpace($EnginePath)) {
        Join-Path $bin 'TetrisVideoRecovery_stream.exe'
    } else {
        Get-PptPreflightFullPath $EnginePath
    }
    $engineExists = Test-Path -LiteralPath $engine -PathType Leaf
    if ($engineExists) {
        Write-PptPreflightOk '解析エンジン' (Split-Path -Leaf $engine)
    } else {
        $failures.Add('解析エンジンが見つかりません')
        Write-PptPreflightFailure -Message '解析エンジンが見つかりません' `
            -Detail ('場所: ' + $engine) `
            -Fix @(
                (Get-PptPreflightSetupFix -ToolsRoot $toolsRoot),
                'tools\video-recovery-source からビルドして bin に置く方法もあります')
    }

    # ---- 2. 必須DLL（エンジンと同じフォルダー） --------------------------
    $engineDir = if ($engineExists) { Split-Path -Parent $engine } else { $bin }
    $requiredDlls = @(
        'onnxruntime.dll',
        'onnxruntime_providers_cuda.dll',
        'onnxruntime_providers_shared.dll',
        'avcodec-63.dll',
        'avformat-63.dll',
        'avutil-61.dll')
    $missingDlls = New-Object System.Collections.Generic.List[string]
    foreach ($name in $requiredDlls) {
        if (-not (Test-Path -LiteralPath (Join-Path $engineDir $name) -PathType Leaf)) {
            $missingDlls.Add($name)
        }
    }
    # cuDNN は 9 系の統合DLL。無ければ分割DLL（cudnn*.dll）でもよい。
    $cudnn = Join-Path $engineDir 'cudnn64_9.dll'
    $cudnnFound = Test-Path -LiteralPath $cudnn -PathType Leaf
    $cudnnName = 'cudnn64_9.dll'
    if (-not $cudnnFound) {
        $cudnnAlt = @()
        try {
            $cudnnAlt = @(Get-ChildItem -LiteralPath $engineDir -Filter 'cudnn*.dll' `
                    -File -ErrorAction SilentlyContinue)
        } catch {
            $cudnnAlt = @()
        }
        if ($cudnnAlt.Count -gt 0) {
            $cudnnFound = $true
            $cudnnName = $cudnnAlt[0].Name
        }
    }
    if (-not $cudnnFound) { $missingDlls.Add('cudnn64_9.dll') }

    if ($missingDlls.Count -eq 0) {
        Write-PptPreflightOk '必須DLL' ("onnxruntime / cuDNN / FFmpeg ランタイム（{0}）" -f $cudnnName)
    } else {
        $failures.Add('必須DLLが見つかりません')
        Write-PptPreflightFailure -Message '必須DLLが見つかりません' `
            -Detail ('不足: ' + ($missingDlls -join ', ')) `
            -Fix @(
                ('解析エンジンと同じフォルダーに配布物・バックアップのDLLをコピーしてください: ' + $engineDir),
                'cudnn 系は CUDA 13 対応の cuDNN 9 のDLL一式が必要です')
    }

    # ---- 3. ffmpeg.exe ---------------------------------------------------
    $ffmpeg = Join-Path $bin 'ffmpeg.exe'
    if (Test-Path -LiteralPath $ffmpeg -PathType Leaf) {
        Write-PptPreflightOk 'ffmpeg.exe'
    } else {
        $failures.Add('ffmpeg.exe が見つかりません')
        Write-PptPreflightFailure -Message 'ffmpeg.exe が見つかりません' `
            -Fix @(
                (Get-PptPreflightSetupFix -ToolsRoot $toolsRoot),
                '手動: https://www.gyan.dev/ffmpeg/builds/ からダウンロードして',
                'tools\video-recovery-runtime\bin\ffmpeg.exe に置いてください')
    }

    # ---- 4. 解析モデル ---------------------------------------------------
    $modelRoot = Join-Path $toolsRoot 'ppt-lab\models'
    $modelEntries = New-Object System.Collections.Generic.List[object]
    foreach ($entry in @(
            @{ Label = 'fast320\scene-classifier.onnx'; Path = (Join-Path $modelRoot 'fast320\scene-classifier.onnx') },
            @{ Label = 'fast320\tetris.onnx'; Path = (Join-Path $modelRoot 'fast320\tetris.onnx') },
            @{ Label = 'fast320\next_hold.onnx'; Path = (Join-Path $modelRoot 'fast320\next_hold.onnx') },
            @{ Label = 'end3\scene-classifier.onnx'; Path = (Join-Path $modelRoot 'end3\scene-classifier.onnx') })) {
        $modelEntries.Add($entry)
    }
    # 呼び出し側が別のモデルを指定したときは、その実ファイルも確認する。
    foreach ($entry in @(
            @{ Label = 'シーン分類モデル（指定）'; Path = $SceneModelPath },
            @{ Label = '盤面モデル（指定）'; Path = $BoardModelPath })) {
        if ([string]::IsNullOrWhiteSpace($entry.Path)) { continue }
        $modelEntries.Add(@{
                Label = $entry.Label
                Path = (Get-PptPreflightFullPath $entry.Path)
            })
    }
    $seenModels = @{}
    $missingModels = New-Object System.Collections.Generic.List[object]
    $checkedModels = New-Object System.Collections.Generic.List[string]
    foreach ($model in $modelEntries) {
        $modelPath = [string]$model.Path
        $key = $modelPath.ToLowerInvariant()
        if ($seenModels.ContainsKey($key)) { continue }
        $seenModels[$key] = $true
        $checkedModels.Add([string]$model.Label)
        if (-not (Test-Path -LiteralPath $modelPath -PathType Leaf)) {
            $missingModels.Add($model)
        }
    }
    if ($missingModels.Count -eq 0) {
        Write-PptPreflightOk '解析モデル' ("{0} 件" -f $checkedModels.Count)
    } else {
        foreach ($model in $missingModels) {
            $failures.Add('解析モデルが見つかりません: ' + $model.Label)
            Write-PptPreflightFailure -Message ('解析モデルが見つかりません: ' + $model.Label) `
                -Detail ('場所: ' + $model.Path) `
                -Fix @(
                    '配布物・バックアップの tools\ppt-lab\models を元に戻してください',
                    '独自のモデルを使う場合は設定「解析モデル」を確認してください')
        }
    }

    # ---- 5. 設定ファイル -------------------------------------------------
    $settingsFiles = New-Object System.Collections.Generic.List[string]
    $settingsFiles.Add((Join-Path $runtime 'config\tetris_recover.ini'))
    if (-not [string]::IsNullOrWhiteSpace($SettingsPath)) {
        $settingsFiles.Add((Get-PptPreflightFullPath $SettingsPath))
    }
    $seenSettings = @{}
    foreach ($settingsFile in $settingsFiles) {
        $key = $settingsFile.ToLowerInvariant()
        if ($seenSettings.ContainsKey($key)) { continue }
        $seenSettings[$key] = $true
        if (Test-Path -LiteralPath $settingsFile -PathType Leaf) {
            Write-PptPreflightOk ('設定 ' + (Split-Path -Leaf $settingsFile))
        } else {
            $failures.Add('設定ファイルが見つかりません: ' + (Split-Path -Leaf $settingsFile))
            Write-PptPreflightFailure -Message ('設定ファイルが見つかりません: ' + (Split-Path -Leaf $settingsFile)) `
                -Detail ('場所: ' + $settingsFile) `
                -Fix @('tools\video-recovery-runtime\config\tetris_recover.ini を元に戻してください')
        }
    }

    # ---- 6. Python -------------------------------------------------------
    $localAppData = $env:LOCALAPPDATA
    if ([string]::IsNullOrWhiteSpace($localAppData)) {
        $localAppData = Join-Path $env:USERPROFILE 'AppData\Local'
    }
    $python311 = Join-Path $localAppData 'Programs\Python\Python311\python.exe'
    $python = ''
    $pythonLabel = ''
    if (Test-Path -LiteralPath $python311 -PathType Leaf) {
        $python = $python311
        $pythonLabel = 'Python 3.11'
    } else {
        $pyLauncher = Get-Command 'py.exe' -ErrorAction SilentlyContinue
        if ($null -ne $pyLauncher) {
            try {
                $null = & $pyLauncher.Source -3 -c 'import sys' 2>$null
                if ($LASTEXITCODE -eq 0) {
                    $python = $pyLauncher.Source
                    $pythonLabel = 'Python (py -3)'
                }
            } catch {
                $python = ''
            }
        }
        if ([string]::IsNullOrWhiteSpace($python)) {
            $pythonCmd = Get-Command 'python.exe' -ErrorAction SilentlyContinue
            if ($null -ne $pythonCmd) {
                $python = $pythonCmd.Source
                $pythonLabel = 'Python (python)'
            }
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($python)) {
        Write-PptPreflightOk $pythonLabel $python
    } else {
        $failures.Add('Python が見つかりません')
        Write-PptPreflightFailure -Message 'Python が見つかりません' `
            -Detail ('探した場所: ' + $python311) `
            -Fix @(
                'Python 3.11 を https://www.python.org/downloads/ からインストールしてください（Add python.exe to PATH を有効にする）',
                'インストール済みなら py -3 か python で起動できるようにしてください')
    }

    # ---- 7. GPU（警告のみ） ----------------------------------------------
    $gpuName = ''
    $gpuOk = $false
    $nvidiaSmi = Get-Command 'nvidia-smi.exe' -ErrorAction SilentlyContinue
    if ($null -ne $nvidiaSmi) {
        try {
            $gpuLines = @(& $nvidiaSmi.Source '--query-gpu=name' '--format=csv,noheader' 2>$null)
            if ($LASTEXITCODE -eq 0 -and $gpuLines.Count -gt 0) {
                $gpuName = ([string]$gpuLines[0]).Trim()
                $gpuOk = -not [string]::IsNullOrWhiteSpace($gpuName)
            }
        } catch {
            $gpuOk = $false
        }
    }
    if ($gpuOk) {
        Write-PptPreflightOk 'GPU' $gpuName
    } else {
        $warnings.Add('GPU（nvidia-smi）を確認できません')
        Write-PptPreflightWarning -Message 'GPU（nvidia-smi）を確認できません' `
            -Detail 'GPU が無くても解析はできますが、非常に遅くなります' `
            -Advice '設定「精度モード」を CPU にするか、NVIDIA ドライバーを更新してください'
    }

    # ---- 8. 空き容量（警告のみ） ------------------------------------------
    $spaceTarget = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $toolsRoot
    } else {
        Get-PptPreflightFullPath $OutputPath
    }
    try {
        $rootPath = [IO.Path]::GetPathRoot($spaceTarget)
        if ([string]::IsNullOrWhiteSpace($rootPath)) { throw 'ドライブを特定できません' }
        $drive = New-Object -TypeName System.IO.DriveInfo -ArgumentList $rootPath
        $freeGB = ([double]$drive.AvailableFreeSpace) / 1GB
        $freeText = '{0} {1:N1} GB 空き' -f $drive.Name, $freeGB
        if ($freeGB -ge $MinimumFreeGB) {
            Write-PptPreflightOk '空き容量' $freeText
        } else {
            $warnings.Add('空き容量が足りません')
            Write-PptPreflightWarning -Message ('空き容量が足りません（必要 {0:N0} GB）' -f $MinimumFreeGB) `
                -Detail ('出力先: ' + $freeText) `
                -Advice '古い解析結果を削除するか、設定「解析の出力先」を空きのあるドライブに変更してください'
        }
    } catch {
        $warnings.Add('空き容量を確認できません')
        Write-PptPreflightWarning -Message '空き容量を確認できません' `
            -Detail ('出力先: ' + $spaceTarget)
    }

    # ---- 結果 -----------------------------------------------------------
    Write-Host ''
    if ($failures.Count -gt 0) {
        Write-Host ("=== チェック結果: 失敗 {0} 件 / 警告 {1} 件 ===" -f `
                $failures.Count, $warnings.Count) -ForegroundColor Red
    } elseif ($warnings.Count -gt 0) {
        Write-Host ("=== チェック結果: すべて OK（警告 {0} 件）===" -f $warnings.Count) `
            -ForegroundColor Yellow
    } else {
        Write-Host '=== チェック結果: すべて OK ===' -ForegroundColor Green
    }

    return [pscustomobject]@{
        Ok = ($failures.Count -eq 0)
        Failures = @($failures)
        Warnings = @($warnings)
    }
}
