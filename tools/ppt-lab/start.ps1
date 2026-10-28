# PPT Lab 起動スクリプト（開発用）
# 使い方: 右クリック → PowerShell で実行、または .\start.ps1
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$candidates = @(
    'C:\Users\hirom\AppData\Local\Programs\Python\Python311\python.exe',
    'python'
)
$python = $null
foreach ($candidate in $candidates) {
    if ($candidate -eq 'python') {
        $command = Get-Command python -ErrorAction SilentlyContinue
        if ($command) { $python = $command.Source; break }
    } elseif (Test-Path -LiteralPath $candidate) {
        $python = $candidate; break
    }
}
if (-not $python) {
    Write-Host 'Python 3.11 以降が見つかりません。' -ForegroundColor Red
    Write-Host 'https://www.python.org/downloads/ からインストールしてください。'
    exit 1
}

Write-Host "PPT Lab を起動します: $python" -ForegroundColor Cyan
& $python (Join-Path $root 'server.py') --open @args
