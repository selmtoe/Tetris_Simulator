#Requires -Version 5.1
<#
.SYNOPSIS
    PPT Lab（ぷよテト動画解析ラボ）アンインストーラ。

.DESCRIPTION
    次のものを、確認しながら順番に削除します。

      1. デスクトップ / スタートメニューのショートカット
      2. スケジュールタスク「PPT RAM disk」（と R: ドライブ）
      3. インストーラが作った作業フォルダー（cuda / python / downloads / backup）
      4. アプリのデータ（%LOCALAPPDATA%\PPTLab と解析結果）※既定では消しません

    アプリ本体（server.py / 解析エンジン / モデル / 設定）は削除しません。
    アプリごと消したい場合は、このリポジトリのフォルダーを丸ごと削除してください。

    破壊的な操作の前には必ず確認プロンプトを出します。
    データ削除だけは「DELETE」と入力してもらう二重確認にしています。

.PARAMETER DryRun
    何も削除せず、削除する予定のものだけを表示します。

.PARAMETER Yes
    ショートカット・タスク・作業フォルダーの確認に「はい」と答えたものとして進めます。
    データ削除は -PurgeData を明示しない限り行いません。

.PARAMETER KeepData
    データ（%LOCALAPPDATA%\PPTLab と解析結果）の確認自体を出しません。

.PARAMETER PurgeData
    データも削除します（-Yes と併用すると完全に無人で削除します）。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1 -DryRun

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1

.NOTES
    文字コード: UTF-8 (BOM)。Windows PowerShell 5.1 / 7 の両方で動作します。
#>
[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Yes,
    [switch]$KeepData,
    [switch]$PurgeData
)

$ErrorActionPreference = 'Stop'

$script:DryRun = [bool]$DryRun
$script:Yes = [bool]$Yes
$script:KeepData = [bool]$KeepData
$script:PurgeData = [bool]$PurgeData

$script:SetupRoot = $PSScriptRoot
$script:ToolRoot = Split-Path -Parent $script:SetupRoot
$script:RepoRoot = Split-Path -Parent $script:ToolRoot
$script:DataDir = Join-Path $env:LOCALAPPDATA 'PPTLab'
$script:BatchesDir = Join-Path (Join-Path $script:ToolRoot 'video-recovery-runtime\data') 'batches'
$script:TaskName = 'PPT RAM disk'
$script:LogPath = Join-Path $env:TEMP ('PPTLab-uninstall-{0}.log' -f (Get-Date).ToString('yyyyMMdd-HHmmss'))
$script:Removed = New-Object System.Collections.ArrayList
$script:Skipped = New-Object System.Collections.ArrayList

# コンソールが UTF-8 でないときは記号を ASCII に落とす（日本語 Windows の CP932 対策）
$script:MarkOk = '✅'
$script:MarkSkip = '－'
try {
    if ([Console]::OutputEncoding.CodePage -ne 65001) {
        $script:MarkOk = '[OK]'
        $script:MarkSkip = '[--]'
    }
} catch {
    $script:MarkOk = '[OK]'
    $script:MarkSkip = '[--]'
}
if ($env:PPTLAB_ASCII -eq '1') { $script:MarkOk = '[OK]'; $script:MarkSkip = '[--]' }

# ---------------------------------------------------------------------------
# 表示とログ
# ---------------------------------------------------------------------------

function Write-Log {
    param([string]$Level, [string]$Message, [string]$Color = 'Gray')
    if ($Color -eq 'None') { Write-Host $Message } else { Write-Host $Message -ForegroundColor $Color }
    try {
        $line = '[{0}] [{1}] {2}' -f (Get-Date).ToString('HH:mm:ss'), $Level, ($Message -replace "`r", '')
        Add-Content -LiteralPath $script:LogPath -Value $line -Encoding UTF8
    } catch { }
}

function Confirm-YesNo {
    param([string]$Question, [bool]$Default = $false)
    # -Yes を最優先（-DryRun と併用したときは「予定の全量」を確認したいため）
    if ($script:Yes) {
        Write-Log -Level 'INFO' -Message ('  確認: {0} → 「はい」(-Yes)' -f $Question) -Color Gray
        return $true
    }
    if ($script:DryRun) {
        Write-Log -Level 'DRYRUN' -Message ('  [DryRun] 確認: {0} → 既定「{1}」' -f $Question, $(if ($Default) { 'はい' } else { 'いいえ' })) -Color Magenta
        return $Default
    }
    $hint = '[y/N]'
    if ($Default) { $hint = '[Y/n]' }
    while ($true) {
        $answer = $null
        try { $answer = Read-Host ('  ? {0} {1}' -f $Question, $hint) } catch { return $Default }
        if ($null -eq $answer -or $answer.Trim().Length -eq 0) { return $Default }
        switch ($answer.Trim().ToLowerInvariant()) {
            'y' { return $true }
            'yes' { return $true }
            'はい' { return $true }
            'n' { return $false }
            'no' { return $false }
            'いいえ' { return $false }
            default { Write-Host '   y か n で答えてください（Enter だけなら既定）。' -ForegroundColor Yellow }
        }
    }
}

function Confirm-DeleteData {
    <#
      データ削除だけは「DELETE」と打ってもらう。誤操作で消えるのを防ぐため。
    #>
    if ($script:PurgeData) {
        Write-Log -Level 'INFO' -Message '  データ削除: -PurgeData が指定されています' -Color Yellow
        return $true
    }
    if ($script:DryRun) {
        Write-Log -Level 'DRYRUN' -Message '  [DryRun] データ削除は「DELETE」入力の二重確認になります（実行時）' -Color Magenta
        return $false
    }
    if ($script:Yes) {
        # -Yes だけではデータを消さない（事故防止）
        Write-Log -Level 'WARN' -Message '  -Yes だけではデータは削除しません。-PurgeData を付けてください。' -Color Yellow
        return $false
    }
    Write-Host ''
    Write-Host '  データを削除すると、解析した動画の一覧・DB・キャッシュが戻せません。' -ForegroundColor Yellow
    while ($true) {
        $answer = $null
        try { $answer = Read-Host '  よければ「DELETE」と入力してください（やめるなら Enter）' } catch { return $false }
        if ($null -eq $answer -or $answer.Trim().Length -eq 0) { return $false }
        if ($answer.Trim() -ceq 'DELETE') { return $true }
        Write-Host '   「DELETE」と完全に一致したときだけ削除します。' -ForegroundColor Yellow
    }
}

function Get-DirectorySizeText {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return 'なし' }
    try {
        $bytes = (Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue |
            Measure-Object -Property Length -Sum).Sum
        if (-not $bytes) { $bytes = 0 }
        if ($bytes -ge 1GB) { return ('{0:N2} GB' -f ($bytes / 1GB)) }
        if ($bytes -ge 1MB) { return ('{0:N1} MB' -f ($bytes / 1MB)) }
        return ('{0:N0} KB' -f ($bytes / 1KB))
    } catch {
        return '不明'
    }
}

function Remove-PathChecked {
    <#
      実在を確認してから消す。DryRun では消さずに予定だけ出す。
    #>
    param(
        [string]$Path,
        [string]$Label,
        [switch]$Directory
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        [void]$script:Skipped.Add(('{0}（元から無い）' -f $Label))
        Write-Log -Level 'SKIP' -Message ('  {0} {1}: 元からありません' -f $script:MarkSkip, $Label) -Color DarkGray
        return
    }
    if ($script:DryRun) {
        Write-Log -Level 'DRYRUN' -Message ('  [DryRun] 削除予定: {0}' -f $Path) -Color Magenta
        [void]$script:Removed.Add(('{0}（予定）' -f $Label))
        return
    }
    try {
        if ($Directory) {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
        } else {
            Remove-Item -LiteralPath $Path -Force -ErrorAction Stop
        }
        Write-Log -Level 'OK' -Message ('  {0} {1} を削除しました' -f $script:MarkOk, $Label) -Color Green
        [void]$script:Removed.Add($Label)
    } catch {
        Write-Log -Level 'NG' -Message ('  ❌ {0} を削除できませんでした: {1}' -f $Label, $_.Exception.Message) -Color Red
        [void]$script:Skipped.Add($Label)
    }
}

function Test-Admin {
    try {
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        $principal = New-Object Security.Principal.WindowsPrincipal($identity)
        return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch {
        return $false
    }
}

# ---------------------------------------------------------------------------
# 表示
# ---------------------------------------------------------------------------

$rule = '=' * 70
Write-Host ''
Write-Log -Level 'HEAD' -Message $rule -Color DarkCyan
Write-Log -Level 'HEAD' -Message '  PPT Lab（ぷよテト動画解析ラボ）アンインストール' -Color Cyan
Write-Log -Level 'HEAD' -Message $rule -Color DarkCyan
Write-Log -Level 'HEAD' -Message ('  対象      : {0}' -f $script:RepoRoot) -Color Gray
Write-Log -Level 'HEAD' -Message ('  ログ      : {0}' -f $script:LogPath) -Color DarkGray
if ($script:DryRun) {
    Write-Host ''
    Write-Host '  ※ DryRun モードです。何も削除しません（予定だけ表示します）。' -ForegroundColor Magenta
}
Write-Host ''

$admin = Test-Admin
if (-not $admin) {
    Write-Log -Level 'INFO' -Message '  ! 管理者権限がありません。スケジュールタスクの削除だけは後回しになります。' -Color Yellow
    Write-Log -Level 'INFO' -Message '    （ショートカットとデータの削除は、このままでも実行できます）' -Color DarkGray
}

# 何を消すかの下見
$desktop = [Environment]::GetFolderPath('Desktop')
$startMenu = [Environment]::GetFolderPath('Programs')
$desktopLink = Join-Path $desktop 'PPT Lab.lnk'
$startMenuFolder = Join-Path $startMenu 'PPT Lab'
$task = $null
try { $task = Get-ScheduledTask -TaskName $script:TaskName -ErrorAction Stop } catch { $task = $null }
$workDirs = @(
    (Join-Path $script:SetupRoot 'cuda'),
    (Join-Path $script:SetupRoot 'python'),
    (Join-Path $script:SetupRoot 'downloads'),
    (Join-Path $script:SetupRoot 'backup'),
    (Join-Path $script:SetupRoot 'licenses')
)

Write-Log -Level 'INFO' -Message '  これから確認する項目:' -Color White
Write-Log -Level 'INFO' -Message ('   ・デスクトップのショートカット : {0}' -f $(if (Test-Path -LiteralPath $desktopLink) { 'あり' } else { 'なし' })) -Color Gray
Write-Log -Level 'INFO' -Message ('   ・スタートメニュー             : {0}' -f $(if (Test-Path -LiteralPath $startMenuFolder) { 'あり' } else { 'なし' })) -Color Gray
Write-Log -Level 'INFO' -Message ('   ・タスク「{0}」   : {1}' -f $script:TaskName, $(if ($task) { 'あり' } else { 'なし' })) -Color Gray
foreach ($dir in $workDirs) {
    if (Test-Path -LiteralPath $dir) {
        Write-Log -Level 'INFO' -Message ('   ・{0} : {1}' -f (Split-Path -Leaf $dir), (Get-DirectorySizeText $dir)) -Color Gray
    }
}
Write-Log -Level 'INFO' -Message ('   ・データ {0} : {1}' -f $script:DataDir, (Get-DirectorySizeText $script:DataDir)) -Color Gray
Write-Log -Level 'INFO' -Message ('   ・解析結果 {0} : {1}' -f $script:BatchesDir, (Get-DirectorySizeText $script:BatchesDir)) -Color Gray
Write-Host ''
Write-Log -Level 'INFO' -Message '  ※ アプリ本体（server.py / 解析エンジン / モデル / 設定）は消しません。' -Color DarkGray
Write-Host ''

$start = Confirm-YesNo -Question 'アンインストールを始めますか？' -Default $false
if (-not $start) {
    Write-Host ''
    Write-Log -Level 'INFO' -Message '  中止しました。何も変更していません。' -Color Yellow
    Write-Host ''
    exit 0
}

# ---------------------------------------------------------------------------
# 1. ショートカット
# ---------------------------------------------------------------------------

Write-Host ''
Write-Log -Level 'STEP' -Message '  [1/4] ショートカットを削除する' -Color Cyan
if (Confirm-YesNo -Question 'デスクトップとスタートメニューのショートカットを削除しますか？' -Default $true) {
    Remove-PathChecked -Path $desktopLink -Label 'デスクトップのショートカット'
    if (Test-Path -LiteralPath $startMenuFolder) {
        Remove-PathChecked -Path $startMenuFolder -Label 'スタートメニューの「PPT Lab」' -Directory
    } else {
        [void]$script:Skipped.Add('スタートメニューの「PPT Lab」（元から無い）')
        Write-Log -Level 'SKIP' -Message ('  {0} スタートメニュー: 元からありません' -f $script:MarkSkip) -Color DarkGray
    }
} else {
    Write-Log -Level 'SKIP' -Message '  ショートカットは残します。' -Color Yellow
}

# ---------------------------------------------------------------------------
# 2. スケジュールタスクと RAM ディスク
# ---------------------------------------------------------------------------

Write-Host ''
Write-Log -Level 'STEP' -Message '  [2/4] RAM ディスクのタスクを削除する' -Color Cyan
if ($null -eq $task) {
    Write-Log -Level 'SKIP' -Message ('  {0} タスク「{1}」はありません' -f $script:MarkSkip, $script:TaskName) -Color DarkGray
} elseif (-not $admin) {
    Write-Log -Level 'WARN' -Message '  管理者権限が無いため、タスクを削除できません。' -Color Yellow
    Write-Log -Level 'WARN' -Message ('  管理者の PowerShell で次を実行してください:  Unregister-ScheduledTask -TaskName "{0}" -Confirm:$false' -f $script:TaskName) -Color Yellow
} elseif (Confirm-YesNo -Question ('スケジュールタスク「{0}」を削除しますか？' -f $script:TaskName) -Default $true) {
    if ($script:DryRun) {
        Write-Log -Level 'DRYRUN' -Message ('  [DryRun] 削除予定: タスク「{0}」' -f $script:TaskName) -Color Magenta
    } else {
        try {
            Unregister-ScheduledTask -TaskName $script:TaskName -Confirm:$false -ErrorAction Stop
            Write-Log -Level 'OK' -Message ('  {0} タスク「{1}」を削除しました' -f $script:MarkOk, $script:TaskName) -Color Green
            [void]$script:Removed.Add('スケジュールタスク「' + $script:TaskName + '」')
        } catch {
            Write-Log -Level 'NG' -Message ('  ❌ タスクを削除できませんでした: {0}' -f $_.Exception.Message) -Color Red
        }
    }
    # R: ドライブも片付ける（ImDisk があるときだけ）
    $ramDiskScript = Join-Path $script:ToolRoot 'ppt-ramdisk.ps1'
    $imdisk = Join-Path $env:SystemRoot 'System32\imdisk.exe'
    if ((Test-Path -LiteralPath $ramDiskScript) -and (Test-Path -LiteralPath $imdisk)) {
        if (Confirm-YesNo -Question 'RAM ディスク（R:）も今すぐ削除しますか？（中のファイルは消えます）' -Default $true) {
            if ($script:DryRun) {
                Write-Log -Level 'DRYRUN' -Message '  [DryRun] 削除予定: R: ドライブ（RAM ディスク）' -Color Magenta
            } else {
                try {
                    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ramDiskScript -DriveLetter R -Remove 2>&1 |
                        ForEach-Object { Write-Log -Level 'INFO' -Message ('    {0}' -f $_) -Color DarkGray }
                    Write-Log -Level 'OK' -Message ('  {0} R: ドライブを削除しました' -f $script:MarkOk) -Color Green
                    [void]$script:Removed.Add('R: ドライブ（RAM ディスク）')
                } catch {
                    Write-Log -Level 'WARN' -Message ('  R: ドライブの削除に失敗しました: {0}' -f $_.Exception.Message) -Color Yellow
                    Write-Log -Level 'WARN' -Message '  （PC を再起動すれば RAM ディスクは消えます）' -Color Yellow
                }
            }
        }
    }
} else {
    Write-Log -Level 'SKIP' -Message '  タスクは残します。' -Color Yellow
}

# ---------------------------------------------------------------------------
# 3. インストーラが作った作業フォルダー
# ---------------------------------------------------------------------------

Write-Host ''
Write-Log -Level 'STEP' -Message '  [3/4] インストーラが作った作業フォルダーを削除する' -Color DarkCyan
$workTargets = @(
    @{ Path = (Join-Path $script:SetupRoot 'cuda'); Label = 'tools\ppt-setup\cuda（CUDA の DLL 展開先）' },
    @{ Path = (Join-Path $script:SetupRoot 'python'); Label = 'tools\ppt-setup\python（同梱 Python）' },
    @{ Path = (Join-Path $script:SetupRoot 'downloads'); Label = 'tools\ppt-setup\downloads（ダウンロードしたファイル）' },
    @{ Path = (Join-Path $script:SetupRoot 'backup'); Label = 'tools\ppt-setup\backup（差し替え前のバックアップ）' },
    @{ Path = (Join-Path $script:SetupRoot 'licenses'); Label = 'tools\ppt-setup\licenses（ライセンス文の控え）' },
    @{ Path = (Join-Path $script:SetupRoot 'state.json'); Label = 'tools\ppt-setup\state.json（インストール記録）'; File = $true }
)
$workExists = @($workTargets | Where-Object { Test-Path -LiteralPath $_.Path }).Count
if ($workExists -eq 0) {
    Write-Log -Level 'SKIP' -Message ('  {0} 削除する作業フォルダーはありません' -f $script:MarkSkip) -Color DarkGray
} elseif (Confirm-YesNo -Question ('インストーラが作った作業フォルダー {0} 個を削除しますか？（合計 {1}）' -f $workExists, (Get-DirectorySizeText $script:SetupRoot)) -Default $true) {
    foreach ($target in $workTargets) {
        if ($target.ContainsKey('File') -and $target.File) {
            Remove-PathChecked -Path $target.Path -Label $target.Label
        } else {
            Remove-PathChecked -Path $target.Path -Label $target.Label -Directory
        }
    }
} else {
    Write-Log -Level 'SKIP' -Message '  作業フォルダーは残します。' -Color Yellow
}

# ---------------------------------------------------------------------------
# 4. データ（既定では消さない）
# ---------------------------------------------------------------------------

Write-Host ''
Write-Log -Level 'STEP' -Message '  [4/4] アプリのデータを削除する（任意）' -Color Cyan
if ($script:KeepData) {
    Write-Log -Level 'SKIP' -Message '  -KeepData が指定されているので、データは残します。' -Color Yellow
} else {
    Write-Log -Level 'INFO' -Message ('  データ: {0}（{1}）' -f $script:DataDir, (Get-DirectorySizeText $script:DataDir)) -Color Gray
    Write-Log -Level 'INFO' -Message ('  解析結果: {0}（{1}）' -f $script:BatchesDir, (Get-DirectorySizeText $script:BatchesDir)) -Color Gray
    Write-Log -Level 'INFO' -Message '  ここを消すと、解析した動画の一覧・DB・キャッシュは元に戻せません。' -Color Yellow

    if (Confirm-YesNo -Question 'データも削除しますか？（次の手順で DELETE の入力が必要です）' -Default $false) {
        if (Confirm-DeleteData) {
            Remove-PathChecked -Path $script:DataDir -Label ('データ（{0}）' -f $script:DataDir) -Directory
            Remove-PathChecked -Path $script:BatchesDir -Label ('解析結果（{0}）' -f $script:BatchesDir) -Directory
        } else {
            Write-Log -Level 'SKIP' -Message '  データは残しました。' -Color Yellow
        }
    } else {
        Write-Log -Level 'SKIP' -Message '  データは残しました（既定どおり）。' -Color Yellow
    }
}

# ---------------------------------------------------------------------------
# まとめ
# ---------------------------------------------------------------------------

Write-Host ''
Write-Log -Level 'SUM' -Message $rule -Color DarkCyan
Write-Log -Level 'SUM' -Message '  結果' -Color Cyan
Write-Log -Level 'SUM' -Message $rule -Color DarkCyan

if ($script:Removed.Count -gt 0) {
    Write-Log -Level 'SUM' -Message ('  削除したもの（{0} 件）' -f $script:Removed.Count) -Color Green
    $index = 0
    foreach ($item in $script:Removed) {
        $index++
        Write-Log -Level 'SUM' -Message ('   {0}. {1}' -f $index, $item) -Color Gray
    }
} else {
    Write-Log -Level 'SUM' -Message '  削除したものはありません。' -Color Yellow
}

if ($script:Skipped.Count -gt 0) {
    Write-Log -Level 'SUM' -Message ('  残したもの（{0} 件）' -f $script:Skipped.Count) -Color DarkGray
    foreach ($item in $script:Skipped) {
        Write-Log -Level 'SUM' -Message ('   - {0}' -f $item) -Color DarkGray
    }
}

Write-Host ''
Write-Log -Level 'SUM' -Message '  アプリ本体（server.py / 解析エンジン / モデル / 設定）は残っています。' -Color Gray
Write-Log -Level 'SUM' -Message '  完全に消す場合は、このリポジトリのフォルダーを削除してください。' -Color Gray
Write-Log -Level 'SUM' -Message ('  ログ: {0}' -f $script:LogPath) -Color DarkGray
Write-Log -Level 'SUM' -Message $rule -Color DarkCyan
Write-Host ''

exit 0
