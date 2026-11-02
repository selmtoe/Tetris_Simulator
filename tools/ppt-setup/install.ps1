#Requires -Version 5.1
<#
.SYNOPSIS
    PPT Lab（ぷよテト動画解析ラボ）1 クリックインストーラ。

.DESCRIPTION
    配布用フォルダーを展開した人が「install.cmd をダブルクリック」するだけで、
    PPT Lab を起動できる状態まで持っていくためのスクリプトです。

    次の順に「あるか確認 → 無ければ導入」を繰り返します。

      1. Windows / 64bit / 管理者権限の確認
      2. Python 3.11 以上の検出（無ければ公式インストーラ or embeddable zip）
      3. GPU 検出（nvidia-smi）。無ければ CPU モードの遅さを警告して確認
      4. CUDA ランタイムと cuDNN の補完（pip の NVIDIA 配布 wheel から DLL をコピー）
      5. ffmpeg を LGPL 共有ビルドへ差し替え（任意・既定は推奨）
      6. 足りないファイルの最終チェック（✅ / ❌ の一覧）
      7. ショートカット作成（デスクトップ / スタートメニュー）＋ launch.cmd 生成
      8. 任意: RAM ディスクのタスク登録（既定は「しない」）

    何度実行しても壊れません（冪等）。既にあるものは「既にあります」と表示して飛ばします。
    実行内容は %LOCALAPPDATA%\PPTLab\logs\install-YYYYMMDD-HHMMSS.log に残ります。

.PARAMETER DryRun
    何もダウンロード・インストール・コピーせず、検出結果と「実行する予定の内容」だけを表示します。
    この PC のように本番環境で、まず安全性を確認したいときに使います。

.PARAMETER WhatIf
    -DryRun と同じ意味です（PowerShell の慣習に合わせた別名）。

.PARAMETER Yes
    すべての確認に「はい」と答えたものとして進めます（無人実行向け）。
    データ削除などの破壊的操作は対象外です（uninstall.ps1 側で別途確認します）。

.PARAMETER NonInteractive
    確認プロンプトを出さず、既定の答えで進めます（-Yes より弱く、既定値に従います）。

.PARAMETER Diagnose
    診断モード。何も変更せず、環境と不足ファイルだけを日本語で報告します。
    launch.cmd が起動に失敗したときにも自動で呼ばれます。

.PARAMETER SkipFfmpeg
    ffmpeg の LGPL ビルドへの差し替えを行いません（既存の GPL ビルドをそのまま使う）。

.PARAMETER NoShortcuts
    デスクトップ / スタートメニューのショートカットを作りません。

.PARAMETER RamDisk
    RAM ディスク（スケジュールタスク "PPT RAM disk"）の登録まで行います。
    既定では行いません。管理者権限と ImDisk が必要です。

.PARAMETER NoRamDisk
    RAM ディスクの確認自体を出しません。

.PARAMETER Port
    PPT Lab の HTTP ポート。既定は 8765。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -DryRun

    何も変更せず、いま何が足りないかだけを確認します。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1

    通常のインストール。確認に答えながら進みます。

.NOTES
    対象: Windows 10 1809 以降 / Windows 11 (64bit)
    文字コード: UTF-8 (BOM) — Windows PowerShell 5.1 でも日本語が化けないようにするため。
#>
[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$WhatIf,
    [switch]$Yes,
    [switch]$NonInteractive,
    [switch]$Diagnose,
    [switch]$SkipFfmpeg,
    [switch]$NoShortcuts,
    [switch]$RamDisk,
    [switch]$NoRamDisk,
    [int]$Port = 8765,
    [string]$FfmpegZipUrl = ''
)

# ============================================================================
# 基本設定（パスはすべてこのスクリプトの位置から解決する）
# ============================================================================

$ErrorActionPreference = 'Stop'

$script:InstallerVersion = '1.0.1'
$script:SetupRoot = $PSScriptRoot                                  # tools\ppt-setup
$script:ToolRoot = Split-Path -Parent $script:SetupRoot             # tools
$script:RepoRoot = Split-Path -Parent $script:ToolRoot              # リポジトリ直下
$script:RuntimeRoot = Join-Path $script:ToolRoot 'video-recovery-runtime'
$script:BinDir = Join-Path $script:RuntimeRoot 'bin'
$script:ConfigDir = Join-Path $script:RuntimeRoot 'config'
$script:PptLabRoot = Join-Path $script:ToolRoot 'ppt-lab'
$script:ServerPy = Join-Path $script:PptLabRoot 'server.py'
$script:ModelFast = Join-Path (Join-Path $script:PptLabRoot 'models') 'fast320'
$script:ModelEnd3 = Join-Path (Join-Path $script:PptLabRoot 'models') 'end3'
$script:DownloadDir = Join-Path $script:SetupRoot 'downloads'
$script:CudaDir = Join-Path $script:SetupRoot 'cuda'
$script:PythonDir = Join-Path $script:SetupRoot 'python'
$script:BackupDir = Join-Path $script:SetupRoot 'backup'
$script:LicenseDir = Join-Path $script:SetupRoot 'licenses'
$script:StatePath = Join-Path $script:SetupRoot 'state.json'
$script:LaunchCmd = Join-Path $script:SetupRoot 'launch.cmd'
$script:AppDataDir = Join-Path $env:LOCALAPPDATA 'PPTLab'
$script:LogDir = Join-Path $script:AppDataDir 'logs'
$script:LogPath = $null
$script:LogReady = $false

# ダウンロード元（バージョンは「既存 DLL / 既存 exe に合わせる」ため定数で持つ）
# Python 3.11.9 は 3.11 系で Windows 用バイナリが提供される最後のリリース。
$script:PythonVersion = '3.11.9'
$script:PythonInstallerUrl = 'https://www.python.org/ftp/python/3.11.9/python-3.11.9-amd64.exe'
$script:PythonEmbedUrl = 'https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip'
$script:GetPipUrl = 'https://bootstrap.pypa.io/get-pip.py'
# BtbN の FFmpeg 9.0 系 LGPL 共有ビルド。
# 「shared」でないと avcodec-63.dll などが入らず、既存の C++ 実行ファイルが動かない。
$script:FfmpegUrlDefault = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip'

$script:VcRedistUrl = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'
$script:CudaDownloadUrl = 'https://developer.nvidia.com/cuda-downloads'
$script:CudnnDownloadUrl = 'https://developer.nvidia.com/cudnn'
$script:NvidiaDriverUrl = 'https://www.nvidia.com/Download/index.aspx'
$script:ImDiskUrl = 'https://sourceforge.net/projects/imdisk-toolkit/'
$script:FfmpegBuildsUrl = 'https://github.com/BtbN/FFmpeg-Builds/releases/latest'
# この配布物で動作確認済みの cuDNN（手元に cuDNN が無い PC ではこの版に固定する）
$script:CudnnPinVersion = '9.12.0.46'

if ([string]::IsNullOrWhiteSpace($FfmpegZipUrl)) { $FfmpegZipUrl = $script:FfmpegUrlDefault }

# モード判定
$script:DryRun = [bool]($DryRun -or $WhatIf -or $Diagnose)
$script:Diagnose = [bool]$Diagnose
$script:Yes = [bool]$Yes
$script:NonInteractive = [bool]($NonInteractive -or $Diagnose)
$script:SkipFfmpeg = [bool]$SkipFfmpeg
$script:NoShortcuts = [bool]$NoShortcuts
$script:WantRamDisk = [bool]$RamDisk
$script:NoRamDisk = [bool]$NoRamDisk
$script:Port = $Port
$script:FfmpegZipUrl = $FfmpegZipUrl

$script:IsAdmin = $false
$script:Checks = New-Object System.Collections.ArrayList
$script:Actions = New-Object System.Collections.ArrayList   # 実行した（する予定の）変更点
$script:Warnings = New-Object System.Collections.ArrayList
$script:Python = $null                                       # 検出した Python の情報
$script:Gpu = $null                                          # GPU の情報
$script:Cuda = $null                                         # 必要 DLL / 導入結果

# ============================================================================
# コンソール記号（日本語 Windows の CP932 コンソールでも崩れないようにする）
# ============================================================================

$script:MarkOk = '✅'
$script:MarkNg = '❌'
try {
    if ([Console]::OutputEncoding.CodePage -ne 65001) {
        # コンソールが UTF-8 でない場合、絵文字は「?」になるので ASCII に落とす
        $script:MarkOk = '[OK]'
        $script:MarkNg = '[NG]'
    }
} catch {
    $script:MarkOk = '[OK]'
    $script:MarkNg = '[NG]'
}
if ($env:PPTLAB_ASCII -eq '1') {
    $script:MarkOk = '[OK]'
    $script:MarkNg = '[NG]'
}

# ============================================================================
# ログと表示
# ============================================================================

function Start-InstallLog {
    <#
      ログを %LOCALAPPDATA%\PPTLab\logs\install-YYYYMMDD-HHMMSS.log に作る。
      ログは常に UTF-8 (BOM) なので、メモ帳でも日本語と ✅/❌ が読める。
    #>
    try {
        if (-not (Test-Path -LiteralPath $script:LogDir)) {
            New-Item -ItemType Directory -Force -Path $script:LogDir | Out-Null
        }
        $name = 'install-{0}.log' -f (Get-Date).ToString('yyyyMMdd-HHmmss')
        $script:LogPath = Join-Path $script:LogDir $name
        $utf8Bom = New-Object System.Text.UTF8Encoding($true)
        [System.IO.File]::WriteAllText($script:LogPath, '', $utf8Bom)
        $script:LogReady = $true
    } catch {
        $script:LogReady = $false
        Write-Host ('ログファイルを作成できませんでした: {0}' -f $_.Exception.Message) -ForegroundColor Yellow
    }
}

function Write-LogLine {
    param(
        [string]$Level,
        [string]$Message,
        [string]$Color = 'Gray'
    )
    if ($null -ne $Message) {
        if ($Color -eq 'None') { Write-Host $Message } else { Write-Host $Message -ForegroundColor $Color }
    }
    if ($script:LogReady) {
        $stamp = (Get-Date).ToString('HH:mm:ss')
        $clean = ($Message -replace "`r", '')
        $line = '[{0}] [{1}] {2}' -f $stamp, $Level, $clean
        try { Add-Content -LiteralPath $script:LogPath -Value $line -Encoding UTF8 } catch { }
    }
}

function Write-Step {
    param([int]$Index, [int]$Total, [string]$Title)
    $rule = '=' * 70
    Write-Host ''
    Write-LogLine -Level 'STEP' -Message $rule -Color DarkCyan
    Write-LogLine -Level 'STEP' -Message (' [{0}/{1}] {2}' -f $Index, $Total, $Title) -Color Cyan
    Write-LogLine -Level 'STEP' -Message $rule -Color DarkCyan
}

function Write-Ok {
    param([string]$Message)
    Write-LogLine -Level 'OK' -Message ('  {0} {1}' -f $script:MarkOk, $Message) -Color Green
}

function Write-Warn {
    param([string]$Message)
    Write-LogLine -Level 'WARN' -Message ('  ! {0}' -f $Message) -Color Yellow
    [void]$script:Warnings.Add($Message)
}

function Write-Fail {
    param([string]$Message)
    Write-LogLine -Level 'NG' -Message ('  {0} {1}' -f $script:MarkNg, $Message) -Color Red
}

function Write-Info {
    param([string]$Message)
    Write-LogLine -Level 'INFO' -Message ('  - {0}' -f $Message) -Color Gray
}

function Write-Detail {
    param([string]$Message, [string]$Color = 'DarkGray')
    Write-LogLine -Level 'DETAIL' -Message ('      {0}' -f $Message) -Color $Color
}

function Write-Dry {
    param([string]$Message)
    Write-LogLine -Level 'DRYRUN' -Message ('  [DryRun] {0}' -f $Message) -Color Magenta
}

function Add-Action {
    param([string]$Message)
    [void]$script:Actions.Add($Message)
    if ($script:DryRun) { Write-Dry $Message } else { Write-Detail ('実行: {0}' -f $Message) }
}

# ============================================================================
# 汎用ヘルパー
# ============================================================================

function New-Directory {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) { return $true }
    if ($script:DryRun) {
        Write-Dry ('フォルダー作成予定: {0}' -f $Path)
        return $false
    }
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    Write-Detail ('フォルダーを作成: {0}' -f $Path)
    return $true
}

function Confirm-YesNo {
    <#
      はい / いいえ を聞く。DryRun と -Yes と -NonInteractive では聞かずに既定値を返す。
      既定値は大文字で表示する（[Y/n] / [y/N]）。
    #>
    param(
        [string]$Question,
        [bool]$Default = $true,
        [string]$Suffix = ''
    )
    $defaultText = 'はい'
    if (-not $Default) { $defaultText = 'いいえ' }
    # -Yes を最優先（-DryRun と併用したときに「予定の全量」を確認したいため）
    if ($script:Yes) {
        Write-Info ('確認: {0} → 「はい」(-Yes)' -f $Question)
        return $true
    }
    if ($script:DryRun) {
        Write-Dry ('確認: {0} → 既定「{1}」' -f $Question, $defaultText)
        return $Default
    }
    if ($script:NonInteractive) {
        Write-Info ('確認: {0} → 既定「{1}」(-NonInteractive)' -f $Question, $defaultText)
        return $Default
    }
    $hint = '[y/N]'
    if ($Default) { $hint = '[Y/n]' }
    $prompt = '  ? {0} {1}' -f $Question, $hint
    while ($true) {
        $answer = $null
        try { $answer = Read-Host $prompt } catch { return $Default }
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

function Invoke-Download {
    <#
      大きいファイルを落とすので、進捗表示を止めて無言で待つ。
      失敗したら 3 回まで再試行し、それでも駄目なら理由と URL を日本語で出す。
    #>
    param(
        [string]$Url,
        [string]$Destination,
        [string]$Label,
        [long]$MinBytes = 1048576,
        [int]$Attempts = 3
    )
    if ($script:DryRun) {
        Write-Dry ('ダウンロード予定: {0}' -f $Label)
        Write-Detail $Url
        return $false
    }
    if (-not (Test-Path -LiteralPath $script:DownloadDir)) {
        New-Item -ItemType Directory -Force -Path $script:DownloadDir | Out-Null
    }
    if ((Test-Path -LiteralPath $Destination) -and ((Get-Item -LiteralPath $Destination).Length -ge $MinBytes)) {
        Write-Ok ('ダウンロード済み（再利用）: {0}' -f (Split-Path -Leaf $Destination))
        return $true
    }

    $partial = $Destination + '.part'
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        try {
            if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue }
            [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12
            $savedProgress = $ProgressPreference
            $ProgressPreference = 'SilentlyContinue'
            try {
                Invoke-WebRequest -Uri $Url -OutFile $partial -UseBasicParsing -TimeoutSec 3600 `
                    -Headers @{ 'User-Agent' = ('PPTLab-Installer/' + $script:InstallerVersion) }
            } finally {
                $ProgressPreference = $savedProgress
            }
            $size = (Get-Item -LiteralPath $partial).Length
            if ($size -lt $MinBytes) {
                throw ('ダウンロードしたファイルが小さすぎます（{0} バイト）' -f $size)
            }
            Move-Item -LiteralPath $partial -Destination $Destination -Force
            Write-Ok ('ダウンロード完了: {0} ({1:N1} MB)' -f (Split-Path -Leaf $Destination), ($size / 1MB))
            return $true
        } catch {
            Write-Warn ('ダウンロード失敗（{0}/{1} 回目）: {2}' -f $attempt, $Attempts, $_.Exception.Message)
            if ($attempt -lt $Attempts) { Start-Sleep -Seconds ([math]::Min(5 * $attempt, 20)) }
        }
    }
    Write-Fail ('ダウンロードできませんでした: {0}' -f $Url)
    Write-Detail '次のどれかを確認してください:'
    Write-Detail '  1) インターネットに接続されているか'
    Write-Detail '  2) 社内プロキシやセキュリティソフトがブロックしていないか'
    Write-Detail '  3) ブラウザーで同じ URL を開いて手動でダウンロードできるか'
    return $false
}

function Expand-ZipArchive {
    <#
      ZIP を安全に展開する（..\ などで外へ書き出そうとするエントリは拒否）。
      既存ファイルは上書きするので、再実行しても同じ結果になる。
    #>
    param(
        [string]$ZipPath,
        [string]$Destination
    )
    if (-not (Test-Path -LiteralPath $ZipPath)) { throw ('ZIP が見つかりません: {0}' -f $ZipPath) }
    if ($script:DryRun) {
        Write-Dry ('展開予定: {0} → {1}' -f (Split-Path -Leaf $ZipPath), $Destination)
        return $false
    }
    $destFull = [System.IO.Path]::GetFullPath($Destination)
    if (-not (Test-Path -LiteralPath $destFull)) {
        New-Item -ItemType Directory -Force -Path $destFull | Out-Null
    }
    try { Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop } catch { }
    $archive = [System.IO.Compression.ZipFile]::OpenRead([System.IO.Path]::GetFullPath($ZipPath))
    try {
        foreach ($entry in $archive.Entries) {
            if ([string]::IsNullOrEmpty($entry.Name)) { continue }
            $target = [System.IO.Path]::GetFullPath((Join-Path $destFull $entry.FullName))
            if (-not $target.StartsWith($destFull, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw ('ZIP に危険なパスが含まれています: {0}' -f $entry.FullName)
            }
            $parent = Split-Path -Parent $target
            if ($parent -and -not (Test-Path -LiteralPath $parent)) {
                New-Item -ItemType Directory -Force -Path $parent | Out-Null
            }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        }
    } finally {
        $archive.Dispose()
    }
    Write-Ok ('展開しました: {0}' -f $Destination)
    return $true
}

function Get-AsciiMatches {
    <#
      DLL の中に埋まっている ASCII 文字列（＝インポートする DLL 名）を拾う。
      234MB の onnxruntime_providers_cuda.dll でもメモリを食わないよう分割読みする。
    #>
    param(
        [string]$Path,
        [string[]]$Patterns,
        [int]$ChunkSize = 4194304
    )
    $result = New-Object 'System.Collections.Generic.HashSet[string]'
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $regexes = @()
    foreach ($pattern in $Patterns) {
        $regexes += New-Object System.Text.RegularExpressions.Regex($pattern, 'IgnoreCase')
    }
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $buffer = New-Object byte[] $ChunkSize
        $carry = ''
        while (($read = $stream.Read($buffer, 0, $ChunkSize)) -gt 0) {
            $text = [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
            $window = $carry + $text
            foreach ($regex in $regexes) {
                foreach ($match in $regex.Matches($window)) { [void]$result.Add($match.Value) }
            }
            if ($window.Length -gt 128) { $carry = $window.Substring($window.Length - 128) } else { $carry = $window }
        }
    } finally {
        $stream.Dispose()
    }
    return @($result | Sort-Object)
}

function Get-FileVersionText {
    param([string]$Path)
    try {
        if (-not (Test-Path -LiteralPath $Path)) { return '' }
        return [string](Get-Item -LiteralPath $Path).VersionInfo.FileVersion
    } catch {
        return ''
    }
}

function ConvertTo-VersionSafe {
    <#
      "9.12.0.46" のような文字列を [version] に。読めなければ $null。
      "6,14,11,1360" のようにカンマ区切りのリソースもあるので両対応。
    #>
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $clean = ($Text -replace ',', '.').Trim()
    $match = [regex]::Match($clean, '\d+(\.\d+){1,3}')
    if (-not $match.Success) { return $null }
    try { return [version]$match.Value } catch { return $null }
}

function Test-Exe {
    param([string]$Path)
    return (Test-Path -LiteralPath $Path -PathType Leaf)
}

function Add-Check {
    <#
      最終チェックの 1 行を記録する。
      Required = $false のものは「あると良い」扱いで、❌ でも致命的にしない。
    #>
    param(
        [string]$Group,
        [string]$Name,
        [bool]$Ok,
        [string]$Detail = '',
        [bool]$Required = $true,
        [string]$Hint = '',
        [string]$Url = ''
    )
    $entry = [pscustomobject]@{
        Group    = $Group
        Name     = $Name
        Ok       = $Ok
        Detail   = $Detail
        Required = $Required
        Hint     = $Hint
        Url      = $Url
    }
    # 戻り値は返さない（呼び出し側の戻り値と混ざらないようにするため）
    [void]$script:Checks.Add($entry)
}

function Get-SizeText {
    param([string]$Path)
    try {
        $length = (Get-Item -LiteralPath $Path).Length
        if ($length -ge 1GB) { return ('{0:N2} GB' -f ($length / 1GB)) }
        if ($length -ge 1MB) { return ('{0:N1} MB' -f ($length / 1MB)) }
        return ('{0:N0} KB' -f ($length / 1KB))
    } catch {
        return ''
    }
}

function Get-FileDetailText {
    <#
      一覧に出す補足。バージョン情報があればそれを、無ければファイルサイズを返す。
      （cublas64_*.dll などは FileVersion が空のことが多い）
    #>
    param([string]$Path)
    $version = Get-FileVersionText $Path
    if (-not [string]::IsNullOrWhiteSpace($version)) { return $version }
    return (Get-SizeText $Path)
}

function Read-Choice {
    <#
      番号で選ばせる質問。-DryRun / -Yes / -NonInteractive では既定値を返す。
    #>
    param(
        [string]$Question,
        [string[]]$Options,
        [int]$Default = 1
    )
    if ($script:DryRun) {
        Write-Dry ('選択: {0} → 既定 {1}「{2}」' -f $Question, $Default, $Options[$Default - 1])
        return $Default
    }
    if ($script:Yes -or $script:NonInteractive) {
        Write-Info ('選択: {0} → 既定 {1}「{2}」' -f $Question, $Default, $Options[$Default - 1])
        return $Default
    }
    Write-Host ''
    Write-Host ('  ? {0}' -f $Question) -ForegroundColor White
    for ($i = 0; $i -lt $Options.Count; $i++) {
        $suffix = ''
        if (($i + 1) -eq $Default) { $suffix = '  ← 既定（Enter）' }
        Write-Host ('     {0}) {1}{2}' -f ($i + 1), $Options[$i], $suffix)
    }
    while ($true) {
        $answer = $null
        try { $answer = Read-Host ('    番号を入力 [1-{0}]' -f $Options.Count) } catch { return $Default }
        if ($null -eq $answer -or $answer.Trim().Length -eq 0) { return $Default }
        $number = 0
        if ([int]::TryParse($answer.Trim(), [ref]$number)) {
            if ($number -ge 1 -and $number -le $Options.Count) { return $number }
        }
        Write-Host ('     1 から {0} の番号で答えてください。' -f $Options.Count) -ForegroundColor Yellow
    }
}

# ============================================================================
# [1/8] Windows / 64bit / 管理者権限
# ============================================================================

function Step-Environment {
    param([int]$Index, [int]$Total)

    Write-Step -Index $Index -Total $Total -Title 'Windows と実行環境を確認する'
    $ok = $true

    # --- OS の種類とバージョン ---
    $caption = ''
    $versionText = ''
    try {
        $os = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop
        $caption = [string]$os.Caption
        $versionText = [string]$os.Version
        $build = [string]$os.BuildNumber
    } catch {
        $caption = 'Windows'
        $versionText = [Environment]::OSVersion.Version.ToString()
        $build = ''
    }
    $caption = $caption.Trim()
    if ([string]::IsNullOrWhiteSpace($caption)) { $caption = 'Windows' }
    Write-Ok ('OS: {0} ({1},{2})' -f $caption, $versionText, $build)

    $osVersion = ConvertTo-VersionSafe $versionText
    if ($null -ne $osVersion -and $osVersion.Major -lt 10) {
        Write-Fail 'Windows 10 以降が必要です（Windows 8.1 以前は非対応）。'
        Add-Check -Group '環境' -Name 'Windows 10 以降' -Ok $false -Detail $versionText -Hint 'Windows 11 の PC で実行してください' -Url 'https://www.microsoft.com/ja-jp/windows/windows-11'
        $ok = $false
    } else {
        Add-Check -Group '環境' -Name 'Windows 10 以降' -Ok $true -Detail ($caption + ' ' + $versionText)
    }

    # --- 64bit ---
    $is64 = [Environment]::Is64BitOperatingSystem
    if ($is64) {
        Write-Ok 'アーキテクチャ: 64bit'
        Add-Check -Group '環境' -Name '64bit Windows' -Ok $true -Detail 'x64'
    } else {
        Write-Fail '64bit Windows が必要です（32bit では解析エンジンが動きません）。'
        Add-Check -Group '環境' -Name '64bit Windows' -Ok $false -Detail '32bit' -Hint '64bit 版 Windows の PC で実行してください'
        $ok = $false
    }

    # --- 管理者権限（無くても動く範囲で続ける） ---
    try {
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        $principal = New-Object Security.Principal.WindowsPrincipal($identity)
        $script:IsAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch {
        $script:IsAdmin = $false
    }
    if ($script:IsAdmin) {
        Write-Ok '管理者権限: あり（RAM ディスクのタスク登録まで可能）'
        Add-Check -Group '環境' -Name '管理者権限' -Ok $true -Detail 'あり' -Required $false
    } else {
        Write-Warn '管理者権限: なし。インストール自体は続行できますが、次の 2 つは後回しになります。'
        Write-Detail '・RAM ディスクのスケジュールタスク登録（手動で管理者 PowerShell から実行できます）'
        Write-Detail '・Program Files 配下への書き込み'
        Add-Check -Group '環境' -Name '管理者権限' -Ok $false -Detail 'なし（RAM ディスク登録のみ不可）' -Required $false `
            -Hint 'RAM ディスクを使う場合だけ「管理者として実行」で install.cmd を再実行してください'
    }

    # --- PowerShell のバージョン ---
    $psVersion = $PSVersionTable.PSVersion
    Write-Ok ('PowerShell: {0}' -f $psVersion.ToString())
    if ($psVersion.Major -lt 5) {
        Write-Fail 'PowerShell 5.1 以降が必要です。'
        $ok = $false
    }
    if ($psVersion.Major -eq 5 -and $psVersion.Minor -ge 1) {
        Write-Detail 'Windows PowerShell 5.1 を検出（このスクリプトは 5.1 / 7 の両方で動きます）'
    }

    # --- リポジトリの場所と書き込み権限 ---
    Write-Info ('インストール先: {0}' -f $script:RepoRoot)
    if ($script:RepoRoot.Length -ge 130) {
        Write-Warn ('インストール先のパスが長めです（{0} 文字）。解析結果の保存でパス長エラーになることがあります。' -f $script:RepoRoot.Length)
        Write-Detail '短い場所（例: C:\PPTLab）へ移動してから実行することをおすすめします。'
        Add-Check -Group '環境' -Name 'パスの長さ' -Ok $false -Detail ('{0} 文字' -f $script:RepoRoot.Length) -Required $false `
            -Hint 'C:\PPTLab のような短い場所に置くと安全です'
    } else {
        Add-Check -Group '環境' -Name 'パスの長さ' -Ok $true -Detail ('{0} 文字' -f $script:RepoRoot.Length) -Required $false
    }

    if (-not $script:DryRun) {
        $probe = Join-Path $script:SetupRoot ('.write-test-{0}.tmp' -f $PID)
        try {
            Set-Content -LiteralPath $probe -Value 'ok' -Encoding ASCII -ErrorAction Stop
            Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
            Write-Ok '書き込み権限: あり'
            Add-Check -Group '環境' -Name 'フォルダーへの書き込み' -Ok $true -Detail '可能'
        } catch {
            Write-Fail ('書き込み権限がありません: {0}' -f $script:SetupRoot)
            Write-Detail '「書き込み可能な場所にコピー」するか、管理者として実行してください。'
            Add-Check -Group '環境' -Name 'フォルダーへの書き込み' -Ok $false -Detail '不可' `
                -Hint 'リポジトリを書き込み可能な場所（例: C:\PPTLab）へコピーしてください'
            $ok = $false
        }
    } else {
        Add-Check -Group '環境' -Name 'フォルダーへの書き込み' -Ok $true -Detail 'DryRun のため未検査'
    }

    # --- 空き容量（CUDA/cuDNN の wheel を展開すると 2GB 前後使う） ---
    try {
        $drive = New-Object System.IO.DriveInfo([System.IO.Path]::GetPathRoot($script:RepoRoot))
        $freeGB = [math]::Round($drive.AvailableFreeSpace / 1GB, 1)
        if ($freeGB -lt 8) {
            Write-Warn ('空き容量が少なめです: {0} GB（目安 8 GB 以上）' -f $freeGB)
            Add-Check -Group '環境' -Name '空き容量' -Ok $false -Detail ('{0} GB' -f $freeGB) -Required $false `
                -Hint '不要なファイルを消すか、別のドライブに置いてください'
        } else {
            Write-Ok ('空き容量: {0} GB' -f $freeGB)
            Add-Check -Group '環境' -Name '空き容量' -Ok $true -Detail ('{0} GB' -f $freeGB) -Required $false
        }
    } catch {
        Write-Detail '空き容量を取得できませんでした（気にしなくて大丈夫です）'
    }

    return $ok
}

# ============================================================================
# [2/8] Python 3.11 以上の検出と、必要なら導入
# ============================================================================

function Measure-PythonExe {
    <#
      指定の python.exe が「3.11 以上」「64bit」かを実際に起動して確かめる。
      戻り値: 使えるなら [pscustomobject]、駄目なら $null。
    #>
    param(
        [string]$Exe,
        [string]$Source = ''
    )
    if ([string]::IsNullOrWhiteSpace($Exe)) { return $null }
    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { return $null }

    # Python 側はシングルクォートで書く（PowerShell 5.1 が引数の二重引用符を落とすため）
    $code = "import sys,platform;print('%d.%d.%d|%s|%s' % (sys.version_info[0],sys.version_info[1],sys.version_info[2],platform.architecture()[0],sys.executable))"
    $raw = $null
    try {
        $raw = & $Exe -c $code 2>&1
        if ($LASTEXITCODE -ne 0) { return $null }
    } catch {
        return $null
    }
    $text = ($raw | Out-String).Trim()
    $parts = $text -split '\|'
    if ($parts.Count -lt 3) { return $null }
    $versionText = $parts[0].Trim()
    $arch = $parts[1].Trim()
    $realExe = $parts[2].Trim()
    $version = ConvertTo-VersionSafe $versionText
    if ($null -eq $version) { return $null }
    if ($version.Major -lt 3) { return $null }
    if ($version.Major -eq 3 -and $version.Minor -lt 11) { return $null }
    if ($arch -ne '64bit') { return $null }
    if ($Exe -match '\\WindowsApps\\') { return $null }   # Microsoft Store のスタブは避ける

    return [pscustomobject]@{
        Path        = $realExe
        Version     = $versionText
        VersionObj  = $version
        Arch        = $arch
        Source      = $Source
        PythonwPath = (Join-Path (Split-Path -Parent $realExe) 'pythonw.exe')
    }
}

function Get-PythonCandidates {
    <#
      怪しい順ではなく「ありそうな順」に python.exe を列挙する。
      py ランチャーは python.exe そのものではないので、実際のパスへ解決してから検査する。
    #>
    $candidates = New-Object System.Collections.ArrayList

    function Add-Candidate([string]$path, [string]$source) {
        if ([string]::IsNullOrWhiteSpace($path)) { return }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return }
        foreach ($existing in $candidates) {
            if ($existing.Path -eq $path) { return }
        }
        [void]$candidates.Add([pscustomobject]@{ Path = $path; Source = $source })
    }

    # 1) このインストーラが展開した embeddable Python
    Add-Candidate -path (Join-Path $script:PythonDir 'python.exe') -source 'このインストーラが用意した Python'

    # 2) py ランチャー（3.11 を優先）
    $pyLauncher = (Get-Command 'py.exe' -ErrorAction SilentlyContinue)
    if ($pyLauncher) {
        foreach ($tag in @('-3.11', '-3.12', '-3.13', '-3')) {
            try {
                $resolved = & $pyLauncher.Source $tag -c 'import sys;print(sys.executable)' 2>$null
                if ($LASTEXITCODE -eq 0 -and $resolved) {
                    $resolvedPath = ([string]($resolved | Select-Object -First 1)).Trim()
                    Add-Candidate -path $resolvedPath -source ('py ' + $tag)
                }
            } catch { }
        }
    }

    # 3) 既知のインストール先
    $knownRoots = @()
    if ($env:LOCALAPPDATA) { $knownRoots += (Join-Path $env:LOCALAPPDATA 'Programs\Python') }
    if ($env:ProgramFiles) { $knownRoots += $env:ProgramFiles }
    if (${env:ProgramFiles(x86)}) { $knownRoots += ${env:ProgramFiles(x86)} }
    foreach ($root in $knownRoots) {
        foreach ($minor in @(11, 12, 13, 14)) {
            Add-Candidate -path (Join-Path $root ('Python3{0}\python.exe' -f $minor)) -source '既知のインストール先'
        }
        Add-Candidate -path (Join-Path $root 'Python311\python.exe') -source '既知のインストール先'
    }
    foreach ($minor in @(11, 12, 13, 14)) {
        Add-Candidate -path ('C:\Python3{0}\python.exe' -f $minor) -source '既知のインストール先'
    }

    # 4) PATH 上の python
    $onPath = Get-Command 'python.exe' -ErrorAction SilentlyContinue
    if ($onPath -and $onPath.Source) { Add-Candidate -path $onPath.Source -source 'PATH' }

    return $candidates
}

function Find-PythonInstallation {
    <#
      使える Python を 1 つ選ぶ。3.11 を最優先、次に 3.12 / 3.13、その後に 3.11 以上のもの。
    #>
    $candidates = Get-PythonCandidates
    $usable = New-Object System.Collections.ArrayList
    foreach ($candidate in $candidates) {
        $measured = Measure-PythonExe -Exe $candidate.Path -Source $candidate.Source
        if ($null -ne $measured) { [void]$usable.Add($measured) }
    }
    if ($usable.Count -eq 0) { return $null }

    $sorted = $usable | Sort-Object -Property `
        @{ Expression = { if ($_.VersionObj.Minor -eq 11 -and $_.VersionObj.Major -eq 3) { 0 } elseif ($_.VersionObj.Minor -in @(12, 13)) { 1 } else { 2 } }; Ascending = $true }, `
        @{ Expression = { $_.VersionObj }; Descending = $true }
    return ($sorted | Select-Object -First 1)
}

function Install-PythonByOfficialInstaller {
    <#
      python.org の公式インストーラを「サイレント導入」する。
      InstallAllUsers=0 なので管理者権限は不要（ユーザー領域に入る）。
    #>
    Write-Info ('Python {0} の公式インストーラを使います（ユーザー領域・管理者不要）' -f $script:PythonVersion)
    $installer = Join-Path $script:DownloadDir ('python-{0}-amd64.exe' -f $script:PythonVersion)
    $downloaded = Invoke-Download -Url $script:PythonInstallerUrl -Destination $installer `
        -Label ('Python {0} インストーラ' -f $script:PythonVersion) -MinBytes 20MB
    if (-not $downloaded) { return $null }

    $arguments = @(
        '/quiet',
        'InstallAllUsers=0',
        'PrependPath=1',
        'Include_launcher=1',
        'Include_pip=1',
        'Include_test=0',
        'Include_doc=0',
        'Include_tcltk=0',
        'Shortcuts=0',
        'AssociateFiles=0',
        'SimpleInstall=1'
    )
    Write-Info 'インストール中です（数分かかります。画面はこのままお待ちください）…'
    $exitCode = -1
    try {
        $process = Start-Process -FilePath $installer -ArgumentList $arguments -PassThru
        if (-not $process.WaitForExit(900000)) {
            Write-Warn 'インストーラが 15 分以内に終わりませんでした。完了を待ってから再実行してください。'
        }
        try { $exitCode = $process.ExitCode } catch { $exitCode = -1 }
    } catch {
        Write-Fail ('サイレントインストールに失敗しました: {0}' -f $_.Exception.Message)
        return $null
    }
    if ($exitCode -ne 0) {
        Write-Warn ('Python インストーラの終了コード: {0}（0 以外でも入っていることがあります）' -f $exitCode)
    }

    # py ランチャーの PATH 反映には新しいプロセスが必要なので、ファイルがあるかで判定する
    $found = Find-PythonInstallation
    if ($null -ne $found) { return $found }

    # それでも見つからない場合は既知のパスを直接確認
    $guessed = Join-Path $env:LOCALAPPDATA ('Programs\Python\Python{0}{1}\python.exe' -f $script:PythonVersion.Split('.')[0], $script:PythonVersion.Split('.')[1])
    $measured = Measure-PythonExe -Exe $guessed -Source '公式インストーラ'
    return $measured
}

function Install-PythonEmbeddable {
    <#
      embeddable zip（持ち運び用の最小 Python）を tools\ppt-setup\python に展開する。
      そのままだと pip も site-packages も無効なので、_pth を直して get-pip.py を通す。
    #>
    Write-Info ('embeddable zip を {0} に展開します' -f $script:PythonDir)
    $zip = Join-Path $script:DownloadDir ('python-{0}-embed-amd64.zip' -f $script:PythonVersion)
    $downloaded = Invoke-Download -Url $script:PythonEmbedUrl -Destination $zip `
        -Label ('Python {0} embeddable zip' -f $script:PythonVersion) -MinBytes 5MB
    if (-not $downloaded) { return $null }

    if (-not (Expand-ZipArchive -ZipPath $zip -Destination $script:PythonDir)) { return $null }

    $pythonExe = Join-Path $script:PythonDir 'python.exe'
    if (-not (Test-Path -LiteralPath $pythonExe)) {
        Write-Fail ('展開しましたが python.exe が見つかりません: {0}' -f $script:PythonDir)
        return $null
    }

    # _pth に site-packages を足す（embeddable は既定で site を読み込まない）
    $pth = Get-ChildItem -LiteralPath $script:PythonDir -Filter 'python*._pth' -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pth) {
        $lines = @()
        try { $lines = @(Get-Content -LiteralPath $pth.FullName -Encoding ASCII) } catch { $lines = @() }
        $needed = @('Lib\site-packages', 'import site')
        foreach ($item in $needed) {
            if (-not ($lines | Where-Object { $_.Trim() -eq $item })) { $lines += $item }
        }
        $lines | Set-Content -LiteralPath $pth.FullName -Encoding ASCII
        Write-Detail ('_pth を修正: {0}' -f $pth.Name)
    }

    # pip を入れる（wheel を取ってくるのに必要）
    $hasPip = $false
    try {
        & $pythonExe -m pip --version 2>&1 | Out-Null
        $hasPip = ($LASTEXITCODE -eq 0)
    } catch { $hasPip = $false }
    if (-not $hasPip) {
        $getPip = Join-Path $script:DownloadDir 'get-pip.py'
        if (Invoke-Download -Url $script:GetPipUrl -Destination $getPip -Label 'get-pip.py（pip の導入用）' -MinBytes 100KB) {
            Write-Info 'pip を導入しています…'
            try {
                & $pythonExe $getPip --no-warn-script-location 2>&1 | ForEach-Object { Write-Detail ([string]$_) }
            } catch {
                Write-Warn ('pip の導入に失敗しました: {0}' -f $_.Exception.Message)
            }
        }
    }

    $measured = Measure-PythonExe -Exe $pythonExe -Source 'embeddable zip'
    if ($null -eq $measured) {
        Write-Fail 'embeddable Python を確認できませんでした。'
        return $null
    }
    return $measured
}

function Step-Python {
    param([int]$Index, [int]$Total)

    Write-Step -Index $Index -Total $Total -Title 'Python 3.11 以上を用意する'

    $found = Find-PythonInstallation
    if ($null -ne $found) {
        $script:Python = $found
        Write-Ok ('Python {0} を見つけました' -f $found.Version)
        Write-Detail ('場所: {0}' -f $found.Path)
        Write-Detail ('検出元: {0}' -f $found.Source)
        if (-not (Test-Path -LiteralPath $found.PythonwPath)) {
            Write-Warn 'pythonw.exe が見つかりません。起動時に黒い画面が出る場合があります。'
        }
        # 標準ライブラリが使えるか軽く確認（壊れた Python / Store 版のスタブ対策）
        $probeCode = "import sqlite3, http.server, json, urllib.request; print('ok')"
        $probeOk = $false
        try {
            $probe = & $found.Path -c $probeCode 2>&1
            $probeOk = ($LASTEXITCODE -eq 0) -and (($probe | Out-String) -match 'ok')
        } catch { $probeOk = $false }
        if ($probeOk) {
            Write-Detail '標準ライブラリ（sqlite3 / http.server）を確認しました'
        } else {
            Write-Warn 'Python の標準ライブラリを確認できませんでした（サーバーが起動しない可能性があります）'
            Write-Detail 'python.org の 3.11 を入れ直すと直ることが多いです。'
        }
        Add-Check -Group 'Python' -Name 'Python 3.11 以上（64bit）' -Ok $true `
            -Detail ('{0} ({1})' -f $found.Version, $found.Path)
        return $true
    }

    Write-Warn 'Python 3.11 以上（64bit）が見つかりませんでした。'
    Write-Detail 'PPT Lab のサーバーは Python だけで動くので、Python が無いと画面が開けません。'

    if ($script:DryRun) {
        Write-Dry 'Python の導入予定（どの方法で入れるかは実行時に選べます）'
        Write-Detail ('  方法A: 公式インストーラ（推奨） {0}' -f $script:PythonInstallerUrl)
        Write-Detail ('  方法B: embeddable zip          {0} → {1}' -f $script:PythonEmbedUrl, $script:PythonDir)
        Add-Check -Group 'Python' -Name 'Python 3.11 以上（64bit）' -Ok $false -Detail '未検出（DryRun）' `
            -Hint 'install.cmd を -DryRun 無しで実行すると自動で入ります' -Url 'https://www.python.org/downloads/windows/'
        return $true
    }

    $doInstall = Confirm-YesNo -Question 'Python 3.11.9 を自動でインストールしますか？' -Default $true
    if (-not $doInstall) {
        Write-Warn 'Python を自動導入せずに続けます（この後、画面の起動だけができません）。'
        Write-Detail '手動で入れる場合: https://www.python.org/downloads/windows/'
        Write-Detail '「Windows installer (64-bit)」を選び、インストール時に Add python.exe to PATH にチェック。'
        Add-Check -Group 'Python' -Name 'Python 3.11 以上（64bit）' -Ok $false -Detail '未検出' `
            -Hint 'https://www.python.org/downloads/windows/ から 3.11 の 64bit 版を入れてください' `
            -Url 'https://www.python.org/downloads/windows/'
        return $true
    }

    $method = Read-Choice -Question 'Python の入れ方を選んでください' -Options @(
        '公式インストーラを使う（推奨・スタートメニューにも登録される）',
        'embeddable zip を tools\ppt-setup\python に展開する（レジストリを触らない）',
        'やめる（自分で入れる）'
    ) -Default 1

    $installed = $null
    switch ($method) {
        1 { $installed = Install-PythonByOfficialInstaller }
        2 { $installed = Install-PythonEmbeddable }
        default {
            Write-Info 'Python の自動導入をやめました。'
            Add-Check -Group 'Python' -Name 'Python 3.11 以上（64bit）' -Ok $false -Detail '未検出' `
                -Hint 'https://www.python.org/downloads/windows/ から 64bit 版を入れてください' `
                -Url 'https://www.python.org/downloads/windows/'
            return $true
        }
    }

    if ($null -eq $installed) {
        Write-Fail 'Python の導入に失敗しました。'
        Write-Detail '手動で入れる場合の手順:'
        Write-Detail '  1) https://www.python.org/downloads/windows/ を開く'
        Write-Detail '  2) Python 3.11 の「Windows installer (64-bit)」をダウンロード'
        Write-Detail '  3) 実行して「Add python.exe to PATH」にチェックして Install'
        Write-Detail '  4) この install.cmd をもう一度実行'
        Add-Check -Group 'Python' -Name 'Python 3.11 以上（64bit）' -Ok $false -Detail '導入に失敗' `
            -Hint 'https://www.python.org/downloads/windows/ から手動で入れてください' `
            -Url 'https://www.python.org/downloads/windows/'
        return $false
    }

    $script:Python = $installed
    Write-Ok ('Python {0} を導入しました' -f $installed.Version)
    Write-Detail ('場所: {0}' -f $installed.Path)
    Add-Action ('Python {0} を導入（{1}）' -f $installed.Version, $installed.Path)
    Add-Check -Group 'Python' -Name 'Python 3.11 以上（64bit）' -Ok $true `
        -Detail ('{0} ({1})' -f $installed.Version, $installed.Path)
    return $true
}

# ============================================================================
# [3/8] GPU 検出
# ============================================================================

function Get-NvidiaSmiPath {
    $search = @(
        (Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'),
        'C:\Program Files\NVIDIA Corporation\NVSMI\nvidia-smi.exe'
    )
    foreach ($path in $search) {
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    }
    $command = Get-Command 'nvidia-smi.exe' -ErrorAction SilentlyContinue
    if ($command -and $command.Source) { return $command.Source }
    return ''
}

function Get-GpuInfo {
    $smiPath = Get-NvidiaSmiPath
    $info = [pscustomobject]@{
        Present     = $false
        Name        = ''
        Driver      = ''
        DriverMajor = 0
        MemoryMB    = 0
        SmiPath     = $smiPath
        Detail      = ''
    }
    if ([string]::IsNullOrWhiteSpace($smiPath)) {
        $info.Detail = 'nvidia-smi.exe が見つかりません（NVIDIA ドライバー未導入の可能性）'
        return $info
    }
    try {
        $raw = & $smiPath '--query-gpu=name,driver_version,memory.total' '--format=csv,noheader' 2>&1
        if ($LASTEXITCODE -ne 0) {
            $info.Detail = 'nvidia-smi が GPU を返しませんでした（GPU 非搭載 / ドライバー停止の可能性）'
            return $info
        }
        $line = (@($raw) | Select-Object -First 1)
        if ([string]::IsNullOrWhiteSpace([string]$line)) {
            $info.Detail = 'nvidia-smi の出力が空でした'
            return $info
        }
        $fields = ([string]$line) -split ','
        $info.Present = $true
        $info.Name = $fields[0].Trim()
        if ($fields.Count -ge 2) { $info.Driver = $fields[1].Trim() }
        if ($fields.Count -ge 3) {
            $memory = [regex]::Match($fields[2], '\d+')
            if ($memory.Success) { $info.MemoryMB = [int]$memory.Value }
        }
        $driverVersion = ConvertTo-VersionSafe $info.Driver
        if ($null -ne $driverVersion) {
            # "591.84" のような 2 桁区切りは Major がそのまま世代番号になる
            $info.DriverMajor = [int]$driverVersion.Major
        }
        $info.Detail = ('{0} / ドライバー {1}' -f $info.Name, $info.Driver)
    } catch {
        $info.Detail = ('nvidia-smi の実行に失敗しました: {0}' -f $_.Exception.Message)
    }
    return $info
}

function Step-Gpu {
    param([int]$Index, [int]$Total)

    Write-Step -Index $Index -Total $Total -Title 'GPU（NVIDIA）を確認する'

    $gpu = Get-GpuInfo
    $script:Gpu = $gpu

    if ($gpu.Present) {
        Write-Ok ('GPU: {0}' -f $gpu.Name)
        Write-Detail ('ドライバー: {0} / ビデオメモリ: 約 {1:N0} MB' -f $gpu.Driver, $gpu.MemoryMB)
        if ($gpu.MemoryMB -gt 0 -and $gpu.MemoryMB -lt 4096) {
            Write-Warn ('ビデオメモリが {0:N0} MB と少なめです。長い動画では失敗することがあります。' -f $gpu.MemoryMB)
        }
        Add-Check -Group 'GPU' -Name 'NVIDIA GPU' -Ok $true -Detail $gpu.Name
        Add-Check -Group 'GPU' -Name 'NVIDIA ドライバー' -Ok $true -Detail $gpu.Driver -Required $false
        return $true
    }

    Write-Warn ('GPU を検出できませんでした: {0}' -f $gpu.Detail)
    Write-Host ''
    Write-Host '  CPU モードでは「非常に遅い」ため、実用に耐えないことがあります。' -ForegroundColor Yellow
    Write-Host '  （目安: GPU で数分の動画が、CPU では数十分〜数時間かかります）' -ForegroundColor Yellow
    Write-Host ''
    Write-Detail '確認すること:'
    Write-Detail '  1) NVIDIA の GPU が載っている PC か（Intel/AMD の内蔵 GPU だけでは動きません）'
    Write-Detail ('  2) NVIDIA ドライバーを入れてあるか: {0}' -f $script:NvidiaDriverUrl)
    Write-Detail '  3) ノート PC で GPU が無効になっていないか（NVIDIA コントロールパネル）'

    Add-Check -Group 'GPU' -Name 'NVIDIA GPU' -Ok $false -Detail $gpu.Detail `
        -Hint 'NVIDIA ドライバーを入れて PC を再起動してから再実行してください' -Url $script:NvidiaDriverUrl

    if ($script:DryRun) {
        Write-Dry 'ここで「CPU モードのまま続行しますか？」と確認します（既定: いいえ）'
        return $true
    }
    $continue = Confirm-YesNo -Question 'GPU 無し（CPU モード）のまま続行しますか？' -Default $false
    if (-not $continue) {
        Write-Warn 'CPU モードでのインストールを中止しました。'
        Write-Detail 'GPU を使えるようにしてから、もう一度 install.cmd を実行してください。'
        $script:Cancelled = $true
        return $false
    }
    Write-Warn 'CPU モードで続行します（解析はとても遅くなります）。'
    return $true
}

# ============================================================================
# [4/8] CUDA ランタイム / cuDNN
# ============================================================================

function Get-CudaRequirement {
    <#
      onnxruntime_providers_cuda.dll が実際に要求する DLL 名を読み取る。
      「既存 DLL の名前・バージョンに合わせる」ための最初の一歩。
      例: cublas64_13.dll / cublasLt64_13.dll / cudnn64_9.dll → CUDA 13 系 + cuDNN 9 系。
    #>
    $provider = Join-Path $script:BinDir 'onnxruntime_providers_cuda.dll'
    $requirements = [pscustomobject]@{
        CudaMajor      = 0
        CudnnMajor     = 0
        CudartMajor    = 0
        CublasName     = ''
        CublasLtName   = ''
        CudartName     = ''
        CudnnName      = ''
        CufftNames     = @()
        Source         = ''
        FoundNames     = @()
    }

    if (Test-Path -LiteralPath $provider) {
        $matchesFound = Get-AsciiMatches -Path $provider -Patterns @(
            'cublas64_\d+', 'cublasLt64_\d+', 'cudart64_\d+', 'cudnn64_\d+',
            'cudnn_[a-z]+64_\d+', 'cufft\w*64_\d+', 'nvrtc64_\d+_\d+', 'nvJitLink_\d+_\d+'
        )
        $requirements.FoundNames = $matchesFound
        # cuFFT は pip では取らず、同梱物かシステムの CUDA Toolkit から補う
        $requirements.CufftNames = @($matchesFound | Where-Object { $_ -match '^cufft\w*64_\d+$' } | ForEach-Object { $_ + '.dll' })
        foreach ($name in $matchesFound) {
            # インポート表の名前には拡張子が無いので、必ず .dll を付けて扱う
            if ($name -match '^cublas64_(\d+)$') { $requirements.CudaMajor = [int]$Matches[1]; $requirements.CublasName = $name + '.dll' }
            elseif ($name -match '^cublasLt64_(\d+)$') { $requirements.CublasLtName = $name + '.dll' }
            elseif ($name -match '^cudart64_(\d+)$') { $requirements.CudartMajor = [int]$Matches[1]; $requirements.CudartName = $name + '.dll' }
            elseif ($name -match '^cudnn64_(\d+)$') { $requirements.CudnnMajor = [int]$Matches[1]; $requirements.CudnnName = $name + '.dll' }
        }
        $requirements.Source = 'onnxruntime_providers_cuda.dll のインポート情報'
    }

    # 読めなかったときは、既に bin にある DLL 名から推測する
    if ($requirements.CudaMajor -eq 0) {
        $existing = Get-ChildItem -LiteralPath $script:BinDir -Filter 'cublasLt64_*.dll' -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($existing -and $existing.Name -match 'cublasLt64_(\d+)') {
            $requirements.CudaMajor = [int]$Matches[1]
            $requirements.Source = 'bin フォルダーの DLL 名から推測'
        }
    }
    if ($requirements.CudaMajor -eq 0) {
        $requirements.CudaMajor = 12
        $requirements.Source = '判定できず（CUDA 12 と仮定）'
    }
    if ($requirements.CudnnMajor -eq 0) { $requirements.CudnnMajor = 9 }
    if ($requirements.CudartMajor -eq 0) { $requirements.CudartMajor = $requirements.CudaMajor }

    if ([string]::IsNullOrEmpty($requirements.CublasName)) { $requirements.CublasName = 'cublas64_{0}.dll' -f $requirements.CudaMajor }
    if ([string]::IsNullOrEmpty($requirements.CublasLtName)) { $requirements.CublasLtName = 'cublasLt64_{0}.dll' -f $requirements.CudaMajor }
    if ([string]::IsNullOrEmpty($requirements.CudartName)) { $requirements.CudartName = 'cudart64_{0}.dll' -f $requirements.CudartMajor }
    if ([string]::IsNullOrEmpty($requirements.CudnnName)) { $requirements.CudnnName = 'cudnn64_{0}.dll' -f $requirements.CudnnMajor }
    return $requirements
}

function Get-CudaSearchDirs {
    <#
      bin 以外で DLL を探す場所。CUDA Toolkit は v13.3 のように bin\x64 へ DLL を置く版がある。
    #>
    $dirs = New-Object System.Collections.ArrayList
    $toolkitRoot = Join-Path $env:ProgramFiles 'NVIDIA GPU Computing Toolkit\CUDA'
    if (Test-Path -LiteralPath $toolkitRoot) {
        foreach ($version in (Get-ChildItem -LiteralPath $toolkitRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)) {
            [void]$dirs.Add((Join-Path $version.FullName 'bin\x64'))
            [void]$dirs.Add((Join-Path $version.FullName 'bin'))
        }
    }
    if ($env:SystemRoot) { [void]$dirs.Add((Join-Path $env:SystemRoot 'System32')) }
    foreach ($entry in ($env:PATH -split ';')) {
        if ($entry -match '(?i)CUDA|cudnn|NVIDIA') { [void]$dirs.Add($entry.Trim()) }
    }
    return @($dirs | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique)
}

function Find-DllSource {
    <#
      bin に無い DLL を、システム側（CUDA Toolkit など）から探す。
      「見つかった場所」を返すので、コピー元として使える。
    #>
    param([string]$Name)
    foreach ($dir in (Get-CudaSearchDirs)) {
        $path = Join-Path $dir $Name
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    }
    return ''
}

function Backup-BinFile {
    param([string]$Path, [string]$Tag)
    if (-not (Test-Path -LiteralPath $Path)) { return }
    if ($script:DryRun) { return }
    $target = Join-Path $script:BackupDir $Tag
    if (-not (Test-Path -LiteralPath $target)) { New-Item -ItemType Directory -Force -Path $target | Out-Null }
    $name = Split-Path -Leaf $Path
    $destination = Join-Path $target $name
    if (-not (Test-Path -LiteralPath $destination)) {
        Copy-Item -LiteralPath $Path -Destination $destination -Force
        Write-Detail ('バックアップ: {0} → {1}' -f $name, $destination)
    }
}

function Copy-DllToBin {
    <#
      DLL を bin へコピーする。既に同じか新しいものが入っていれば何もしない（冪等）。
      cuDNN のように「セットで揃っていないと動かない」ものは呼び出し側でまとめて渡す。
    #>
    param(
        [string]$Source,
        [string]$Name,
        [string]$Tag = 'dll'
    )
    $destination = Join-Path $script:BinDir $Name
    $sourceVersion = ConvertTo-VersionSafe (Get-FileVersionText $Source)
    $destVersion = ConvertTo-VersionSafe (Get-FileVersionText $destination)

    if ((Test-Path -LiteralPath $destination) -and $null -ne $sourceVersion -and $null -ne $destVersion) {
        if ($sourceVersion -le $destVersion) {
            Write-Detail ('既に同じか新しい版があります: {0} ({1})' -f $Name, $destVersion)
            return $true
        }
    }

    if ($script:DryRun) {
        Write-Dry ('コピー予定: {0} → {1}' -f $Source, $destination)
        return $false
    }

    if (Test-Path -LiteralPath $destination) { Backup-BinFile -Path $destination -Tag $Tag }
    Copy-Item -LiteralPath $Source -Destination $destination -Force
    Write-Detail ('コピーしました: {0}' -f $Name)
    return $true
}

function Invoke-PipInstall {
    <#
      pip で NVIDIA の wheel を取ってきて、指定フォルダーに展開する。
      NVIDIA アカウントは不要（PyPI の公式 wheel を使う）。
      戻り値: $true / $false
    #>
    param(
        [string]$TargetDir,
        [string[]]$Packages,
        [string]$Label
    )
    if ($null -eq $script:Python) {
        Write-Fail ('pip を使うには Python が必要です（{0}）' -f $Label)
        return $false
    }
    if ($script:DryRun) {
        Write-Dry ('pip install --target {0} {1}' -f $TargetDir, ($Packages -join ' '))
        return $false
    }
    if (-not (Test-Path -LiteralPath $TargetDir)) { New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null }

    $python = $script:Python.Path
    $arguments = @('-m', 'pip', 'install', '--disable-pip-version-check', '--no-warn-script-location', '--upgrade', '--target', $TargetDir) + $Packages
    Write-Info ('pip で取得中: {0}（数百 MB になることがあります）…' -f ($Packages -join ' '))
    $output = $null
    try {
        $output = & $python @arguments 2>&1
        $exitCode = $LASTEXITCODE
    } catch {
        Write-Fail ('pip の実行に失敗しました: {0}' -f $_.Exception.Message)
        return $false
    }
    foreach ($line in @($output)) { Write-Detail ([string]$line) }
    if ($exitCode -ne 0) {
        Write-Warn ('pip が終了コード {0} で終わりました' -f $exitCode)
        return $false
    }
    Write-Ok ('pip で取得しました: {0}' -f $Label)
    return $true
}

function Get-CudnnSetNames {
    <#
      cuDNN 9 は 1 個の DLL ではなく「セット」で動く。
      cudnn64_9.dll と cudnn_ops64_9.dll / cudnn_engines_precompiled64_9.dll などが揃っていないと、
      実行時に読み込みエラーになる。ここでは指定フォルダーにある分を全部返す。
    #>
    param(
        [string]$Root,
        [int]$Major
    )
    if ([string]::IsNullOrEmpty($Root)) { return @() }
    if (-not (Test-Path -LiteralPath $Root)) { return @() }
    $filter = 'cudnn*64_{0}.dll' -f $Major
    return @(Get-ChildItem -LiteralPath $Root -Filter $filter -File -ErrorAction SilentlyContinue | Sort-Object Name)
}

function Copy-CudnnSet {
    <#
      cuDNN のセットを bin へコピーする（既にあるものは飛ばす）。
      コピーした（または DryRun でコピー予定の）ファイル名を返す。
    #>
    param(
        [string]$SourceDir,
        [int]$Major,
        [string]$Tag = 'cuda'
    )
    $names = New-Object System.Collections.ArrayList
    foreach ($file in (Get-CudnnSetNames -Root $SourceDir -Major $Major)) {
        $destination = Join-Path $script:BinDir $file.Name
        if (Test-Path -LiteralPath $destination) { continue }
        if (Copy-DllToBin -Source $file.FullName -Name $file.Name -Tag $Tag) {
            [void]$names.Add($file.Name)
        } elseif ($script:DryRun) {
            [void]$names.Add($file.Name)
        }
    }
    return $names
}

function Get-PipDllSource {
    <#
      pip --target で展開したフォルダーから、指定 DLL を再帰的に探す。
    #>
    param(
        [string]$Root,
        [string]$Name
    )
    if (-not (Test-Path -LiteralPath $Root)) { return '' }
    $found = Get-ChildItem -LiteralPath $Root -Recurse -Filter $Name -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.FullName }
    return ''
}

function Get-CudaDllHint {
    <#
      足りない DLL ごとの「どこから持ってくるか」を返す。
      一覧の ❌ の下に出る対処文になる。
    #>
    param([string]$Name)
    if ($Name -like 'cudnn*') {
        return 'cuDNN の配布物（pip なら nvidia/cudnn/bin）から tools\video-recovery-runtime\bin へコピーしてください'
    }
    if ($Name -like 'cufft*') {
        return '配布物の bin に同梱されています。別の PC の tools\video-recovery-runtime\bin からコピーしてください'
    }
    return 'tools\video-recovery-runtime\bin に置いてください（install.cmd をもう一度実行すると自動取得します）'
}

function Get-CudaDllUrl {
    param([string]$Name)
    if ($Name -like 'cudnn*') { return $script:CudnnDownloadUrl }
    return $script:CudaDownloadUrl
}

function Step-Cuda {
    param([int]$Index, [int]$Total)

    Write-Step -Index $Index -Total $Total -Title 'CUDA ランタイムと cuDNN をそろえる'

    $requirement = Get-CudaRequirement
    $script:Cuda = $requirement

    Write-Info ('解析エンジンが要求する世代: CUDA {0} 系 / cuDNN {1} 系' -f $requirement.CudaMajor, $requirement.CudnnMajor)
    Write-Detail ('判定の根拠: {0}' -f $requirement.Source)
    if ($requirement.FoundNames.Count -gt 0) {
        Write-Detail ('参照した DLL 名: {0}' -f ($requirement.FoundNames -join ', '))
    }

    # --- ドライバーの世代チェック（CUDA 13 は 580 系以上が必要） ---
    if ($null -ne $script:Gpu -and $script:Gpu.Present) {
        $requiredDriver = 525
        if ($requirement.CudaMajor -ge 13) { $requiredDriver = 580 }
        if ($script:Gpu.DriverMajor -gt 0 -and $script:Gpu.DriverMajor -lt $requiredDriver) {
            Write-Warn ('NVIDIA ドライバーが古い可能性があります（{0}）。CUDA {1} 系には {2} 以上が必要です。' -f $script:Gpu.Driver, $requirement.CudaMajor, $requiredDriver)
            Write-Detail ('最新ドライバー: {0}' -f $script:NvidiaDriverUrl)
            Add-Check -Group 'GPU' -Name 'NVIDIA ドライバーの版' -Ok $false -Detail $script:Gpu.Driver `
                -Required $false -Hint ('{0} 以上へ更新してください' -f $requiredDriver) -Url $script:NvidiaDriverUrl
        } else {
            Add-Check -Group 'GPU' -Name 'NVIDIA ドライバーの版' -Ok $true -Detail $script:Gpu.Driver -Required $false
        }
    }

    # --- 必要な DLL の分類 ---
    #   pipNames  : pip（nvidia-cublas / nvidia-cuda-runtime）で取得できる
    #   extraNames: 同梱物かシステムの CUDA Toolkit から補う（cuFFT は pip の対象外）
    #   cuDNN     : 9 系は分割ライブラリも含めて「セット」で必要
    $pipNames = @($requirement.CublasName, $requirement.CublasLtName, $requirement.CudartName)
    $extraNames = @($requirement.CufftNames)
    $cudnnInBin = @(Get-CudnnSetNames -Root $script:BinDir -Major $requirement.CudnnMajor)
    $cudnnNames = @($cudnnInBin | ForEach-Object { $_.Name })

    $wantedNames = New-Object System.Collections.ArrayList
    foreach ($name in $pipNames) { if (-not $wantedNames.Contains($name)) { [void]$wantedNames.Add($name) } }
    foreach ($name in $extraNames) { if (-not $wantedNames.Contains($name)) { [void]$wantedNames.Add($name) } }
    foreach ($name in $cudnnNames) { if (-not $wantedNames.Contains($name)) { [void]$wantedNames.Add($name) } }
    if (-not $wantedNames.Contains($requirement.CudnnName)) { [void]$wantedNames.Add($requirement.CudnnName) }

    $systemSources = @{}
    $copied = New-Object System.Collections.ArrayList
    $missing = New-Object System.Collections.ArrayList
    $cudnnPlanned = @()

    # --- 1) bin にあるか確認。無ければシステム（CUDA Toolkit など）からコピー ---
    foreach ($name in $wantedNames) {
        $inBin = Join-Path $script:BinDir $name
        if (Test-Path -LiteralPath $inBin) {
            $version = Get-FileVersionText $inBin
            if ([string]::IsNullOrEmpty($version)) { Write-Detail ('あり: {0}' -f $name) } else { Write-Detail ('あり: {0} ({1})' -f $name, $version) }
            continue
        }
        $systemSource = Find-DllSource -Name $name
        if ([string]::IsNullOrEmpty($systemSource)) {
            [void]$missing.Add($name)
            continue
        }
        Write-Info ('bin に無いのでシステム側から補います: {0}' -f $name)
        Write-Detail ('コピー元: {0}' -f $systemSource)
        $systemSources[$name] = $systemSource
        if (Copy-DllToBin -Source $systemSource -Name $name -Tag 'cuda') {
            [void]$copied.Add($name)
            Add-Action ('{0} を bin へコピー（{1} から）' -f $name, $systemSource)
        } elseif ($script:DryRun) {
            [void]$copied.Add($name)
            if ($name -like 'cudnn*') { $cudnnPlanned += $name }
        }
    }

    # --- 2) 足りない分は pip で取得（NVIDIA アカウント不要） ---
    $pipTargets = @($missing | Where-Object { $pipNames -contains $_ })
    $cudnnMissing = ($cudnnNames.Count -eq 0) -or (@($missing | Where-Object { $_ -like 'cudnn*' }).Count -gt 0)
    $needPip = ($pipTargets.Count -gt 0) -or $cudnnMissing

    $cudaRange = ('>={0},<{1}' -f $requirement.CudaMajor, ($requirement.CudaMajor + 1))
    $cudnnPackage = ('nvidia-cudnn-cu{0}' -f $requirement.CudaMajor)
    $pipPackages = New-Object System.Collections.ArrayList
    [void]$pipPackages.Add(('nvidia-cublas{0}' -f $cudaRange))
    [void]$pipPackages.Add(('nvidia-cuda-runtime{0}' -f $cudaRange))
    $cudnnPin = ''
    $existingCudnnVersion = ConvertTo-VersionSafe (Get-FileVersionText (Join-Path $script:BinDir $requirement.CudnnName))
    if ($null -ne $existingCudnnVersion) {
        $cudnnPin = ('{0}.{1}.{2}.{3}' -f $existingCudnnVersion.Major, $existingCudnnVersion.Minor, $existingCudnnVersion.Build, $existingCudnnVersion.Revision)
        [void]$pipPackages.Add(('{0}=={1}' -f $cudnnPackage, $cudnnPin))
    } else {
        # 手元に cuDNN が無い場合は、この配布物で動作確認済みの版に固定する（毎回変わらないように）
        $cudnnPin = $script:CudnnPinVersion
        [void]$pipPackages.Add(('{0}=={1}' -f $cudnnPackage, $cudnnPin))
        Write-Info ('cuDNN は動作確認済みの {0} に固定します。' -f $cudnnPin)
    }

    if ($needPip) {
        Write-Host ''
        if ($missing.Count -gt 0) {
            Write-Warn ('bin に無い DLL が {0} 個あります: {1}' -f $missing.Count, ($missing -join ', '))
        }
        Write-Detail 'CUDA Toolkit が入っていれば、そこからコピーするのが一番速い方法です。'
        Write-Detail '入っていない場合は、pip の NVIDIA 公式 wheel から取り寄せられます（NVIDIA アカウント不要）。'
        if ($cudnnPin) {
            Write-Info ('既存の {0} は {1}。同じ版を pip で指定します。' -f $requirement.CudnnName, $cudnnPin)
        }

        if ($script:DryRun) {
            Write-Dry ('pip install --target {0} {1}' -f $script:CudaDir, ($pipPackages -join ' '))
            Write-Detail '（実行時は、この後に wheel の中の DLL を bin へコピーします）'
        } else {
            $doPip = Confirm-YesNo -Question 'pip で NVIDIA の CUDA / cuDNN を取得して bin にコピーしますか？（数百 MB）' -Default $true
            if ($doPip) {
                $ok = Invoke-PipInstall -TargetDir $script:CudaDir -Packages $pipPackages -Label 'CUDA ランタイム + cuBLAS + cuDNN'
                if (-not $ok -and $cudnnPin) {
                    # 固定した版が無いこともあるので、固定を外して一度だけやり直す
                    Write-Warn '指定したバージョンで取得できませんでした。バージョン指定を外して再試行します。'
                    $pipPackages = @(
                        ('nvidia-cublas{0}' -f $cudaRange),
                        ('nvidia-cuda-runtime{0}' -f $cudaRange),
                        $cudnnPackage
                    )
                    $ok = Invoke-PipInstall -TargetDir $script:CudaDir -Packages $pipPackages -Label 'CUDA ランタイム + cuBLAS + cuDNN（最新版）'
                }

                # wheel の中の DLL を bin へ（cublas / cublasLt / cudart）
                foreach ($name in $pipNames) {
                    if (Test-Path -LiteralPath (Join-Path $script:BinDir $name)) { continue }
                    $source = Get-PipDllSource -Root $script:CudaDir -Name $name
                    if ([string]::IsNullOrEmpty($source)) {
                        Write-Warn ('pip の中に {0} が見つかりませんでした' -f $name)
                        continue
                    }
                    if (Copy-DllToBin -Source $source -Name $name -Tag 'cuda') {
                        [void]$copied.Add($name)
                        Add-Action ('{0} を bin へコピー（pip 版）' -f $name)
                    }
                }

                # cuDNN は分割ライブラリも含めてセットでコピーする
                if (@(Get-CudnnSetNames -Root $script:BinDir -Major $requirement.CudnnMajor).Count -eq 0) {
                    $pipCudnn = Get-PipDllSource -Root $script:CudaDir -Name $requirement.CudnnName
                    if ($pipCudnn) {
                        $planned = @(Copy-CudnnSet -SourceDir (Split-Path -Parent $pipCudnn) -Major $requirement.CudnnMajor -Tag 'cuda')
                        foreach ($name in $planned) {
                            if (-not $copied.Contains($name)) { [void]$copied.Add($name) }
                            Add-Action ('{0} を bin へコピー（pip 版 cuDNN）' -f $name)
                        }
                    } else {
                        Write-Warn 'pip の中に cuDNN が見つかりませんでした'
                    }
                }
            } else {
                Write-Warn 'CUDA / cuDNN の自動補完を行いませんでした。'
                Write-Detail 'この状態でも画面は「CPU モード」で起動できますが、解析は非常に遅くなります（目安: GPU の数十倍）。'
                Write-Detail ('手動で入れる場合: CUDA Toolkit {0} 系 {1}' -f $requirement.CudaMajor, $script:CudaDownloadUrl)
                Write-Detail ('cuDNN: {0}（NVIDIA アカウントが必要です）' -f $script:CudnnDownloadUrl)
                Write-Detail ('CUDA Toolkit の DLL は C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v{0}.x\bin\x64 にあります。' -f $requirement.CudaMajor)
            }
        }
    }

    # --- 3) 最終確認: インポート名と bin の突き合わせ ---
    $finalNames = New-Object System.Collections.ArrayList
    foreach ($name in $pipNames) { if (-not $finalNames.Contains($name)) { [void]$finalNames.Add($name) } }
    foreach ($name in $extraNames) { if (-not $finalNames.Contains($name)) { [void]$finalNames.Add($name) } }
    foreach ($name in (Get-CudnnSetNames -Root $script:BinDir -Major $requirement.CudnnMajor | ForEach-Object { $_.Name })) {
        if (-not $finalNames.Contains($name)) { [void]$finalNames.Add($name) }
    }
    foreach ($name in $cudnnPlanned) { if (-not $finalNames.Contains($name)) { [void]$finalNames.Add($name) } }
    if (-not $finalNames.Contains($requirement.CudnnName)) { [void]$finalNames.Add($requirement.CudnnName) }

    Write-Host ''
    Write-Info 'インポート名と bin の突き合わせ:'
    $failed = New-Object System.Collections.ArrayList
    foreach ($name in $finalNames) {
        $path = Join-Path $script:BinDir $name
        $ok = $false
        $detail = 'bin にありません'
        if (Test-Path -LiteralPath $path) {
            $ok = $true
            $detail = Get-FileDetailText $path
        } elseif ($script:DryRun -and $systemSources.ContainsKey($name)) {
            $ok = $true
            $detail = 'システム側にあり（実行時にコピー）'
        } elseif ($script:DryRun -and $needPip -and ($pipNames -contains $name -or $name -eq $requirement.CudnnName)) {
            $ok = $true
            $detail = 'DryRun: pip で取得予定'
        } else {
            [void]$failed.Add($name)
        }
        $mark = $script:MarkNg
        if ($ok) { $mark = $script:MarkOk }
        Write-Detail ('  {0} {1} → {2}' -f $mark, $name, $detail)
        Add-Check -Group 'CUDA / cuDNN' -Name $name -Ok $ok -Detail $detail `
            -Hint (Get-CudaDllHint $name) -Url (Get-CudaDllUrl $name)
    }

    $cudnnCount = @(Get-CudnnSetNames -Root $script:BinDir -Major $requirement.CudnnMajor).Count
    if ($cudnnCount -gt 0) {
        Write-Detail ('cuDNN のセット: {0} 個' -f $cudnnCount)
    }

    if ($failed.Count -eq 0) {
        Write-Ok ('CUDA / cuDNN の DLL がそろいました（{0} 個）' -f $finalNames.Count)
        return $true
    }

    Write-Fail ('まだ足りない DLL があります: {0}' -f ($failed -join ', '))
    Write-Host ''
    Write-Warn 'CUDA が無くても「CPU モード」で画面は起動できます。ただし解析は非常に遅くなります（目安: GPU の数十倍）。'
    Write-Detail '手動で入れる場合の手順:'
    Write-Detail ('  1) NVIDIA の CUDA Toolkit（{0} 系）をインストールする' -f $requirement.CudaMajor)
    Write-Detail ('     {0}' -f $script:CudaDownloadUrl)
    Write-Detail ('  2) C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v{0}.x\bin\x64 を開く' -f $requirement.CudaMajor)
    Write-Detail ('  3) {0} / {1} / {2} をコピー' -f $requirement.CublasName, $requirement.CublasLtName, $requirement.CudartName)
    Write-Detail '  4) tools\video-recovery-runtime\bin に貼り付ける'
    Write-Detail ('  5) cuDNN は {0} から（要 NVIDIA アカウント）' -f $script:CudnnDownloadUrl)
    Write-Detail '  6) cuFFT（cufft64_12.dll）は配布物の bin に同梱されています。別の PC からコピーしてください。'
    return $false
}

# ============================================================================
# [5/8] ffmpeg（LGPL ビルドへの差し替え）
# ============================================================================

function Get-FfmpegInfo {
    <#
      既存 ffmpeg.exe のビルド情報を読む。
      gyan.dev の full_build は --enable-gpl なので、配布時は LGPL に差し替えたい。
    #>
    param([string]$Exe)
    $info = [pscustomobject]@{
        Present     = $false
        Version     = ''
        IsGpl       = $false
        IsNonFree   = $false
        IsVersion3  = $false
        AvcodecMajor = 0
        Detail      = ''
    }
    if (-not (Test-Path -LiteralPath $Exe)) {
        $info.Detail = 'ffmpeg.exe がありません'
        return $info
    }
    $info.Present = $true
    # av*.dll が欠けた状態で ffmpeg.exe を起動すると、Windows のエラーダイアログが
    # 出てスクリプトが止まってしまう。先に依存 DLL の有無を確認する。
    $exeDir = Split-Path -Parent $Exe
    $deps = @(
        @(Get-ChildItem -LiteralPath $exeDir -Filter 'avcodec-*.dll' -File -ErrorAction SilentlyContinue).Count,
        @(Get-ChildItem -LiteralPath $exeDir -Filter 'avformat-*.dll' -File -ErrorAction SilentlyContinue).Count,
        @(Get-ChildItem -LiteralPath $exeDir -Filter 'avutil-*.dll' -File -ErrorAction SilentlyContinue).Count
    )
    if (($deps | Measure-Object -Minimum).Minimum -lt 1) {
        $info.Detail = 'av*.dll が無いため、ビルド情報を確認できません'
        return $info
    }
    try {
        $output = & $Exe -hide_banner -version 2>&1
    } catch {
        $info.Detail = ('ffmpeg -version の実行に失敗: {0}' -f $_.Exception.Message)
        return $info
    }
    $text = ($output | Out-String)
    $versionMatch = [regex]::Match($text, 'ffmpeg version\s+(\S+)')
    if ($versionMatch.Success) { $info.Version = $versionMatch.Groups[1].Value }
    $info.IsGpl = ($text -match '--enable-gpl')
    $info.IsNonFree = ($text -match '--enable-nonfree')
    $info.IsVersion3 = ($text -match '--enable-version3')
    $avcodecMatch = [regex]::Match($text, 'libavcodec\s+(\d+)\.')
    if ($avcodecMatch.Success) { $info.AvcodecMajor = [int]$avcodecMatch.Groups[1].Value }
    $license = 'LGPL'
    if ($info.IsGpl) { $license = 'GPL' }
    if ($info.IsNonFree) { $license = 'GPL + nonfree（再配布不可）' }
    $info.Detail = ('{0} / {1} ビルド / libavcodec {2}' -f $info.Version, $license, $info.AvcodecMajor)
    return $info
}

function Get-BinSonameMajor {
    param([string]$Prefix)
    $found = Get-ChildItem -LiteralPath $script:BinDir -Filter ($Prefix + '-*.dll') -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found -and $found.Name -match ([regex]::Escape($Prefix) + '-(\d+)\.dll')) { return [int]$Matches[1] }
    return 0
}

function Step-Ffmpeg {
    param([int]$Index, [int]$Total)

    Write-Step -Index $Index -Total $Total -Title 'ffmpeg（LGPL ビルド）を確認する'

    $ffmpegExe = Join-Path $script:BinDir 'ffmpeg.exe'
    $ffprobeExe = Join-Path $script:BinDir 'ffprobe.exe'
    $info = Get-FfmpegInfo -Exe $ffmpegExe

    if (-not $info.Present) {
        Write-Fail 'ffmpeg.exe がありません（動画の読み込みに必須です）。'
        Add-Check -Group 'ffmpeg' -Name 'ffmpeg.exe' -Ok $false -Detail '未検出' `
            -Hint 'tools\video-recovery-runtime\bin に置いてください' -Url $script:FfmpegBuildsUrl
        return $false
    }

    Write-Ok ('ffmpeg: {0}' -f $info.Detail)

    $avcodecMajor = Get-BinSonameMajor -Prefix 'avcodec'
    $avformatMajor = Get-BinSonameMajor -Prefix 'avformat'
    $avutilMajor = Get-BinSonameMajor -Prefix 'avutil'
    Write-Detail ('同梱 DLL: avcodec-{0} / avformat-{1} / avutil-{2}' -f $avcodecMajor, $avformatMajor, $avutilMajor)

    if (-not (Test-Path -LiteralPath $ffprobeExe)) {
        Write-Warn 'ffprobe.exe がありません（動画の長さ取得などで使います）。'
        Add-Check -Group 'ffmpeg' -Name 'ffprobe.exe' -Ok $false -Detail '未検出' `
            -Hint 'tools\video-recovery-runtime\bin に置いてください' -Url $script:FfmpegBuildsUrl
    } else {
        Add-Check -Group 'ffmpeg' -Name 'ffprobe.exe' -Ok $true -Detail (Get-SizeText $ffprobeExe)
    }

    # --- 既に LGPL なら何もしない ---
    if (-not $info.IsGpl -and -not $info.IsNonFree) {
        Write-Ok 'LGPL ビルドです。ライセンス上の追加作業はありません。'
        Add-Check -Group 'ffmpeg' -Name 'ffmpeg のライセンス' -Ok $true -Detail 'LGPL'
        Add-Check -Group 'ffmpeg' -Name 'ffmpeg.exe' -Ok $true -Detail $info.Version
        return $true
    }

    if ($info.IsNonFree) {
        Write-Fail 'nonfree を含むビルドです。再配布できません。'
        Write-Detail 'このビルドは配布用に使えません。LGPL ビルドへ必ず差し替えてください。'
    } else {
        Write-Warn 'GPL ビルドです（gyan.dev の full_build）。このまま配布すると GPL の義務が発生します。'
    }
    Write-Detail 'PPT Lab 本体が使うのは「デコード / 情報取得」だけなので、LGPL 共有ビルドで足ります。'
    Write-Detail '（配布するなら LGPL に差し替えるのが安全です。詳しくは tools\ppt-setup\README.md）'

    Add-Check -Group 'ffmpeg' -Name 'ffmpeg のライセンス' -Ok $false -Detail ('GPL' + $(if ($info.IsVersion3) { ' v3' } else { '' })) `
        -Required $false -Hint 'LGPL 共有ビルドへ差し替えると再配布しやすくなります' -Url $script:FfmpegBuildsUrl

    if ($script:SkipFfmpeg) {
        Write-Info '(-SkipFfmpeg が指定されたので差し替えません)'
        Add-Check -Group 'ffmpeg' -Name 'ffmpeg.exe' -Ok $true -Detail $info.Version
        return $true
    }

    if ($script:DryRun) {
        Write-Dry ('LGPL 共有ビルドを取得して bin を差し替える予定: {0}' -f $script:FfmpegZipUrl)
        Write-Detail '（既存の ffmpeg.exe / av*.dll は tools\ppt-setup\backup に退避します）'
        Add-Check -Group 'ffmpeg' -Name 'ffmpeg.exe' -Ok $true -Detail $info.Version
        return $true
    }

    $replace = Confirm-YesNo -Question 'ffmpeg を LGPL 共有ビルドに差し替えますか？（推奨・約 75 MB ダウンロード）' -Default $true
    if (-not $replace) {
        Write-Warn 'GPL ビルドのまま続けます。配布する場合は GPL の義務（ソース提供など）に注意してください。'
        Add-Check -Group 'ffmpeg' -Name 'ffmpeg.exe' -Ok $true -Detail $info.Version
        return $true
    }

    # --- ダウンロードして一時フォルダーへ展開 ---
    $zipName = Split-Path -Leaf $script:FfmpegZipUrl
    $zipPath = Join-Path $script:DownloadDir $zipName
    if (-not (Invoke-Download -Url $script:FfmpegZipUrl -Destination $zipPath -Label ('ffmpeg LGPL 共有ビルド（{0}）' -f $zipName) -MinBytes 40MB)) {
        Write-Warn 'LGPL ビルドを取得できませんでした。既存の ffmpeg をそのまま使います。'
        Write-Detail ('手動で取得する場合: {0}' -f $script:FfmpegBuildsUrl)
        Write-Detail 'ダウンロードした zip の bin フォルダーの中身を tools\video-recovery-runtime\bin へコピーしてください。'
        Add-Check -Group 'ffmpeg' -Name 'ffmpeg.exe' -Ok $true -Detail $info.Version
        return $true
    }

    $tempRoot = Join-Path $script:DownloadDir 'ffmpeg-lgpl-extract'
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue }
    if (-not (Expand-ZipArchive -ZipPath $zipPath -Destination $tempRoot)) { return $false }

    # --- 展開結果から bin を探す（zip のルート名はビルドによって変わる） ---
    $newFfmpeg = Get-ChildItem -LiteralPath $tempRoot -Recurse -Filter 'ffmpeg.exe' -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $newFfmpeg) {
        Write-Fail 'zip の中に ffmpeg.exe が見つかりませんでした。'
        Add-Check -Group 'ffmpeg' -Name 'ffmpeg.exe' -Ok $true -Detail $info.Version
        return $true
    }
    $newBin = $newFfmpeg.Directory.FullName
    Write-Detail ('新しい ffmpeg: {0}' -f $newFfmpeg.FullName)

    # --- ライセンスを確認（本当に LGPL か） ---
    $newInfo = Get-FfmpegInfo -Exe $newFfmpeg.FullName
    if ($newInfo.IsGpl -or $newInfo.IsNonFree) {
        Write-Fail 'ダウンロードしたビルドが GPL でした。差し替えを中止します（既存を維持）。'
        Add-Check -Group 'ffmpeg' -Name 'ffmpeg.exe' -Ok $true -Detail $info.Version
        return $true
    }
    Write-Ok ('取得したビルド: {0} → LGPL を確認' -f $newInfo.Version)

    # --- soname が一致するか（avcodec-63 が avcodec-64 になると動かない） ---
    $newAvcodec = 0
    $newAvcodecFile = Get-ChildItem -LiteralPath $newBin -Filter 'avcodec-*.dll' -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($newAvcodecFile -and $newAvcodecFile.Name -match 'avcodec-(\d+)\.dll') { $newAvcodec = [int]$Matches[1] }
    if ($newAvcodec -ne 0 -and $avcodecMajor -ne 0 -and $newAvcodec -ne $avcodecMajor) {
        Write-Fail ('avcodec のメジャー版が違います（既存 {0} / 新規 {1}）。差し替えを中止します。' -f $avcodecMajor, $newAvcodec)
        Write-Detail '既存の C++ 実行ファイルは avcodec-63.dll を直接読み込むため、番号が違うと起動できません。'
        Write-Detail ('新しいビルドを探す場合: {0}' -f $script:FfmpegBuildsUrl)
        Add-Check -Group 'ffmpeg' -Name 'ffmpeg.exe' -Ok $true -Detail $info.Version
        return $true
    }
    if ($avcodecMajor -eq 0 -and $newAvcodec -ne 0) {
        Write-Warn ('既存の avcodec DLL が見つかりません。新しいビルドの avcodec-{0}.dll を使います。' -f $newAvcodec)
    }

    # --- 退避してからコピー ---
    $tag = 'ffmpeg-{0}' -f (Get-Date).ToString('yyyyMMdd-HHmmss')
    foreach ($name in @('ffmpeg.exe', 'ffprobe.exe')) {
        $path = Join-Path $script:BinDir $name
        if (Test-Path -LiteralPath $path) { Backup-BinFile -Path $path -Tag $tag }
    }
    foreach ($dll in (Get-ChildItem -LiteralPath $script:BinDir -Filter 'av*.dll' -File -ErrorAction SilentlyContinue)) {
        Backup-BinFile -Path $dll.FullName -Tag $tag
    }

    $copiedCount = 0
    foreach ($file in (Get-ChildItem -LiteralPath $newBin -File -ErrorAction SilentlyContinue)) {
        $ext = $file.Extension.ToLowerInvariant()
        if ($ext -ne '.exe' -and $ext -ne '.dll') { continue }
        if ($file.Name -notmatch '^(ffmpeg|ffprobe|av|sw)') { continue }
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $script:BinDir $file.Name) -Force
        $copiedCount++
    }
    Write-Ok ('ffmpeg を LGPL 共有ビルドに入れ替えました（{0} ファイル）' -f $copiedCount)
    Add-Action ('ffmpeg を LGPL 共有ビルド（{0}）へ差し替え' -f $newInfo.Version)

    # --- ライセンス文を残す（再配布時の同梱物） ---
    New-Directory -Path $script:LicenseDir | Out-Null
    $licenseSource = Get-ChildItem -LiteralPath $tempRoot -Recurse -Filter 'LICENSE.txt' -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($licenseSource) {
        Copy-Item -LiteralPath $licenseSource.FullName -Destination (Join-Path $script:LicenseDir 'FFmpeg-LICENSE.txt') -Force
        Write-Detail ('LGPL のライセンス文を保存: {0}' -f (Join-Path $script:LicenseDir 'FFmpeg-LICENSE.txt'))
    }
    $notice = @(
        'このフォルダーの ffmpeg（共有ライブラリ版）は LGPL v2.1 以降で提供されています。',
        '取得元: ' + $script:FfmpegZipUrl,
        'ビルド: ' + $newInfo.Version + ' (BtbN/FFmpeg-Builds)',
        'ソースコード: https://github.com/FFmpeg/FFmpeg',
        'ビルドスクリプト: https://github.com/BtbN/FFmpeg-Builds',
        '',
        '再配布するときは、このライセンス文と入手元 URL を一緒に配ってください。',
        'LGPL は「動的リンクのまま配布する」ことが条件なので、DLL を同梱して exe から読み込む形は問題ありません。'
    ) -join "`r`n"
    Set-Content -LiteralPath (Join-Path $script:LicenseDir 'FFmpeg-LGPL-NOTICE.txt') -Value $notice -Encoding UTF8
    Write-Detail 'LGPL の案内文を保存: licenses\FFmpeg-LGPL-NOTICE.txt'

    $afterInfo = Get-FfmpegInfo -Exe (Join-Path $script:BinDir 'ffmpeg.exe')
    Add-Check -Group 'ffmpeg' -Name 'ffmpeg.exe' -Ok $afterInfo.Present -Detail $afterInfo.Detail
    Add-Check -Group 'ffmpeg' -Name 'ffmpeg のライセンス' -Ok (-not $afterInfo.IsGpl) -Detail 'LGPL'
    return $true
}

# ============================================================================
# [6/8] 最終チェック
# ============================================================================

function Get-RequiredFileSpecs {
    <#
      「配布先で動くために必要なファイル」の一覧。
      ここに 1 行足せばチェックも表示も自動で増える。
    #>
    return @(
        @{ Group = '解析エンジン'; Name = 'TetrisVideoRecovery_stream.exe'; Path = (Join-Path $script:BinDir 'TetrisVideoRecovery_stream.exe'); Required = $true;
           Hint = '配布物に含まれているはずです。別の PC の同じ場所からコピーしてください'; Url = '' },
        @{ Group = '解析エンジン'; Name = 'onnxruntime.dll'; Path = (Join-Path $script:BinDir 'onnxruntime.dll'); Required = $true;
           Hint = 'tools\video-recovery-runtime\bin に置いてください'; Url = '' },
        @{ Group = '解析エンジン'; Name = 'onnxruntime_providers_cuda.dll'; Path = (Join-Path $script:BinDir 'onnxruntime_providers_cuda.dll'); Required = $true;
           Hint = 'tools\video-recovery-runtime\bin に置いてください'; Url = '' },
        @{ Group = '解析エンジン'; Name = 'onnxruntime_providers_shared.dll'; Path = (Join-Path $script:BinDir 'onnxruntime_providers_shared.dll'); Required = $true;
           Hint = 'tools\video-recovery-runtime\bin に置いてください'; Url = '' },
        @{ Group = '解析エンジン'; Name = 'avcodec-*.dll'; Path = (Join-Path $script:BinDir 'avcodec-*.dll'); Required = $true; Wildcard = $true;
           Hint = 'ffmpeg の共有ライブラリです。LGPL 共有ビルドの bin からコピーしてください'; Url = $script:FfmpegBuildsUrl },
        @{ Group = '解析エンジン'; Name = 'avformat-*.dll'; Path = (Join-Path $script:BinDir 'avformat-*.dll'); Required = $true; Wildcard = $true;
           Hint = 'ffmpeg の共有ライブラリです。LGPL 共有ビルドの bin からコピーしてください'; Url = $script:FfmpegBuildsUrl },
        @{ Group = '解析エンジン'; Name = 'avutil-*.dll'; Path = (Join-Path $script:BinDir 'avutil-*.dll'); Required = $true; Wildcard = $true;
           Hint = 'ffmpeg の共有ライブラリです。LGPL 共有ビルドの bin からコピーしてください'; Url = $script:FfmpegBuildsUrl },
        @{ Group = '解析エンジン'; Name = 'ffmpeg.exe'; Path = (Join-Path $script:BinDir 'ffmpeg.exe'); Required = $true;
           Hint = 'tools\video-recovery-runtime\bin に置いてください'; Url = $script:FfmpegBuildsUrl },
        @{ Group = '解析エンジン'; Name = 'ffprobe.exe'; Path = (Join-Path $script:BinDir 'ffprobe.exe'); Required = $true;
           Hint = 'tools\video-recovery-runtime\bin に置いてください'; Url = $script:FfmpegBuildsUrl },
        @{ Group = '解析エンジン'; Name = 'TetrisStudy.exe（任意）'; Path = (Join-Path $script:BinDir 'TetrisStudy.exe'); Required = $false;
           Hint = '学習用の実行ファイルです。無くても解析はできます'; Url = '' },

        @{ Group = 'モデル'; Name = 'fast320\scene-classifier.onnx'; Path = (Join-Path $script:ModelFast 'scene-classifier.onnx'); Required = $true;
           Hint = 'tools\ppt-lab\models\fast320 に置いてください'; Url = '' },
        @{ Group = 'モデル'; Name = 'fast320\tetris.onnx'; Path = (Join-Path $script:ModelFast 'tetris.onnx'); Required = $true;
           Hint = 'tools\ppt-lab\models\fast320 に置いてください'; Url = '' },
        @{ Group = 'モデル'; Name = 'fast320\next_hold.onnx'; Path = (Join-Path $script:ModelFast 'next_hold.onnx'); Required = $true;
           Hint = 'tools\ppt-lab\models\fast320 に置いてください'; Url = '' },
        @{ Group = 'モデル'; Name = 'end3\scene-classifier.onnx'; Path = (Join-Path $script:ModelEnd3 'scene-classifier.onnx'); Required = $true;
           Hint = 'tools\ppt-lab\models\end3 に置いてください'; Url = '' },

        @{ Group = 'アプリ'; Name = 'server.py'; Path = $script:ServerPy; Required = $true;
           Hint = 'tools\ppt-lab\server.py が配布物に入っているか確認してください'; Url = '' },
        @{ Group = 'アプリ'; Name = 'web\index.html'; Path = (Join-Path (Join-Path $script:PptLabRoot 'web') 'index.html'); Required = $true;
           Hint = 'tools\ppt-lab\web フォルダーごとコピーしてください'; Url = '' },
        @{ Group = 'アプリ'; Name = 'config\tetris_recover.ini'; Path = (Join-Path $script:ConfigDir 'tetris_recover.ini'); Required = $true;
           Hint = 'tools\video-recovery-runtime\config に置いてください'; Url = '' },
        @{ Group = 'アプリ'; Name = 'profiles.json（任意）'; Path = (Join-Path $script:PptLabRoot 'profiles.json'); Required = $false;
           Hint = '解析プロファイルの初期値を入れるファイルです'; Url = '' }
    )
}

function Step-Verify {
    param([int]$Index, [int]$Total)

    Write-Step -Index $Index -Total $Total -Title '足りないファイルがないか最終チェックする'

    $specs = Get-RequiredFileSpecs
    foreach ($spec in $specs) {
        $exists = $false
        $detail = ''
        if ($spec.ContainsKey('Wildcard') -and $spec.Wildcard) {
            $found = @(Get-ChildItem -Path $spec.Path -File -ErrorAction SilentlyContinue | Sort-Object Name)
            $exists = ($found.Count -gt 0)
            if ($exists) { $detail = ($found | ForEach-Object { $_.Name }) -join ', ' } else { $detail = '見つかりません' }
        } else {
            $exists = Test-Path -LiteralPath $spec.Path -PathType Leaf
            if ($exists) {
                $detail = Get-SizeText $spec.Path
                $version = Get-FileVersionText $spec.Path
                if (-not [string]::IsNullOrEmpty($version)) { $detail = ('{0} / {1}' -f $detail, $version) }
            } else {
                $detail = '見つかりません'
            }
        }
        Add-Check -Group $spec.Group -Name $spec.Name -Ok $exists -Detail $detail `
            -Required ([bool]$spec.Required) -Hint $spec.Hint -Url $spec.Url
    }

    # cuDNN の分割ライブラリは「1 個以上あるか」で判定する
    $cudnnMajor = 9
    if ($null -ne $script:Cuda) { $cudnnMajor = $script:Cuda.CudnnMajor }
    $split = @(Get-ChildItem -LiteralPath $script:BinDir -Filter ('cudnn*64_{0}.dll' -f $cudnnMajor) -File -ErrorAction SilentlyContinue)
    Add-Check -Group 'CUDA / cuDNN' -Name 'cuDNN のセット（分割ライブラリ込み）' -Ok ($split.Count -ge 1) `
        -Detail $(if ($split.Count -ge 1) { ('{0} 個そろっています' -f $split.Count) } else { '見つかりません' }) `
        -Hint 'cuDNN の wheel（pip 版）から bin へコピーしてください' -Url $script:CudnnDownloadUrl

    # Python の pythonw（黒い画面を出さずに起動するため）
    if ($null -ne $script:Python) {
        $pythonwOk = Test-Path -LiteralPath $script:Python.PythonwPath
        Add-Check -Group 'Python' -Name 'pythonw.exe' -Ok $pythonwOk `
            -Detail $(if ($pythonwOk) { $script:Python.PythonwPath } else { '見つかりません' }) -Required $false `
            -Hint 'python.exe と同じフォルダーに pythonw.exe があるはずです'
    }

    # Visual C++ ランタイム（解析エンジンが依存）
    $vcOk = (Test-Path -LiteralPath (Join-Path $env:SystemRoot 'System32\msvcp140.dll')) -and
            (Test-Path -LiteralPath (Join-Path $env:SystemRoot 'System32\vcruntime140.dll'))
    Add-Check -Group '環境' -Name 'Visual C++ ランタイム' -Ok $vcOk -Required $false `
        -Detail $(if ($vcOk) { 'あり' } else { '見つかりません（起動時にエラーになることがあります）' }) `
        -Hint 'Microsoft の VC++ 再頒布可能パッケージを入れてください' -Url $script:VcRedistUrl

    Show-CheckTable
    return (Show-CheckCounts)
}

function Get-DisplayWidth {
    <#
      コンソール上の「見た目の幅」を数える。日本語や ✅ は 2 文字分として扱う。
      これで一覧の列がそろう。
    #>
    param([string]$Text)
    if ([string]::IsNullOrEmpty($Text)) { return 0 }
    $width = 0
    foreach ($character in $Text.ToCharArray()) {
        $code = [int]$character
        $isWide = $false
        if (($code -ge 0x1100 -and $code -le 0x115F) -or
            ($code -ge 0x2600 -and $code -le 0x27BF) -or
            ($code -ge 0x2E80 -and $code -le 0xA4CF) -or
            ($code -ge 0xAC00 -and $code -le 0xD7A3) -or
            ($code -ge 0xF900 -and $code -le 0xFAFF) -or
            ($code -ge 0xFE30 -and $code -le 0xFE6F) -or
            ($code -ge 0xFF00 -and $code -le 0xFF60) -or
            ($code -ge 0xFFE0 -and $code -le 0xFFE6)) { $isWide = $true }
        if ($isWide) { $width += 2 } else { $width += 1 }
    }
    return $width
}

function Show-CheckTable {
    <#
      ✅ / ❌ の一覧をグループごとに表示する。
      コンソールが UTF-8 でない場合は [OK] / [NG] に自動で切り替わる。
    #>
    $groups = $script:Checks | Group-Object -Property Group
    foreach ($group in $groups) {
        Write-Host ''
        Write-LogLine -Level 'CHECK' -Message ('  ■ {0}' -f $group.Name) -Color White
        foreach ($entry in $group.Group) {
            $mark = $script:MarkOk
            $color = 'Green'
            if (-not $entry.Ok) {
                $mark = $script:MarkNg
                if ($entry.Required) { $color = 'Red' } else { $color = 'Yellow' }
            }
            $name = [string]$entry.Name
            $padding = 40 - (Get-DisplayWidth $name)
            if ($padding -lt 1) { $padding = 1 }
            $detail = [string]$entry.Detail
            if ([string]::IsNullOrWhiteSpace($detail)) { $detail = '' }
            $line = '    {0} {1}{2}{3}' -f $mark, $name, (' ' * $padding), $detail
            Write-LogLine -Level 'CHECK' -Message $line -Color $color
        }
    }
}

function Show-CheckCounts {
    <#
      件数を数えて表示し、足りない必須項目の数を返す。
    #>
    $total = @($script:Checks).Count
    $ok = @($script:Checks | Where-Object { $_.Ok }).Count
    $missingRequired = @($script:Checks | Where-Object { -not $_.Ok -and $_.Required })

    Write-Host ''
    if ($missingRequired.Count -eq 0) {
        Write-LogLine -Level 'CHECK' -Message ('  合計 {0}/{1} そろっています。' -f $ok, $total) -Color Green
    } else {
        Write-LogLine -Level 'CHECK' -Message ('  合計 {0}/{1}。うち必須が {2} 件足りません。' -f $ok, $total, $missingRequired.Count) -Color Yellow
    }
    return $missingRequired.Count
}

function Show-MissingAdvice {
    <#
      足りない必須項目について「何を・どこに・どこから取るか」を日本語で出す。
      ここが「失敗しても次に何をすればいいか分かる」の要。
    #>
    $missing = @($script:Checks | Where-Object { -not $_.Ok -and $_.Required })
    if ($missing.Count -eq 0) {
        Write-Host ''
        Write-LogLine -Level 'NEXT' -Message '  必須のファイルはすべてそろっています。' -Color Green
        return
    }
    Write-Host ''
    Write-LogLine -Level 'NEXT' -Message ('  この後やること（{0} 件）' -f $missing.Count) -Color Yellow
    $index = 0
    foreach ($item in $missing) {
        $index++
        Write-LogLine -Level 'NEXT' -Message ('   {0}. {1}' -f $index, $item.Name) -Color Yellow
        if (-not [string]::IsNullOrWhiteSpace($item.Detail)) {
            Write-Host ('      いまの状態: {0}' -f $item.Detail) -ForegroundColor DarkGray
        }
        if (-not [string]::IsNullOrWhiteSpace($item.Hint)) {
            Write-Host ('      対処: {0}' -f $item.Hint) -ForegroundColor Gray
        }
        if (-not [string]::IsNullOrWhiteSpace($item.Url)) {
            Write-Host ('      入手先: {0}' -f $item.Url) -ForegroundColor Gray
        }
        if ($script:LogReady) {
            Add-Content -LiteralPath $script:LogPath -Value ('[NEXT] {0}. {1} / {2} / {3}' -f $index, $item.Name, $item.Hint, $item.Url) -Encoding UTF8
        }
    }
}

function Show-DownloadPlan {
    <#
      DryRun / 診断モードのときに「実行すると何をダウンロードするか」を一覧にする。
      配布前に URL・サイズ・ライセンスをまとめて確認できるようにするため。
      （実際の取得は、利用者の PC 上でインストール時に行う）
    #>
    $ffmpegExe = Join-Path $script:BinDir 'ffmpeg.exe'
    $ffmpegInfo = $null
    if (Test-Path -LiteralPath $ffmpegExe) { $ffmpegInfo = Get-FfmpegInfo -Exe $ffmpegExe }

    $pythonNeeded = ($null -eq $script:Python)

    $cudaNeeded = $false
    if ($null -ne $script:Cuda) {
        foreach ($name in @($script:Cuda.CublasName, $script:Cuda.CublasLtName, $script:Cuda.CudartName, $script:Cuda.CudnnName)) {
            if ([string]::IsNullOrEmpty($name)) { continue }
            if (Test-Path -LiteralPath (Join-Path $script:BinDir $name)) { continue }
            # bin に無くてもシステム側からコピーできるなら、ダウンロードは不要
            if (-not [string]::IsNullOrEmpty((Find-DllSource -Name $name))) { continue }
            $cudaNeeded = $true
        }
        if ($script:Cuda.CudaMajor -gt 0) {
            $cudaRange = ('>={0},<{1}' -f $script:Cuda.CudaMajor, ($script:Cuda.CudaMajor + 1))
        } else {
            $cudaRange = '>=13,<14'
        }
    } else {
        $cudaRange = '>=13,<14'
    }

    $ffmpegNeeded = $false
    if ($null -ne $ffmpegInfo) {
        $ffmpegNeeded = (($ffmpegInfo.IsGpl -or $ffmpegInfo.IsNonFree) -and (-not $script:SkipFfmpeg))
    }

    $rows = New-Object System.Collections.ArrayList
    if ($pythonNeeded) {
        [void]$rows.Add([pscustomobject]@{ Kind = '必要'; Name = ('Python {0} 公式インストーラ（64bit）' -f $script:PythonVersion); Url = $script:PythonInstallerUrl; Size = '約 26 MB'; License = 'PSF License' })
        [void]$rows.Add([pscustomobject]@{ Kind = '条件付'; Name = ('Python {0} embeddable zip（方法 B を選んだときだけ）' -f $script:PythonVersion); Url = $script:PythonEmbedUrl; Size = '約 11 MB'; License = 'PSF License' })
        [void]$rows.Add([pscustomobject]@{ Kind = '条件付'; Name = 'get-pip.py（embeddable を選んだときだけ）'; Url = $script:GetPipUrl; Size = '約 2 MB'; License = 'MIT' })
    }
    if ($cudaNeeded) {
        [void]$rows.Add([pscustomobject]@{ Kind = '必要'; Name = ('cuBLAS（pip: nvidia-cublas{0}）' -f $cudaRange); Url = 'https://pypi.org/project/nvidia-cublas/'; Size = '約 420 MB'; License = 'NVIDIA CUDA EULA' })
        [void]$rows.Add([pscustomobject]@{ Kind = '必要'; Name = ('CUDA Runtime（pip: nvidia-cuda-runtime{0}）' -f $cudaRange); Url = 'https://pypi.org/project/nvidia-cuda-runtime/'; Size = '約 5 MB'; License = 'NVIDIA CUDA EULA' })
        if ($null -ne $script:Cuda) {
            $cudnnUrl = ('https://pypi.org/project/nvidia-cudnn-cu{0}/' -f $script:Cuda.CudaMajor)
            [void]$rows.Add([pscustomobject]@{ Kind = '必要'; Name = ('cuDNN（pip: nvidia-cudnn-cu{0}）' -f $script:Cuda.CudaMajor); Url = $cudnnUrl; Size = '約 400〜600 MB'; License = 'NVIDIA cuDNN SLA' })
        }
    }
    if ($ffmpegNeeded) {
        [void]$rows.Add([pscustomobject]@{ Kind = '必要'; Name = 'FFmpeg LGPL 共有ビルド（ffmpeg / ffprobe / av*.dll）'; Url = $script:FfmpegZipUrl; Size = '約 77 MiB'; License = 'LGPL v2.1 以降' })
    }

    Write-Host ''
    Write-LogLine -Level 'PLAN' -Message ('  ' + ('-' * 66)) -Color DarkCyan
    Write-LogLine -Level 'PLAN' -Message '  ダウンロード予定の一覧（配布前の確認用）' -Color Cyan
    Write-LogLine -Level 'PLAN' -Message '  実際の取得は、インストール時に利用者の PC 上で行います。' -Color DarkGray
    Write-LogLine -Level 'PLAN' -Message ('  ' + ('-' * 66)) -Color DarkCyan

    if ($rows.Count -eq 0) {
        Write-LogLine -Level 'PLAN' -Message '  追加のダウンロードはありません（全部そろっています）。' -Color Green
    }
    foreach ($row in $rows) {
        $color = 'Yellow'
        if ($row.Kind -eq '条件付') { $color = 'DarkGray' }
        Write-LogLine -Level 'PLAN' -Message ('  [{0}] {1}（{2} / {3}）' -f $row.Kind, $row.Name, $row.Size, $row.License) -Color $color
        Write-LogLine -Level 'PLAN' -Message ('         {0}' -f $row.Url) -Color DarkGray
    }
    Write-LogLine -Level 'PLAN' -Message '  ※ NVIDIA の部品は再配布せず、利用者の PC で取得する方式にしています。' -Color DarkGray
    Write-LogLine -Level 'PLAN' -Message '  ※ cuFFT（cufft64_12.dll）は配布物の bin に同梱されているため取得しません。' -Color DarkGray
    Write-LogLine -Level 'PLAN' -Message ('  ' + ('-' * 66)) -Color DarkCyan
}

# ============================================================================
# [7/8] launch.cmd とショートカット
# ============================================================================

function Get-LaunchCmdContent {
    <#
      launch.cmd のテンプレート。
      cmd.exe は .cmd を OEM コードページ（日本語環境なら CP932）で読み、
      UTF-8 BOM 付きだと 1 行目を誤解釈する。そのため
      「BOM 無し・ASCII のみ」で書き出す。日本語のメッセージは install.ps1 側で出す。
    #>
    param([int]$Port)
    $template = @'
@echo off
setlocal EnableExtensions
rem ================================================================
rem  PPT Lab launcher (generated by tools\ppt-setup\install.ps1).
rem  Double-click: starts the server, then opens the browser.
rem  Change the port with an argument:  launch.cmd 9000
rem  Keep this file ASCII only (cmd.exe uses the OEM code page) and
rem  do not add a UTF-8 BOM (it breaks the first line).
rem ================================================================
set "HERE=%~dp0"
for %%I in ("%HERE%..\..") do set "ROOT=%%~fI"
set "PORT=__PORT__"
if not "%~1"=="" set "PORT=%~1"
set "URL=http://127.0.0.1:%PORT%/ppt-lab/"
set "HEALTH=http://127.0.0.1:%PORT%/api/health"
set "SERVER=%ROOT%\tools\ppt-lab\server.py"
set "PYW="
set "EDGE86=%ProgramFiles(x86)%"
set "EDGE64=%ProgramFiles%"

rem --- 1) Python bundled by the installer (embeddable zip) ---
if exist "%HERE%python\pythonw.exe" set "PYW=%HERE%python\pythonw.exe"
if defined PYW goto :run

rem --- 2) py launcher (prefer 3.11) ---
where py >nul 2>&1
if errorlevel 1 goto :known_path
for /f "usebackq delims=" %%P in (`py -3.11 -c "import sys;print(sys.executable)" 2^>nul`) do call :swap "%%P"
if defined PYW goto :run
for /f "usebackq delims=" %%P in (`py -3 -c "import sys;print(sys.executable)" 2^>nul`) do call :swap "%%P"
if defined PYW goto :run

:known_path
rem --- 3) well-known install locations ---
for %%D in ("%LOCALAPPDATA%\Programs\Python\Python314" "%LOCALAPPDATA%\Programs\Python\Python313" "%LOCALAPPDATA%\Programs\Python\Python312" "%LOCALAPPDATA%\Programs\Python\Python311") do (
  if exist "%%~fD\pythonw.exe" if not defined PYW set "PYW=%%~fD\pythonw.exe"
)
if defined PYW goto :run

rem --- 4) pythonw on PATH ---
for /f "delims=" %%P in ('where pythonw 2^>nul') do if not defined PYW set "PYW=%%P"
if defined PYW goto :run

echo.
echo  [PPT Lab] Python was not found.
echo  Please run tools\ppt-setup\install.cmd once.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%install.ps1" -Diagnose
pause
exit /b 1

:swap
rem Build a pythonw.exe path (no console window).
set "PYW=%~1"
set "PYW=%PYW:python.exe=pythonw.exe%"
if not exist "%PYW%" set "PYW="
goto :eof

:run
if not exist "%SERVER%" goto :no_server

rem Do not start a second server if one is already running.
powershell -NoProfile -Command "try{ if((Invoke-RestMethod '%HEALTH%' -TimeoutSec 2).ok){ exit 0 } else { exit 1 } }catch{ exit 1 }" >nul 2>&1
if not errorlevel 1 goto :open

start "" "%PYW%" "%SERVER%" --port %PORT%
rem Wait for the server (max 15 seconds).
powershell -NoProfile -Command "for($i=0;$i -lt 60;$i++){ try{ if((Invoke-RestMethod '%HEALTH%' -TimeoutSec 2).ok){ exit 0 } }catch{}; Start-Sleep -Milliseconds 250 }; exit 1" >nul 2>&1
if errorlevel 1 goto :fail

:open
if defined EDGE86 if exist "%EDGE86%\Microsoft\Edge\Application\msedge.exe" goto :open_edge86
if exist "%EDGE64%\Microsoft\Edge\Application\msedge.exe" goto :open_edge64
start "" "%URL%"
exit /b 0

:open_edge86
start "" "%EDGE86%\Microsoft\Edge\Application\msedge.exe" --app=%URL% --start-maximized --no-first-run
exit /b 0

:open_edge64
start "" "%EDGE64%\Microsoft\Edge\Application\msedge.exe" --app=%URL% --start-maximized --no-first-run
exit /b 0

:no_server
echo.
echo  [PPT Lab] server.py was not found:
echo  %SERVER%
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%install.ps1" -Diagnose
pause
exit /b 1

:fail
echo.
echo  [PPT Lab] Could not start the server (port %PORT%).
echo  Running the diagnostic tool...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%install.ps1" -Diagnose
pause
exit /b 1
'@
    return $template.Replace('__PORT__', [string]$Port)
}

function Write-TextUtf8NoBom {
    param(
        [string]$Path,
        [string]$Content
    )
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

function New-ShortcutFile {
    <#
      .lnk を作る（既にあれば上書き＝冪等）。
      WScript.Shell は PS 5.1 / 7 のどちらでも使える。
    #>
    param(
        [string]$Path,
        [string]$TargetPath,
        [string]$Arguments = '',
        [string]$WorkingDirectory = '',
        [string]$IconLocation = '',
        [int]$WindowStyle = 1,
        [string]$Description = ''
    )
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    $shell = New-Object -ComObject WScript.Shell
    try {
        $shortcut = $shell.CreateShortcut($Path)
        $shortcut.TargetPath = $TargetPath
        if (-not [string]::IsNullOrEmpty($Arguments)) { $shortcut.Arguments = $Arguments }
        if (-not [string]::IsNullOrEmpty($WorkingDirectory)) { $shortcut.WorkingDirectory = $WorkingDirectory }
        if (-not [string]::IsNullOrEmpty($IconLocation)) { $shortcut.IconLocation = $IconLocation }
        if (-not [string]::IsNullOrEmpty($Description)) { $shortcut.Description = $Description }
        $shortcut.WindowStyle = $WindowStyle
        $shortcut.Save()
    } finally {
        [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($shell)
    }
}

function Step-Launchers {
    param([int]$Index, [int]$Total)

    Write-Step -Index $Index -Total $Total -Title 'ショートカットと起動用ファイルを用意する'

    # --- launch.cmd を生成（このスクリプトが常に最新にする）---
    if ($script:DryRun) {
        Write-Dry ('launch.cmd を生成予定: {0} (port {1})' -f $script:LaunchCmd, $script:Port)
        Add-Check -Group '起動' -Name 'launch.cmd' -Ok $true -Detail 'DryRun のため未生成' -Required $false
    } else {
        try {
            $content = Get-LaunchCmdContent -Port $script:Port
            Write-TextUtf8NoBom -Path $script:LaunchCmd -Content $content
            Write-Ok ('起動用ファイルを用意しました: {0}' -f $script:LaunchCmd)
            Add-Check -Group '起動' -Name 'launch.cmd' -Ok $true -Detail $script:LaunchCmd
        } catch {
            Write-Fail ('launch.cmd を作れませんでした: {0}' -f $_.Exception.Message)
            Add-Check -Group '起動' -Name 'launch.cmd' -Ok $false -Detail '作成に失敗' `
                -Hint 'tools\ppt-setup フォルダーへの書き込み権限を確認してください'
        }
    }

    # --- ポートの使用状況を確認しておく（起動できない原因の 1 位）---
    try {
        $listeners = @(Get-NetTCPConnection -LocalPort $script:Port -State Listen -ErrorAction SilentlyContinue)
        if ($listeners.Count -gt 0) {
            $isPptLab = $false
            try {
                $health = Invoke-RestMethod ('http://127.0.0.1:{0}/api/health' -f $script:Port) -TimeoutSec 2
                $isPptLab = ($health.ok -eq $true)
            } catch { $isPptLab = $false }
            if ($isPptLab) {
                Write-Info ('ポート {0} では既に PPT Lab が動いています（そのまま使えます）。' -f $script:Port)
            } else {
                Write-Warn ('ポート {0} は別のアプリが使っています。PPT Lab が起動できない可能性があります。' -f $script:Port)
                Write-Detail ('launch.cmd の引数で別のポートを指定できます: launch.cmd 8766')
            }
        } else {
            Write-Detail ('ポート {0} は空いています。' -f $script:Port)
        }
    } catch {
        Write-Detail 'ポートの使用状況を確認できませんでした（気にしなくて大丈夫です）'
    }

    if ($script:NoShortcuts) {
        Write-Info '(-NoShortcuts が指定されたのでショートカットは作りません)'
        return $true
    }
    if ($script:DryRun) {
        Write-Dry 'デスクトップとスタートメニューに「PPT Lab」のショートカットを作成予定'
        Add-Check -Group '起動' -Name 'ショートカット' -Ok $true -Detail 'DryRun のため未作成' -Required $false
        return $true
    }

    $iconPath = Join-Path $script:PptLabRoot 'icon.ico'
    if (-not (Test-Path -LiteralPath $iconPath)) { $iconPath = '' }

    $desktop = [Environment]::GetFolderPath('Desktop')
    $startMenu = [Environment]::GetFolderPath('Programs')
    $shortcutTargets = @(
        @{ Path = (Join-Path $desktop 'PPT Lab.lnk'); Name = 'デスクトップ' },
        @{ Path = (Join-Path (Join-Path $startMenu 'PPT Lab') 'PPT Lab.lnk'); Name = 'スタートメニュー' }
    )

    $created = 0
    foreach ($target in $shortcutTargets) {
        try {
            $existed = Test-Path -LiteralPath $target.Path
            New-ShortcutFile -Path $target.Path -TargetPath $script:LaunchCmd `
                -WorkingDirectory $script:SetupRoot -IconLocation $(if ($iconPath) { '{0},0' -f $iconPath } else { '' }) `
                -WindowStyle 7 -Description 'PPT Lab（ぷよテト動画解析ラボ）を起動します'
            $verb = '作成'
            if ($existed) { $verb = '更新' }
            Write-Ok ('{0}ショートカットを{1}: {2}' -f $target.Name, $verb, $target.Path)
            if (-not $existed) { Add-Action ('{0}ショートカットを作成' -f $target.Name) }
            $created++
        } catch {
            Write-Warn ('{0}ショートカットを作れませんでした: {1}' -f $target.Name, $_.Exception.Message)
            Write-Detail ('手動で作る場合: 「{0}」のショートカットを作り、リンク先を {1} にしてください' -f $target.Name, $script:LaunchCmd)
        }
    }

    # スタートメニューに「削除」も置く（初心者が迷わないように）
    $uninstallScript = Join-Path $script:SetupRoot 'uninstall.ps1'
    if (Test-Path -LiteralPath $uninstallScript) {
        try {
            $uninstallLink = Join-Path (Join-Path $startMenu 'PPT Lab') 'PPT Lab を削除.lnk'
            New-ShortcutFile -Path $uninstallLink -TargetPath 'powershell.exe' `
                -Arguments ('-NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $uninstallScript) `
                -WorkingDirectory $script:SetupRoot -IconLocation $(if ($iconPath) { '{0},0' -f $iconPath } else { '' }) `
                -WindowStyle 1 -Description 'PPT Lab のショートカット・設定・データを削除します'
            Write-Detail ('スタートメニューに削除用ショートカットを置きました: {0}' -f $uninstallLink)
        } catch {
            Write-Detail '削除用ショートカットは作りませんでした（uninstall.ps1 を直接実行できます）'
        }
    }

    if ($created -gt 0) {
        Add-Check -Group '起動' -Name 'ショートカット' -Ok $true -Detail ('{0} 個' -f $created)
    } else {
        Add-Check -Group '起動' -Name 'ショートカット' -Ok $false -Detail '作成できませんでした' -Required $false `
            -Hint ('手動でショートカットを作り、リンク先を {0} にしてください' -f $script:LaunchCmd)
    }
    return $true
}

# ============================================================================
# [8/8] 任意: RAM ディスク
# ============================================================================

function Get-RamDiskTask {
    try {
        return (Get-ScheduledTask -TaskName 'PPT RAM disk' -ErrorAction Stop)
    } catch {
        return $null
    }
}

function Step-RamDisk {
    param([int]$Index, [int]$Total)

    Write-Step -Index $Index -Total $Total -Title 'RAM ディスク（任意）'

    $task = Get-RamDiskTask
    if ($null -ne $task) {
        Write-Ok 'スケジュールタスク「PPT RAM disk」は既に登録されています。'
        Add-Check -Group 'RAM ディスク' -Name 'スケジュールタスク' -Ok $true -Detail '登録済み' -Required $false
        return $true
    }

    if ($script:NoRamDisk) {
        Write-Info '(-NoRamDisk が指定されたので確認しません)'
        Add-Check -Group 'RAM ディスク' -Name 'スケジュールタスク' -Ok $true -Detail '未登録（任意）' -Required $false
        return $true
    }

    Write-Info 'RAM ディスク（R:）を使うと、動画の書き出しが速くなり SSD を痛めにくくなります。'
    Write-Detail 'ただし RAM を 4 GB 使うので、メモリが 16 GB 未満の PC にはおすすめしません。'
    Write-Detail 'あとからでも登録できます（tools\ppt-ramdisk-install.ps1 を管理者で実行）。'

    if ($script:DryRun) {
        Write-Dry 'ここで「RAM ディスクのタスクを登録しますか？」と確認します（既定: いいえ）'
        Add-Check -Group 'RAM ディスク' -Name 'スケジュールタスク' -Ok $true -Detail 'DryRun（任意・既定はしない）' -Required $false
        return $true
    }

    $wanted = $script:WantRamDisk
    if (-not $wanted) {
        $wanted = Confirm-YesNo -Question 'RAM ディスク（R: 4GB）のタスクを登録しますか？' -Default $false
    }
    if (-not $wanted) {
        Write-Info 'RAM ディスクは登録しません（既定どおり）。あとで tools\ppt-ramdisk-install.ps1 から登録できます。'
        Add-Check -Group 'RAM ディスク' -Name 'スケジュールタスク' -Ok $true -Detail '未登録（任意）' -Required $false
        return $true
    }

    $imdisk = Join-Path $env:SystemRoot 'System32\imdisk.exe'
    if (-not (Test-Path -LiteralPath $imdisk)) {
        Write-Fail 'ImDisk が見つかりません（RAM ディスクの作成に必要です）。'
        Write-Detail ('入手先: {0}' -f $script:ImDiskUrl)
        Write-Detail 'インストール後、この install.cmd をもう一度実行してください。'
        Add-Check -Group 'RAM ディスク' -Name 'ImDisk' -Ok $false -Detail '未導入' -Required $false `
            -Hint 'ImDisk Toolkit を入れてください' -Url $script:ImDiskUrl
        return $true
    }
    if (-not $script:IsAdmin) {
        Write-Fail 'RAM ディスクのタスク登録には管理者権限が必要です。'
        Write-Detail 'install.cmd を右クリック →「管理者として実行」でやり直してください。'
        Add-Check -Group 'RAM ディスク' -Name 'スケジュールタスク' -Ok $false -Detail '管理者権限が必要' -Required $false `
            -Hint '管理者として install.cmd を再実行してください'
        return $true
    }
    $installScript = Join-Path $script:ToolRoot 'ppt-ramdisk-install.ps1'
    if (-not (Test-Path -LiteralPath $installScript)) {
        Write-Fail ('RAM ディスク用のスクリプトがありません: {0}' -f $installScript)
        Add-Check -Group 'RAM ディスク' -Name 'スケジュールタスク' -Ok $false -Detail 'スクリプトが無い' -Required $false
        return $true
    }

    Write-Info 'RAM ディスクを作成し、ログオン時の自動再作成タスクを登録します…'
    try {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $installScript -SizeMB 4096 -DriveLetter R 2>&1 |
            ForEach-Object { Write-Detail ([string]$_) }
        if ($LASTEXITCODE -ne 0) { throw ('終了コード {0}' -f $LASTEXITCODE) }
        Write-Ok 'RAM ディスクのタスクを登録しました（R:）'
        Add-Action 'RAM ディスク（R: 4GB）のタスクを登録'
        Add-Check -Group 'RAM ディスク' -Name 'スケジュールタスク' -Ok $true -Detail '登録済み'
    } catch {
        Write-Fail ('RAM ディスクの登録に失敗しました: {0}' -f $_.Exception.Message)
        Write-Detail ('手動で: 管理者の PowerShell で {0} を実行してください' -f $installScript)
        Add-Check -Group 'RAM ディスク' -Name 'スケジュールタスク' -Ok $false -Detail '登録に失敗' -Required $false `
            -Hint ('管理者の PowerShell で {0} を実行してください' -f $installScript)
    }
    return $true
}

# ============================================================================
# 状態ファイル（アンインストールやサポートで使う）
# ============================================================================

function Save-InstallState {
    if ($script:DryRun) {
        Write-Dry ('インストール結果を記録予定: {0}' -f $script:StatePath)
        return
    }
    $state = [pscustomobject]@{
        installerVersion = $script:InstallerVersion
        installedAt      = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
        repoRoot         = $script:RepoRoot
        port             = $script:Port
        isAdmin          = $script:IsAdmin
        pythonPath       = $(if ($null -ne $script:Python) { $script:Python.Path } else { '' })
        pythonVersion    = $(if ($null -ne $script:Python) { $script:Python.Version } else { '' })
        pythonwPath      = $(if ($null -ne $script:Python) { $script:Python.PythonwPath } else { '' })
        cudaMajor        = $(if ($null -ne $script:Cuda) { $script:Cuda.CudaMajor } else { 0 })
        cudnnMajor       = $(if ($null -ne $script:Cuda) { $script:Cuda.CudnnMajor } else { 0 })
        ffmpegLicense    = $(if (Test-Path -LiteralPath (Join-Path $script:BinDir 'ffmpeg.exe')) { (Get-FfmpegInfo -Exe (Join-Path $script:BinDir 'ffmpeg.exe')).Detail } else { '' })
        launchCmd        = $script:LaunchCmd
        checksOk         = @($script:Checks | Where-Object { $_.Ok }).Count
        checksTotal      = @($script:Checks).Count
    }
    try {
        $json = $state | ConvertTo-Json -Depth 5
        $encoding = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($script:StatePath, $json, $encoding)
        Write-Detail ('インストール結果を記録: {0}' -f $script:StatePath)
    } catch {
        Write-Warn ('インストール結果を記録できませんでした: {0}' -f $_.Exception.Message)
    }
}

# ============================================================================
# 表示（バナー / まとめ）
# ============================================================================

function Write-Banner {
    $rule = '=' * 70
    Write-Host ''
    Write-LogLine -Level 'BANNER' -Message $rule -Color DarkCyan
    Write-LogLine -Level 'BANNER' -Message ('  PPT Lab（ぷよテト動画解析ラボ）セットアップ  v{0}' -f $script:InstallerVersion) -Color Cyan
    Write-LogLine -Level 'BANNER' -Message $rule -Color DarkCyan

    $mode = '通常インストール'
    if ($script:Diagnose) { $mode = '診断モード（変更しません）' }
    elseif ($script:DryRun) { $mode = 'DryRun（変更しません）' }
    Write-LogLine -Level 'BANNER' -Message ('  モード  : {0}' -f $mode) -Color Gray
    Write-LogLine -Level 'BANNER' -Message ('  対象    : {0}' -f $script:RepoRoot) -Color Gray
    if ($script:LogReady) {
        Write-LogLine -Level 'BANNER' -Message ('  ログ    : {0}' -f $script:LogPath) -Color DarkGray
    }
    if ($script:DryRun) {
        Write-Host ''
        Write-Host '  ※ このモードでは、ダウンロード・インストール・コピー・ショートカット作成を一切行いません。' -ForegroundColor Magenta
        Write-Host '     表示されるのは「いま何があるか」と「実行すると何をするか」だけです。' -ForegroundColor Magenta
    }
    Write-LogLine -Level 'BANNER' -Message $rule -Color DarkCyan
}

function Show-Summary {
    param([int]$MissingRequired)

    $rule = '=' * 70
    Write-Host ''
    Write-LogLine -Level 'SUMMARY' -Message $rule -Color DarkCyan
    Write-LogLine -Level 'SUMMARY' -Message '  結果' -Color Cyan
    Write-LogLine -Level 'SUMMARY' -Message $rule -Color DarkCyan

    $total = @($script:Checks).Count
    $ok = @($script:Checks | Where-Object { $_.Ok }).Count
    Write-LogLine -Level 'SUMMARY' -Message ('  チェック済み: {0} 項目 / そろっている: {1} / 必須の不足: {2}' -f $total, $ok, $MissingRequired) -Color Gray

    if ($script:Actions.Count -gt 0) {
        Write-Host ''
        Write-LogLine -Level 'SUMMARY' -Message ('  この実行で行った変更（{0} 件）' -f $script:Actions.Count) -Color White
        $index = 0
        foreach ($action in $script:Actions) {
            $index++
            Write-LogLine -Level 'SUMMARY' -Message ('   {0}. {1}' -f $index, $action) -Color Gray
        }
    } elseif ($script:DryRun) {
        Write-Host ''
        Write-LogLine -Level 'SUMMARY' -Message '  この実行では何も変更していません（DryRun / 診断モード）。' -Color Magenta
    }

    if ($script:Warnings.Count -gt 0) {
        Write-Host ''
        Write-LogLine -Level 'SUMMARY' -Message ('  注意（{0} 件）' -f $script:Warnings.Count) -Color Yellow
        $index = 0
        foreach ($warning in $script:Warnings) {
            $index++
            if ($index -gt 12) { Write-LogLine -Level 'SUMMARY' -Message '   …（以下はログを参照）' -Color DarkGray; break }
            Write-LogLine -Level 'SUMMARY' -Message ('   - {0}' -f $warning) -Color Yellow
        }
    }

    Show-MissingAdvice

    Write-Host ''
    if ($MissingRequired -eq 0) {
        if ($script:DryRun) {
            Write-LogLine -Level 'SUMMARY' -Message '  必要なものはそろっています。' -Color Green
            Write-LogLine -Level 'SUMMARY' -Message '  このまま install.cmd を（-DryRun を付けずに）実行すると、ショートカットを作って終わります。' -Color Green
        } else {
            Write-LogLine -Level 'SUMMARY' -Message '  準備ができました。デスクトップの「PPT Lab」をダブルクリックして起動してください。' -Color Green
            Write-LogLine -Level 'SUMMARY' -Message ('  画面: http://127.0.0.1:{0}/ppt-lab/' -f $script:Port) -Color Green
        }
    } else {
        Write-LogLine -Level 'SUMMARY' -Message '  上に書いた「この後やること」を済ませてから、もう一度 install.cmd を実行してください。' -Color Yellow
        Write-LogLine -Level 'SUMMARY' -Message '  何度実行しても、既にあるものはそのまま使われます（壊れません）。' -Color Gray
    }
    if ($script:LogReady) {
        Write-LogLine -Level 'SUMMARY' -Message ('  詳しい記録: {0}' -f $script:LogPath) -Color DarkGray
        Write-LogLine -Level 'SUMMARY' -Message '  うまくいかないときは docs\INSTALL.md の「うまくいかないとき」を読んでください。' -Color DarkGray
    }
    Write-LogLine -Level 'SUMMARY' -Message $rule -Color DarkCyan
    Write-Host ''
}

# ============================================================================
# 本体
# ============================================================================

function Invoke-Main {
    param([int]$Total = 8)

    Write-Banner

    if (-not $script:DryRun) {
        Write-Host '  ヒント: 何も変更せずに確認だけしたいときは  install.cmd -DryRun  で実行できます。'
    }

    $environmentOk = $false
    try {
        $environmentOk = Step-Environment -Index 1 -Total $Total
    } catch {
        Write-Fail ('環境の確認でエラーが発生しました: {0}' -f $_.Exception.Message)
    }

    if (-not $environmentOk) {
        Write-Host ''
        Write-Warn 'この PC では前提条件が満たされていないため、インストールを続けられません。'
        $script:Cancelled = $true
        return
    }

    # --- [2] Python ---
    try {
        Step-Python -Index 2 -Total $Total | Out-Null
    } catch {
        Write-Fail ('Python の確認でエラー: {0}' -f $_.Exception.Message)
    }

    # --- [3] GPU（ここで「やめる」を選ばれたら中断） ---
    $gpuOk = $true
    try {
        $gpuOk = Step-Gpu -Index 3 -Total $Total
    } catch {
        Write-Fail ('GPU の確認でエラー: {0}' -f $_.Exception.Message)
    }
    if (-not $gpuOk -or $script:Cancelled) { return }

    # --- [4] CUDA / cuDNN ---
    try {
        Step-Cuda -Index 4 -Total $Total | Out-Null
    } catch {
        Write-Fail ('CUDA / cuDNN の確認でエラー: {0}' -f $_.Exception.Message)
        Write-Detail '手動での導入方法は docs\INSTALL.md の「CUDA / cuDNN が足りない」を参照してください。'
    }

    # --- [5] ffmpeg ---
    try {
        Step-Ffmpeg -Index 5 -Total $Total | Out-Null
    } catch {
        Write-Fail ('ffmpeg の確認でエラー: {0}' -f $_.Exception.Message)
    }

    # --- [6] 最終チェック ---
    $missing = 0
    try {
        $missing = Step-Verify -Index 6 -Total $Total
    } catch {
        Write-Fail ('最終チェックでエラー: {0}' -f $_.Exception.Message)
    }

    # --- [7] ショートカット（診断モードでは作らない） ---
    if ($script:Diagnose) {
        Write-Host ''
        Write-Info '診断モードのため、ショートカットの作成は行いません。'
    } else {
        try {
            Step-Launchers -Index 7 -Total $Total | Out-Null
        } catch {
            Write-Fail ('ショートカットの作成でエラー: {0}' -f $_.Exception.Message)
        }
    }

    # --- [8] RAM ディスク（任意） ---
    if ($script:Diagnose) {
        Write-Host ''
        Write-Info '診断モードのため、RAM ディスクの確認は行いません。'
    } else {
        try {
            Step-RamDisk -Index 8 -Total $Total | Out-Null
        } catch {
            Write-Fail ('RAM ディスクの確認でエラー: {0}' -f $_.Exception.Message)
        }
    }

    Save-InstallState

    # DryRun / 診断モードでは「何をダウンロードする予定か」を一覧で出す
    if ($script:DryRun) { Show-DownloadPlan }

    # ショートカット作成で増えたチェックも含めて数え直す
    $missing = @($script:Checks | Where-Object { -not $_.Ok -and $_.Required }).Count
    $script:MissingRequired = $missing
}

# ---------------------------------------------------------------------------
# ここから実行
# ---------------------------------------------------------------------------

$script:MissingRequired = 0
$script:Cancelled = $false
$exitCode = 0

Start-InstallLog

try {
    Invoke-Main -Total 8
} catch {
    Write-Host ''
    Write-Fail ('予期しないエラーで停止しました: {0}' -f $_.Exception.Message)
    Write-Detail ('発生場所: {0}' -f $_.InvocationInfo.PositionMessage)
    Write-Detail 'お手数ですが、このログを添えて報告してください。'
    $exitCode = 1
} finally {
    $missingRequired = @($script:Checks | Where-Object { -not $_.Ok -and $_.Required }).Count
    Show-Summary -MissingRequired $missingRequired

    if ($exitCode -eq 0) {
        if ($script:Cancelled) { $exitCode = 3 }
        elseif ($missingRequired -gt 0) { $exitCode = 2 }
    }
    if ($script:LogReady) {
        Add-Content -LiteralPath $script:LogPath -Value ('[END] exit={0} missing={1} cancelled={2}' -f $exitCode, $missingRequired, $script:Cancelled) -Encoding UTF8
    }
    Write-Host ('  終了コード: {0}' -f $exitCode) -ForegroundColor DarkGray
    if (-not $script:NonInteractive -and -not $script:DryRun -and $exitCode -ne 0) {
        # ダブルクリック実行だとウィンドウが閉じてしまうので、結果を読む時間を作る
        try { [void](Read-Host '  Enter キーを押すと閉じます') } catch { }
    }
}

exit $exitCode
