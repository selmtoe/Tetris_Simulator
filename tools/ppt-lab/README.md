# PPT Lab

配布を意識した統合アプリ。Lab / シミュレータ / ビューワー / 配信解析を1つの画面にまとめる。
仕様と現状は `docs\PPT-LAB.md`、エコシステム全体は `docs\ECOSYSTEM.md`。

## 起動

デスクトップまたはスタートメニューの **PPT Lab** をダブルクリック。
（サーバーが動いていなければ裏で起動し、Edge のアプリモードで開く）

コマンドから起動する場合:

```powershell
.\start.ps1            # 画面まで開く
.\launch.ps1 -NoOpen   # サーバーだけ
```

- データ: `%LOCALAPPDATA%\PPTLab\pptlab.sqlite3`（起動時に `backups\` へ最大5世代）
- ポート: 8765

## できること

| 画面 | 内容 |
|---|---|
| Videos | YouTube / ファイルから追加 → 自動解析 → 名前確認 → 登録。試合一覧・絞り込み・40件ページ送り |
| 動画詳細 | 動画と盤面の同期、局面スライダー、試合の連続再生、保存（試合/全体/P1/P2） |
| 名前の確認 | 名前画像つきの表、同じ画像は一括選択、過去の確定名を優先 |
| Tetofu | テト譜の登録・検索・タグ・削除 |
| Tags | タグ一覧、複数選択で AND、条件式検索（`and` / `or` / `()`）、削除 |

## 必要なもの

| 用途 | 必要なもの | 無い場合 |
|---|---|---|
| 画面 | Python 3.11 以降 | 起動しない |
| 解析 | `runtime\bin\TetrisVideoRecovery_stream.exe` + GPU（CUDA / cuDNN） | 追加はできるが解析で失敗する |
| 名前画像 | FFmpeg | 名前画像が出ない |
| YouTube取得 | yt-dlp | ファイルから追加のみ使える |

FFmpeg / yt-dlp は同梱しません（ライセンスとサイズのため）。README の手順で導入してください。

## API（抜粋）

| メソッド | パス | 内容 |
|---|---|---|
| GET | `/api/env` | エンジン / FFmpeg / GPU の有無 |
| GET/POST | `/api/videos` | 一覧 / 追加（URL or パス） |
| DELETE | `/api/videos/<id>` | 動画を一覧から削除 |
| GET | `/api/videos/<id>/matches[?light=1]` | 試合一覧（light は画像なし） |
| POST | `/api/videos/<id>/names` | 名前の保存 |
| POST | `/api/videos/<id>/title` | タイトル変更 |
| GET/POST | `/api/tetofu` | テト譜の一覧 / 登録 |
| DELETE | `/api/tetofu/<id>` | テト譜を削除 |
| GET/POST/DELETE | `/api/tags` | タグの一覧 / 追加 / 全削除 |
| POST | `/api/tags/apply` | 対象のタグを設定 |