# 起動方法（アプリのウィンドウ / ブラウザのタブ）を Lab の設定から読む。
# launch.cmd から呼ばれる（cmd の for /f で括弧を扱わずに済むよう、別ファイルにしている）。
# 出力: "app" または "tab"。サーバーが居ない・設定が無いときは app。
param([int]$Port = 8765)
$ErrorActionPreference = 'SilentlyContinue'
$mode = ''
try {
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/settings" -TimeoutSec 3
    if ($response.settings) { $mode = [string]$response.settings.launchMode }
} catch { $mode = '' }
if ($mode -eq 'tab') { 'tab' } else { 'app' }