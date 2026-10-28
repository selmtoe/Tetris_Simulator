# PPT Lab — 仕様と現状（2026-10-27）

配布を意識した統合アプリ。Lab / シミュレータ / ビューワー / 配信解析 / yt-dlp を1つの画面にまとめる。
デザインは `Documents\Lab` の実CSS（`base.css` / `lab-enhancements.css` / `lab-writer.css`）をコピーして使用。

## 構成

```
tools/ppt-lab/
  server.py       ローカルHTTPサーバ（127.0.0.1）+ API
  jobs.py         取得（yt-dlp）→解析（TetrisVideoRecovery_stream.exe）のジョブ
  namereview.py   名前画像の生成・同一画像の組分け・勝敗・名前の書き戻し
  store.py        SQLite（videos / tetofu / tags / tag_links / name_memory / jobs）
  web/
    index.html    Lab と同じヘッダー・ナビ・検索
    app.js        3画面（Videos / Tetofu / Tags）と動画詳細・名前編集
    theme/        Lab のCSS ＋ ppt-lab.css（追加分のみ）
  launch.ps1 / PPTLab.vbs / icon-lab.ico
```

- データ: `%LOCALAPPDATA%\PPTLab\pptlab.sqlite3`（起動時に `backups\` へ最大5世代）
- 起動: デスクトップ／スタートメニューの **PPT Lab**（サーバー自動起動＋Edgeアプリモード）

## 画面

| 画面 | 内容 |
|---|---|
| Videos（動画一覧） | タブ（動画一覧/試合一覧）、絞り込み（検索・プレイヤー・相手・タグ・勝敗・並び順）、連続再生、40件ページ送り |
| Videos（試合一覧） | 全動画を横断。王冠つきタイトル、時刻範囲、タグ行、40件ページ送り |
| 動画詳細 | 左に動画（速度・シミュレータ）、右に作業台（シミュレータ/ビューワー/リンクをコピー/保存）、試合タイムライン、局面スライダー |
| 名前の確認 | 145試合の表、名前画像、同一画像の組で一括変更、登録（何度でも再編集可） |
| Tetofu | テト譜・盤面アーカイブ。登録（シミュレータ/エディタ/本家）、検索・タグ絞り込み |
| Tags | タグ一覧（件数つき）。複数クリックで AND、検索は条件式対応 |

## 検索の条件式

- 空白は AND。`and` / `or` / `&&` / `||` / `()` が使える
- 語はタグ名（部分一致）とタイトルに当たる
- 例: `(COM1 or どらえもん) and タマキ`

## タグ

- カードのタグはリンク。押すとタグ画面へ移動し、そのタグで絞り込む
- タグ画面で複数選択すると AND
- 鉛筆メニュー →「タグを削除」で、そのタグを**全オブジェクトから**削除

## 名前まわり

- 解析完了時に自動保存（`needs-names`）→ 名前を直して「登録」で `ready`
- 画像指紋 → 名前 を `name_memory` に保存し、次回は OCR より優先（組全体へ伝播）
- 動画タイトルは鉛筆メニューから変更

## 解析

- `POST /api/videos`（URL or パス）→ ジョブ → 進捗を `/api/videos` に載せる
- ファイル選択はサーバー側で OS ダイアログ（最前面の親ウィンドウ付き）
- 一覧用の軽量API `/api/videos/<id>/matches?light=1`（145試合で約150ms）

## 既知の制約

- 盤面ビューワーの同期は postMessage ではなく `?page=N` の再読み込み（700msスロットル）
  - 参照実装（Lab）は postMessage でページだけ送るが、この環境ではビューワーが `labViewerReady` を返さず成立しなかった
- 「連続再生」は絞り込み結果の先頭試合から再生し、以降は同期で自動送り（Lab の特集ページ相当ではない）
- 解析失敗区間の送信は未実装
- yt-dlp / FFmpeg / CUDA は同梱しない（ユーザー導入）

---

## 13. 監査で見つかった不具合の修正（2026-10-27 03:40）

| 重要度 | 内容 | 修正 |
|---|---|---|
| blocker | `/media` が任意のローカルファイルを配信（HTMLを同一オリジンで実行できる） | 動画拡張子のみ許可（`VIDEO_EXTENSIONS`）、`X-Content-Type-Options: nosniff`、追加時にパス検証 |
| blocker | 動画詳細から戻ると二度と開けない（共有 iframe が破棄され `null` 参照） | `ensureViewerFrame()` で無ければ作り直す |
| blocker | タグ削除ダイアログが空で UI から削除できない | チップ追加前に `return` していたのを修正 |
| major | 「タイトルを変更」が UI から実行できない（URL 必須チェックが先） | rename 時は URL 不要に |
| major | 不正なハッシュ（`#tags/%E0%A4%A`）で白画面 | `decodeURIComponent` を try/catch |
| major | 名前更新で存在しない試合番号を送ると `changed:0` のまま `ready` になる | 事前検証＋変更0件なら状態を変えない |
| major | 名前の長さ無制限 | 40文字上限（サーバー） |
| major | `#videos` に戻っても「試合一覧」のまま | ハッシュが `#videos` なら動画一覧へ |
| major | タグ選択が他画面に漏れて不可視フィルタになる | タグ画面だけで適用 |
| major | タグ絞り込みの選択肢が空（プレイヤー名を除外していた） | 除外をやめた |
| minor | 検索欄が1文字でフォーカスを失う | 260ms デバウンス＋フォーカス復元 |
| minor | `&&` / `||` を空白なしで使うと演算子にならない | トークナイザ修正 |
| minor | 日付が1日ずれる（UTC変換） | ローカル日付で整形 |
| minor | 終端ジョブが `running` のまま残る | `done`/`failed` は必ずDBへ保存 |
| minor | 存在しないテト譜の DELETE が 200 | 404 |
| minor | 不正な Range で全ファイルを返す | 416 |
| minor | 動画・テト譜を削除できない | `DELETE /api/videos/<id>`・`/api/tetofu/<id>` と UI（鉛筆メニュー / カード） |

## 14. 次の候補（未実装）

1. 既存バッチ（`batch-results.json`）の取り込み — `.work-*` を消すと資産が消える問題の根本対策
2. 試合テーブル（試合単位のタグ・メモ・採点）
3. タグの改名・統合
4. export / import（JSON）と即時バックアップ
5. 環境プリフライト（エンジン / FFmpeg / GPU / yt-dlp の状態を画面に出す）
6. 再解析・キャンセル・重複検出
7. 名前グループを P1 / P2 別に
8. 解析物をアプリ管理下へコピー（パス依存の解消）
---

## 15. 使い勝手の改善（2026-10-27 午後）

| 項目 | 内容 |
|---|---|
| 名前の確認 | **同じ名前帯の画像ごとにカードでまとめた**（145試合 → 9組）。1か所直すとその組の全試合に反映。試合ごとの一覧は折りたたみで残す |
| 試合の移動 | 「前の試合 / 次の試合」でタイムラインが**滑らかにスクロール**して追従 |
| 局面の移動 | 「前の局面 / 次の局面」で盤面を一瞬フェードさせ、切り替わりのちらつきを抑える |
| 盤面同期 | **postMessage（load / seek）に切り替え**。`labViewerLoaded` が3秒返らない場合だけ URL 再読み込みへフォールバック |
| リプレイのキャッシュ | 上限8試合（LRU）。145試合を開いても数百MBにならない |
| 自動タグ | 「動画解析」とプレイヤー名も実タグとして保存。タグ一覧の件数・削除・全解除の対象になる（`tags_locked` で再付与を防止） |
## 16. 画面と保存の修正（2026-10-27 夕方）

| 項目 | 内容 |
|---|---|
| ウィンドウ | 起動時に**最大化**（`--start-maximized`） |
| レイアウト | 横幅の上限（1200px）を撤廃して画面いっぱいに。動画と盤面を `calc(100vh - …)` で高さに合わせ、1920×1080 でスクロールなしに収まる |
| 保存（試合 / 全体盤面 / P1 / P2） | 保存URLに**今の局面（`?page=N`）を埋め込む**ようにした。以前は1ページ目（試合開始＝盤面が空）が開いていた |
| 保存の種類 | 試合・P1・P2 = 現在の局面 / 全体盤面 = その試合の決着時（最終ページ） |
## 17. 通知・保存・テト譜・アイコン（2026-10-27 夜）

| 項目 | 内容 |
|---|---|
| 通知 | 画面上部の文字ではなく、**右からスライドインする通知**（3.6秒で消える） |
| 保存 | 保存の前に**タグ選択ダイアログ**を出す。選んだタグを付けてからテト譜に保存 |
| テト譜 | 1行1件のリストをやめて、**正方形タイルのグリッド**に。各タイルに**盤面プレビュー**（P1/P2をCanvasで描画） |
| アイコン | LAB ロゴの白背景を透明化（`icon-lab2.ico` をショートカットに設定、favicon も差し替え） |
## 18. テト譜のプレビューをビューワー埋め込みに（2026-10-27 夜）

- 各タイルのプレビューは、**本物のビューワー（F/index.html）を iframe で埋め込んだもの**。保存URLに `&labViewer=1` を足して、操作できないサムネイルとして縮小表示（`pointer-events: none`）。
- `loading="lazy"` で画面外は読み込まない。
- アイコンは `icon-lab3.ico`（透過）に更新し、Windows のアイコンキャッシュも再構築した。
## 19. タグ選択と通知デザイン（2026-10-27 夜）

| 項目 | 内容 |
|---|---|
| タグ付け | ダイアログを開く前に `/api/tags` を読み込むようにした。**既存タグがチップで並び、クリックで付け外し**できる（起動時にも読み込み） |
| 通知 | 白いカード＋細い枠＋左のアクセントバー（緑＝通常、赤＝エラー）＋点。Lab のカードと同じ質感に統一 |
## 20. タイル統一・タグ・移行（2026-10-27 夜）

### 表示
- 動画一覧もテト譜と同じ**正方形タイル**（`.ppt-tile-grid`）に統一。タグ検索の結果も同じグリッドで並ぶので、動画とテト譜が混ざっても大きさが揃う。

### タグの付き方
1. 動画を追加すると、解析結果から **`動画解析` + プレイヤー名** が自動で付く（`videos.tags_locked = 0` のときだけ materialize）。
2. そのあと手動でタグを付けると `tags_locked = 1` になり、自動付与は止まる（＝全解除もできる）。
3. **自動タグ（動画解析・プレイヤー名）はダイアログで外せない**（`自動` ラベル付きの無効チップ）。

### プレイヤー名
- 名前を確認済みなら **autosave の確定名**を優先（`video_players`）。
- 未確認なら OCR 名を使うが、NFKC＋記号除去＋小文字化したキーで**表記ゆれを畳む**（例: いんふい / いんふlI / いんふしI → 1件）。

### 公開Labからの移行（`import_lab.py`）
- Lab DB（records）から **18動画 / 1460試合 / 4テト譜 / タグ** を取り込み済み。
- 動画ごとに `%LOCALAPPDATA%\PPTLab\imported\<videoId>\manifest.json` と `winner-detection.json` を作る。
- 試合の名前は manifest の `player_names`、勝敗は `winner-detection.json` から読む。
- 同じYouTube IDが既にローカルにある場合は重複を作らない。
## 21. デバッグ第2ラウンドの修正（2026-10-27 夜）

| 重大度 | 内容 | 修正 |
|---|---|---|
| blocker | 取り込み動画（画像なし）で名前が1組に融合し、1文字の変更で全員上書き | 画像が無い場合は**名前ごとにグループ化**（137試合→13組で確認） |
| blocker | 動画を切り替えても名前の未保存編集が残り、別動画に誤適用 | `state.edits` を動画IDで管理し、切替時に破棄 |
| 高 | `tags_locked` が API に出ておらず、タグを全消ししても自動で復活 | `videos()` に列を追加＋自動付与は `materialize_tags`（ロックしない） |
| 高 | リプレイのキャッシュが動画をまたいで混線 | キーを `videoId:index` に変更 |
| 高 | 自動タグが「タグを削除」で消せる | 削除モードでも自動タグは無効チップ |
| 中 | `()` や末尾 `or` が全件ヒット | 不正な式は**0件**（`invalid` を返す） |
| 中 | 画面ごとに検索対象が違う | タグ画面も title＋players を対象に |
| 中 | タグの複数選択が URL に載らない | `#tags/a+b` に同期（`replaceState`） |
| 中 | 条件式を入れるとタグクラウドが消える | 単純語のときだけクラウドを絞る |
| 中 | 試合一覧のプレイヤー絞り込みが試合行に効かない | `row.p1/p2` で絞る。選手が居ない行は勝敗で落とす |
| 中 | プレイヤー選択肢に OCR のゴミ（`???`・1文字） | 2文字未満と `?` を含む名前を除外 |
| 中 | `/api/env` を毎回 nvidia-smi で取得 | 60秒キャッシュ |
| 低 | 名前確認画面に動画名が無い | 見出しに動画タイトルを追加 |
| 低 | 試合カードの空タグ行・通知の増えすぎ・メニューが外側クリックで閉じない | 行ごと非表示／最大4件／外側クリックで閉じる |
## 22. 取り込みのやり直しと状態の修正（2026-10-27 夜）

- 「名前を確認」バッジは `status = needs-names` の動画にだけ出る。ローカル動画がテストで `needs-names` のまま残っていたのが原因。
- **状態を実データから再計算**するようにした（全試合に P1/P2 の名前が入っていれば `ready`）。
- 取り込みを一度きれいに消して（`video-lab-*` と `imported/` を削除）、`import_lab.py` を作り直して再実行。
  - YouTube ID で既存動画を判定してスキップ（ローカルの bTL5xEH5OJA は二重にならない）
  - `imported/<videoId>/manifest.json` と `winner-detection.json` を同じ場所に出力
  - Lab 由来のタグだけを足す（手動タグは消さない）
  - 名前が揃っている動画は `ready` で登録
- 結果: **18動画 / 1460試合 / 48タグ / テト譜5件**、`needs-names` は0件。
## 23. 自動タグの扱いを整理（2026-10-27 夜）

- 自動タグ（`動画解析` + 全プレイヤー名）は**常に付く**ようにした。`/api/videos` で自動タグを合成して返し、`tag_links` にも実体として入れる（件数・検索の対象）。
- 手動保存（`/api/tags/apply`）では自動タグを保存しない（常に導出されるため、消えることがない）。
- タイルのタグ表示は 8 件まで＋`+N`。以前は 6 件で切れていたため「全員分ない」ように見えていた。
- ローカル動画は `tags_locked=1` でも自動タグが出るようになった（手動タグ `COM1` と合わせて 10 件）。
## 24. 設定画面とプレイヤーネームの対応（2026-10-27 夜）

### 設定（ナビの「設定」タブ）

| 項目 | 内容 | 保存先 |
|---|---|---|
| テーマ | ライト / ダーク（`html[data-theme]` で Lab の色変数を切り替え） | `settings.theme` |
| シミュレータ・エディタ | 既定のURL、外部リンクの開き方（新しいタブ / 同じタブ） | `settings.simulatorBase` / `settings.openLinks` |
| 解析 | 同時に解析する本数（1〜3）※保存のみ。実際の並列化はエンジン側の実装待ち | `settings.analysisWorkers` |

設定は SQLite の `settings` テーブルに保存され、再起動後も保持されます（API: `GET/POST /api/settings`）。

### プレイヤーネームの対応

- `name_memory`（画像の指紋 → 名前）の一覧を表示。**画像・名前・使用回数・更新日**が見えます。
- 「解除」で紐づけだけ削除（過去に保存した名前は消えません）。
- 画像は名前を保存したときに `%LOCALAPPDATA%\PPTLab\name-crops\<指紋>.png` へ複製。既存分は後から補完済み（241件）。
- API: `GET /api/names/mappings`、`DELETE /api/names/mappings {fingerprint}`、画像は `GET /api/name-crops/<指紋>.png`。

### 動画ごとの一時上書き

- プレイヤー名の変更画面の各グループに **「この動画だけ」** チェックを追加。
- チェックして保存すると `name_overrides(video_id, fingerprint, name)` に記録され、**その動画だけ**表示名が変わります。
- 優先順位: **動画の上書き ＞ 全体の対応（name_memory） ＞ OCR**。
- 上書き中はマッチ行に `p1Override` / `p2Override` が立ち、画面でもチェック済みで開きます。
- チェックを外して保存すると上書きが解除されます。

### 名前ラベル

- 「名前を確認」→ **「プレイヤー名を変更」** に統一（バッジ・通知・鉛筆メニュー・見出し）。
## 25. ダークテーマと名前のまとめ方（2026-10-27 深夜）

### ダークテーマ
- 公開Lab（`Documents\Lab\library.css`）の定義をそのまま採用。
  `--bg-color:#252527` / `--surface-color:#303032` / `--text-main:#dedee1` / `--text-muted:#aaaab1` / `--border-color:#48484b` / `--accent-color:#ffffff`
- 入力欄・カード・タグ・トーストも Lab と同じ規則（タグは `#3a3a3d` / `#d6d6da`）で切り替え。

### 名前のまとめ方
- **プレイヤー名の変更画面**: 画像のまとまり（`p1Group`/`p2Group`）で1枚のカード。画像が無い動画（取り込み分）は**名前ごと**に分ける。同じ名前の組は P1/P2 をまたいで統合。
- **設定 → プレイヤーネームの対応**: 名前ごとに1行（画像・名前・「N回 / M画像」・更新日）。解除はその名前の対応をまとめて削除（過去の保存名は残る）。
## 26. 名前登録が無反応だった件とダークの追い込み（2026-10-27 深夜）

### 原因
- `saveNames()` の「取りこぼし防止」ブロックが `entry.matches` を参照していたが、グループは `rows` を持つよう変わっていたため **毎回 TypeError**。しかも `try` の外だったので握り潰され、POST も飛ばず無反応に見えていた。
- 同種のループが二重にあり、しかも `updates` 計算の後だったため効果がなかった。

### 修正
- 壊れた同期ループを**両方削除**（入力欄は input ハンドラで `state.edits` に入っているので不要）。
- 登録の動作確認: 29試合 → 「29試合の名前を保存しました。」、再描画で「変更 0件」。

### 併せて直したもの
- `REVIEW.forget()` が `_light` キャッシュを捨てておらず、**保存しても一覧が古いまま**だった → `_light` も破棄。
- autosave が無い取り込み動画は `apply_names` が manifest の `player_names` を更新するようにした。
- ダークの白地に白文字（ダイアログの主ボタン、ネーム表のホバー）を修正。タブの選択中も判別できるように。

### ラベル
- バッジ・通知は **「プレイヤー名を確認」**（メニューは「プレイヤー名を変更」）。
## 27. 閲覧専用版（GitHub Pages）（2026-10-28）

### 公開先
**https://selmtoe.github.io/Tetris_Simulator/ppt-lab/**

- 公開Lab（`selmtoe.github.io/Lab`）のナビに **「PPT Lab ↗」** を追加（新しいタブで開く）
- 配布版（`dist` のインストーラ）には含めない。Lab からの外部ツール扱い

### 仕組み
- `tools/ppt-lab/export_web.py` が DB から `tools/ppt-lab/webdata/` に静的JSONを書き出す
  （videos / matches / tags / tetofu / name-mappings / 名前画像）
- `tools/build-web.py` が `ppt-lab/`（index・app・theme・vendor）と `webdata` を `dist/pages/ppt-lab/` にコピー
- ビルド時に `index.html` へ `<script>window.PPT_STATIC=true;</script>` を注入
- `web/static-api.js` が `/api/*` を `data/*.json` に差し替え（`PPT_STATIC` のときだけ動く）
- 書き込み系（追加・保存・削除・名前編集・タグ編集）は `html[data-readonly]` の CSS で非表示

### できること / できないこと
| 機能 | 閲覧専用版 |
|---|---|
| 動画一覧・試合一覧・絞り込み・検索 | ○ |
| 動画詳細（YouTube 再生） | ○ |
| 盤面ビューワー | ×（リプレイJSONは公開していない） |
| プレイヤー名の確認（閲覧） | ○（編集は不可） |
| テト譜・タグ・設定（表示） | ○ |
| 追加・解析・保存・削除 | × |

### 更新手順
```
python tools\ppt-lab\export_web.py
python tools\build-web.py
git add tools/ppt-lab/webdata && git commit && git push
```
## 28. 名前の変更と自動タグの連動（2026-10-29）

- プレイヤー名を保存すると、**その動画の自動タグも付け替える**。
  - 変更前のプレイヤー名（`video_players`）と変更後を比べ、消えた名前のタグリンクを外し、増えた名前のタグを付ける。
  - どの動画・テト譜にも付かなくなったタグは**自動で削除**（`STORE.prune_tags()`）。
- 名前を戻すとタグも元に戻る（テストで往復確認済み: `いんたい` → `テスト名前` → `いんたい`）。
- 実装: `server.py` の `/names` ハンドラ、`store.py` の `remove_tag_link` / `prune_tags`。
## 29. 解析エンジンの差し替えとモデル切替の土台（2026-10-29）

### エンジン
- `runtime\bin\TetrisVideoRecovery_stream.exe` を **onepass_b**（SHA256 339290F4…）へ差し替え。旧版は削除済み。
- 実測（145試合・同一動画）: **752.7秒**（旧 1688.0秒）＝約2.24倍高速。試合数145・失敗0で一致。
  - 内訳: 解析 724.7秒（旧1254.0）、シーン検出 741.2秒（旧434.0）。検出と解析が重なるため合計が大幅に短縮。
- `analysisContractVersion` は変更していない（キャッシュは有効なまま）。

### 起動時の環境変数（8GB GPUで3ワーカー常時）
`run-ppt-stream-analysis.ps1` と `tools\tetris-lab\server.py` の両方で設定:
```
TR_BATCH_WORKERS=3 / TR_BOARD_WORKERS=2
TR_BATCH_WORKER_RAM_MB=1536 / TR_BATCH_RAM_RESERVE_MB=2048
TR_BATCH_WORKER_VRAM_MB=1024 / TR_BATCH_VRAM_RESERVE_MB=512
TR_BATCH_RELEASE_DELAY_SECONDS=5
```

### モデル切替の土台
- `tools\ppt-lab\profiles.json` に解析プロファイル（standard / fast320 / other-game）。
  `sceneModel` / `boardModel` / `settings` を書くと、そのモデルで解析する。
- 設定キー `analysisProfile`（既定 `standard`＝従来どおり）。
- API: `GET /api/analysis/profiles`。
- `jobs.py` が `-SceneModelPath` / `-BoardModelPath` / `-SettingsPath` を PS1 に渡す。

### 注意
- PS1 は **UTF-8 BOM 必須**（BOM を落とすと PowerShell 5.1 が文字化けして構文エラーになる）。
## 30. 設定タブの全面実装（2026-10-29）

### 構造
Settings は **5タブ**: 一般 / 表示 / 解析 / 名前とタグ / データ管理。
`#settings/データ管理` のようにタブをURLで指定できる。

### 設定の器
- `SETTINGS_SCHEMA`（server.py）に19項目の型・既定値・選択肢・注意書きを定義
- `GET /api/settings/schema` を画面が読んで描画（設定追加時にJSを触らない）
- 値は JSON で保存（bool/int/enum/path）。`GET/POST /api/settings` は既定値マージ＋型変換
- 保存失敗時は画面の値を元に戻す（`saveSetting` のロールバック）

### 配線済み
| 設定 | 反映先 |
|---|---|
| theme | `html[data-theme]` |
| pageSize | 一覧のページ件数 |
| playbackRate | 動画の既定再生速度 |
| sortOrder | 一覧の既定並び順 |
| startView | 起動時に開く画面（`pptLastView` を localStorage に保存） |
| autoTags | `/api/videos` の自動タグ付与を止める |
| downloadQuality | yt-dlp の `-f`（1080/720/480/320） |
| analysisDevice=cpu | PS1 `-ForceCpu`（`TR_BATCH_CPU_MODE=1`） |
| useRamDisk | PS1 `-UseRamDisk` |
| detailedLog | PS1 `-DetailedLog` |
| analysisProfile | `profiles.json` のモデルパスを PS1 へ |

### データ管理タブ
- プレイヤーネームの対応: 検索・名前変更・統合・解除（`/api/names/rename`・`/api/names/merge`）
- 動画の保存: `videoStorage` / `missingVideoAction` / 合計サイズ
- キャッシュ: 名前画像・解析キャッシュのサイズ表示と削除（`/api/data/cache`）
- このアプリについて: 版・データの場所・DBサイズ・動画/試合数（`/api/data/info`）

### 未配線（値は保存されるが動作に未反映）
`tagPromptOnSave` / `downloadDir` / `analysisOutputDir` / `videoStorage` / `missingVideoAction`
### 追加配線（同日）
- `downloadDir`: yt-dlp の出力先（`_target_dir()`）。未設定なら従来のデータフォルダ。
- `analysisOutputDir`: PS1 へ `-OutputDirectory` を渡す。
- `videoStorage=copy`: ローカル動画を追加したとき `media\pending-xxxx\` へ複製してから登録。
- `analysisDevice=cpu`: PS1 `-ForceCpu` → `TR_BATCH_CPU_MODE=1`。
- 全ビュー（videos / settings 5タブ / tags / tetofu）でコンソールエラーなしを確認。
## 31. 盤面ビューワーの不具合修正（2026-10-29）

### 症状
- 盤面が「1 / 1」になり、動画を再生しても局面が進まない
- 症状が進むと盤面自体が表示されない

### 原因
`replay_file()` が `simulator.combined` の**親フォルダにある最初のJSON**をリプレイとして返していた。
取り込み動画は `combined` が空なので `Path('').parent` = カレント（`tools\ppt-lab`）になり、
同日に追加した `profiles.json` をリプレイとして読んでいた。

### 修正
- `combined` が空なら `None`
- フォルダ名が `simulator` のときだけ返す
- `profiles.json` は除外

### 併せて
- YouTube 動画は `iframe` 埋め込みだけで再生時間が取れず同期できなかったため、**YouTube IFrame API** でプレイヤーを作るようにした（`loadYouTubeApi` / `mountYouTubePlayer`）。

### 確認
| 動画 | 局面 |
|---|---|
| ローカル（145試合） | 1 / 106 |
| ローカル（186試合） | 1 / 34（選択中の試合） |
| 取り込み | 1 / 57・1 / 170 |
## 32. 連続再生を公開Labの挙動に近づけた（2026-10-29）

- 公開Lab は絞り込み結果を仮想動画（`#feature/<id>`）として開き、該当試合を連結再生する。
- PPT Lab は「先頭の動画を開くだけ」だったので、**絞り込み結果の動画を順に再生**するようにした。
  - `連続再生` で `state.featureList`（試合がある動画のID列）を作る
  - 動画が終わったら（local は `ended`、YouTube は `onStateChange` の状態0）次の動画へ
  - 対象外の動画を開いたら連続再生は解除
- 試合の切り替えは従来どおり同期（`syncTick`）が行う。
## 33. 連続再生を「試合だけをつなぐ特集動画」に（2026-10-29）

- 絞り込み結果の**試合単位**でセグメントを作る（`buildFeatureSegments`）。
- 再生中は `featureTick` が現在の試合の範囲だけを再生し、`match.end` に達したら次の試合へ飛ぶ（試合間の待ち時間は飛ばす）。
- 動画が変わるときは `#video/<id>` へ移動して先頭試合から続ける。
- 動画の終了（local の `ended` / YouTube の状態0）でも次の試合へ。
- 試合が尽きたら「連続再生が終わりました。」で解除。
## 34. 盤面がP1だけになる件と連続再生が進まない件（2026-10-29）

### 1) 盤面が P1 だけになる
**原因（データ）**: 公開Lab は試合ごとに `p1` / `p2` / `combined` の3つのURLを持つ。
`p1` と `p2` は**片側だけの1Pコレクション**（`m:1`）で、両者の盤面が入っているのは `combined`（`m:2`）だけ。
`lab_replay()` が `p1 or p2` を使っていたため、取り込み動画は P1 の盤面しか届いていなかった。

**修正**:
- `server.py` `lab_replay()`: `combined` を優先（`decode_state_hash(combined) or p1 or p2`）、`urls.combined` も `combined`。
- `export_web.py`: 閲覧専用版の状態ハッシュも `labSimulator.combined` を優先。
- 既存の `tools/ppt-lab/webdata/replays/*.json`（17本）を combined のハッシュへ更新。

**確認**（Edge ヘッドレスで実測）:
| 対象 | 局面 | P2 のある局面 |
|---|---|---|
| ローカル（`/api`） | 157 | 157 / 157 |
| 閲覧専用版（`dist/pages`） | 157 | 157 / 157 |
| 全19動画の `/api/.../replay/{1,2}` | 38件 | 38 / 38 が `m:2` |

### 2) 連続再生が動かない
**原因（コード）は4つ**:
1. `videoTime()` が `pendingSeek` を**握ったまま解除しない**。
   YouTube プレイヤーの準備前にシークすると `pendingSeek` が入り、以後ずっとその秒を返すため、
   同期も連続再生も同じ局面で止まっていた。
2. `seekVideoTo()` が**プレイヤー準備前のシークを捨てていた**（`onReady` で反映されない）。
3. `buildFeatureSegments()` が**動画をまたいで `start` で並べ替え**ていたため、別の動画の試合が混ざっていた。
4. 次の試合・動画の読み込み中も `featureTick` が古い `pages` で動いていた／手動で試合を選んでも連続再生が解除されなかった。

**修正**:
- `pendingSeek` は「シーク前の位置から動いた」「目標に追いついた」「4秒経過」のいずれかで必ず解除（`pendingSeekFrom` / `pendingSeekAt`）。
- 準備前のシークは `onReady` で一度だけ反映（`pendingSeekPaused` も保持）。
- セグメントは**絞り込み結果の動画順**のまま、同じ動画の中だけ試合番号順に並べる。
- `state.featurePending`（読み込み待ち）で `featureTick` を止め、`showMatch` が読み終えたら解除。
- 手動で試合を選ぶ／前後ボタン／動画一覧へ戻る／対象外の動画を開く → `stopFeature()` で解除。
- `showMatch` は読み込み前に `state.pages` を空にし、読み込み中に動画が変わっていたら破棄。

**確認**（Edge ヘッドレス + YouTube プレイヤーだけ差し替え）:
- 試合の終わりで次の試合へ進む（`feature.index` 0 → 1、`currentMatch` 1 → 2）
- 186試合ぶん送っても**動画をまたいで続く**（`video-5802f…` → `video-lab-4f650…`、移動先も P2 つき 105局面）
- `window.__pptDebug()` に連続再生・同期の状態（`pagesWithP2` / `feature.index` / `videoTime` / `pendingSeek` など）を追加
## 35. プレイヤー1/2の絞り込みと試合単位の一致（2026-10-29）

### 症状
- プレイヤー=いんたい／相手=syakegohan にしても、**あめみや・たいようの動画**が一覧に出る
- 試合一覧は 72試合なのに、連続再生すると **172試合** が流れる

### 原因
- 絞り込みが**動画単位**だった。`video.players` に2人の名前が入っていれば通過するので、
  「いんたい vs あめみや」と「syakegohan vs たいよう」を別々に含む動画まで出ていた。
- 連続再生は動画一覧から作り、その動画の**全試合**をセグメント化していた（72 + 100 = 172）。

### 修正
- 表記: 「プレイヤー」→**プレイヤー1**、「相手」→**プレイヤー2**。
- `pairOk(row)`: 指定した2人が**同じ試合に居る**ときだけ通す。P1/P2 の左右は問わない
  （両方に同じ名前を選んだときだけ両側一致を要求）。
- `matchFilterOk(row)` = `pairOk` + `resultOk` を**試合一覧と連続再生で共用**。
- 動画一覧も `videoHasPairMatch()` で試合データを見て絞る（プレイヤー指定時は一覧でも試合データを読む）。
- 動画カードと「N動画 · M試合」の件数も、絞り込み後の試合数を表示。
- 連続再生は `buildFeatureSegments()` の中で同じ `matchFilterOk` を通すので、**試合一覧と必ず同じ件数**になる。

### 確認（実データ: いんたい / syakegohan）
| 画面 | 修正前 | 修正後 |
|---|---|---|
| 動画一覧 | 2動画 · 172試合 | **1動画 · 72試合** |
| 試合一覧 | 1動画 · 72試合 | 1動画 · 72試合 |
| 連続再生のセグメント | 172 | **72** |
| P1/P2 を入れ替え | — | 同じ 1動画 · 72試合 |
| 片方だけ指定（いんたい） | — | 15動画 · 860試合（従来どおり） |