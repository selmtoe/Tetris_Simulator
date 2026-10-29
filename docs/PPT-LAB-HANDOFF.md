# PPT Lab 引き継ぎメモ（2026-10-29 14:30）

次のセッションはこのファイルから始めてください。

## 今の状態

- ローカル版: `tools\ppt-lab`（server.py / jobs.py / namereview.py / store.py / web/app.js）
- **画面は `http://127.0.0.1:8765/ppt-lab/`**（デスクトップのショートカットもここを開く）
- **`/` はシミュレータ本体**。中身は `dist/pages`（= GitHub Pages と同じビルド）を配信している。
  - 起動時に `dist/pages` が無い／古ければ `tools/build-web.py` を自動実行（`PPT_LAB_SKIP_BUILD=1` で抑止）
  - `F/` `hub/` `simulator/` `shared/` `styles/` `icons/` `ai-lab/` `Load PPT/` を許可制で配信（`tools/` や `docs/` は404）
- 公開版: https://selmtoe.github.io/Tetris_Simulator/ppt-lab/（`ppt-lab/` + `tools/ppt-lab/webdata` をビルドして配信）
- ローカルサーバーは 8765 で起動中（`server.py` を直したので再起動済み。`web/*` は `Cache-Control: no-store`）
- 直前のコミット: `e243458`（ビューワー/シミュレータをローカル配信）→ 今回の修正を次のコミットにまとめる

## 今日直したこと（2026-10-29）

| 項目 | 状態 |
|---|---|
| 盤面がP1だけになる | ○ `lab_replay()` / `export_web.py` が `combined`（2P）を使うように。既存 webdata の replays 17本も更新 |
| 連続再生が動かない | ○ `pendingSeek` の解除漏れ / 準備前シークの取りこぼし / セグメントの並び / 読み込み中の誤同期 を修正 |
| 手動操作で連続再生を解除 | ○ 試合クリック・前後ボタン・動画一覧へ戻る・対象外の動画を開く |
| デバッグフック | ○ `window.__pptDebug()` に同期・連続再生の状態を追加 |
| プレイヤーの絞り込み | ○ 「プレイヤー1 / プレイヤー2」に改名。**2人が同じ試合で対戦している動画だけ**に |
| 試合一覧と連続再生の件数 | ○ 同じ `matchFilterOk()` を使うので一致（72 = 72） |
| ビューワー/シミュレータ | ○ ネットではなく**ローカル**から。ローカルは公開版と同じビルド（`dist/pages`）を配信 |
| テト譜のプレビュー | ○ Lab自身が映る不具合を解消（`/` をシミュレータに戻した） |

詳細は `docs/PPT-LAB.md` §34〜§37。

## 検証済み（Edge ヘッドレス + YouTube プレイヤー差し替え）

- `/api` 経由: 157局面すべてに P2 の盤面（19動画 × 試合1・2 の38リプレイが `m:2`）
- 閲覧専用版（`dist/pages/ppt-lab`）: 同じく 157/157
- 連続再生: 試合の終わりで次へ（index 0→1）、186試合送っても動画をまたいで継続、移動先も P2 つき105局面
- いんたい / syakegohan: 動画一覧 1動画・72試合、試合一覧 72、連続再生 72（左右を入れ替えても同じ）
- ビューワー: `http://127.0.0.1:8765/F/?labViewer=1` を開いて 157局面・2400x1148 で描画、ページ送りも追従
- テト譜のプレビュー: シミュレータ4件＋エディタ1件。Lab を映すものは 0

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
