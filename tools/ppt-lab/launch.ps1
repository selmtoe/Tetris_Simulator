# PPT Lab ランチャー
# サーバーが動いていなければ起動し、Edge のアプリモードで画面を開く。
param(
    [switch]$NoOpen,
    [int]$Port = 8765
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$url = "http://127.0.0.1:$Port/"

function Show-Message([string]$text) {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [System.Windows.Forms.MessageBox]::Show($text, 'PPT Lab') | Out-Null
    } catch {
        Write-Host $text
    }
}

function Test-Running {
    try {
        $health = Invoke-RestMethod "http://127.0.0.1:$Port/api/health" -TimeoutSec 2
        return $health.ok -eq $true
    } catch {
        return $false
    }
}

# 画面やサーバーのソースが更新されていたら、古いサーバーを止めて起動し直す
function Test-Stale {
    try {
        $health = Invoke-RestMethod "http://127.0.0.1:$Port/api/health" -TimeoutSec 2
    } catch {
        return $false
    }
    if (-not $health.startedAt) { return $false }
    $watched = @(
        (Join-Path $root 'server.py'), (Join-Path $root 'jobs.py'), (Join-Path $root 'namereview.py'),
        (Join-Path $root 'web\app.js'), (Join-Path $root 'web\index.html'),
        (Join-Path $root 'web\theme\ppt-lab.css')
    )
    $newest = ($watched | Where-Object { Test-Path -LiteralPath $_ } |
        ForEach-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } |
        Measure-Object -Maximum).Maximum
    if (-not $newest) { return $false }
    $epoch = New-Object DateTime 1970, 1, 1, 0, 0, 0, ([System.DateTimeKind]::Utc)
    $started = $epoch.AddSeconds([double]$health.startedAt)
    return $started -lt $newest
}

function Stop-Server {
    $listeners = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
    foreach ($listener in $listeners) {
        Stop-Process -Id $listener.OwningProcess -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 500
}

if (Test-Stale) { Stop-Server }

if (-not (Test-Running)) {
    $pythonw = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\pythonw.exe'
    if (-not (Test-Path -LiteralPath $pythonw)) {
        $command = Get-Command python -ErrorAction SilentlyContinue
        $pythonw = if ($command) { $command.Source } else { '' }
    }
    if (-not $pythonw) {
        Show-Message 'Python 3.11 が見つかりません。'
        exit 1
    }
    $server = Join-Path $root 'server.py'
    # パスに空白があるので引数は明示的に引用する
    $arguments = '"' + $server + '" --port ' + $Port
    Start-Process -FilePath $pythonw -ArgumentList $arguments `
        -WorkingDirectory $root -WindowStyle Hidden | Out-Null
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 250
        if (Test-Running) { break }
    }
}

if (-not (Test-Running)) {
    Show-Message 'PPT Lab のサーバーを起動できませんでした。'
    exit 1
}

if (-not $NoOpen) {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe"
    )
    $edge = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if ($edge) {
        Start-Process -FilePath $edge -ArgumentList @(
            "--app=$url", '--start-maximized', '--no-first-run'
        ) | Out-Null
    } else {
        Start-Process $url | Out-Null
    }
}
