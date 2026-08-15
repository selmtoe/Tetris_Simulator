# Tetris Video Analyzer (browser)

`<video>` と Canvas で動画を読み込み、同梱のONNXモデルをONNX Runtime WebのWASM実行プロバイダで推論するブラウザ版です。動画はサーバーへアップロードせず、ブラウザ内だけで処理します。

## 起動

GitHub Pagesのサイトを開き、動画を選択してください。ローカルで確認する場合はHTTPサーバー経由で開きます。

```powershell
powershell -ExecutionPolicy Bypass -File tools/serve.ps1 -OpenPath tetris_video_recovery/web/ -NoBrowser
```

## 対応範囲

- iPad Safariを想定したローカル動画選択
- 16:9中央クロップ
- P1/P2の盤面、HOLD、NEXTの認識
- ONNX Runtime Web（WASM）による200セル一括推論
- 解析サンプルとキュー変化イベントのJSON保存

動画と解析結果は自動送信されません。現在のWindows版 `../src/` にある法的手順のビーム探索や確認用HTMLレポートの全機能ではなく、ブラウザで利用できる認識部分を提供します。既存のC++版はそのまま残してあり、Windowsでの完全な復旧処理は従来どおり `../start.bat` / `../run.ps1` から起動できます。

GitHub Pagesでは、リポジトリの `tetris_video_recovery/web/` を開いてください。ルートのSimulatorは変更していません。
