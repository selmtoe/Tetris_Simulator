# PPT Lab 引き継ぎメモ（2026-10-29 04:20）

次のセッションはこのファイルから始めてください。

## 今の状態

- ローカル版: `tools\ppt-lab`（server.py / jobs.py / namereview.py / store.py / web/app.js）
- 公開版: https://selmtoe.github.io/Tetris_Simulator/ppt-lab/（`ppt-lab/` + `tools/ppt-lab/webdata` をビルドして配信）
- デスクトップの「PPT Lab」ショートカットから起動（launch.ps1 → server → Edge アプリモード）
- ローカルサーバーは 8765 で起動中（`server.py` を直したので再起動済み。`web/*` は `Cache-Control: no-store` なので再読み込みで反映）
- 直前のコミット: `8b69788`（連続再生の試合連結）→ 今回の修正を次のコミットにまとめる

## 今回直したこと（2026-10-29 早朝）

| 項目 | 状態 |
|---|---|
| 盤面がP1だけになる | ○ `lab_replay()` / `export_web.py` が `combined`（2P）を使うように。既存 webdata の replays 17本も更新 |
| 連続再生が動かない | ○ `pendingSeek` の解除漏れ / 準備前シークの取りこぼし / セグメントの並び / 読み込み中の誤同期 を修正 |
| 手動操作で連続再生を解除 | ○ 試合クリック・前後ボタン・動画一覧へ戻る・対象外の動画を開く |
| デバッグフック | ○ `window.__pptDebug()` に同期・連続再生の状態を追加 |

詳細は `docs/PPT-LAB.md` §34。

## 検証済み（Edge ヘッドレス + YouTube プレイヤー差し替え）

- `/api` 経由: 157局面すべてに P2 の盤面（19動画 × 試合1・2 の38リプレイが `m:2`）
- 閲覧専用版（`dist/pages/ppt-lab`）: 同じく 157/157
- 連続再生: 試合の終わりで次へ（index 0→1）、186試合送っても動画をまたいで継続、移動先も P2 つき105局面

検証スクリプトは `C:\Users\hirom\Documents\AI-Agent-Workspace\ppt-lab-fix\`
（`check-feature.mjs`＝app.jsのロジック単体、`e2e.py`＝ブラウザ、`e2e-static-build.py`＝配信物、`cdp.py`＝DevTools Protocol の最小クライアント）。

## 次にやるとよさそうなこと

1. **公開版の反映**: `python tools\build-web.py` → `python tools\test-web-build.py` はローカルで成功済み。push すれば GitHub Actions が配信する。
2. 連続再生の**画面表示**: いまは開始時にトーストが出るだけで、何番目を再生中か・止めるボタンが無い（`state.feature` はある）。
3. `playNextFeature()` は `featureList` 方式の名残で、いまは実質到達しない（`advanceFeature()` が常に先に処理する）。整理するならここ。
4. 解析失敗区間の送信は未実装（§12 の残り）。
5. `featureTick` は「動画に局面が無い（`state.pages` が空）」と進まない。解析結果が欠けている試合が多い動画では、スキップする仕組みを足すとよい。

## 今日ハマった落とし穴（再発防止）

1. **PowerShell で日本語入りのファイルを書き換えない**。`Get-Content`/`Set-Content` は cp932 で壊れる（今回は検証スクリプトを2回壊した）。編集はエディタ経由で。
2. **PS1 は UTF-8 BOM 必須**。BOM を落とすと PowerShell 5.1 が文字化けして構文エラー。
3. PowerShell の実行ポリシーで `launch.ps1` は直接動かない。サーバー再起動は `pythonw server.py --port 8765` を直接起動する。
4. `export_web.py` を丸ごと流すと**名前画像が消える**（`name_memory` に無い crop は書き出されない）。公開データを部分的に直すときは `webdata` をバックアップし、必要なファイルだけ更新する。
5. `Copy-Item src\theme dst\theme -Recurse` は `dst\theme\theme` を作る。中身は `src\theme\*` でコピー。
6. `replay_file()` は `simulator` フォルダ限定（`profiles.json` をリプレイと誤認した事故）。
7. 設定の bool は `json.dumps` で保存（`0/1` になっていた）。

## 参照ドキュメント

- `docs/PPT-LAB.md` §24〜§34（変更履歴）
- `docs/PPT-LAB-SETTINGS.md`（設定の確定設計）
- `docs/ASTRA-ANALYSIS-OPTIMIZATION.md`（解析高速化の依頼プロンプト）
