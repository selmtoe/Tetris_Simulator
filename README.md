# Tetris AI

Simulator、譜面管理、Hub を1つの静的Webアプリとしてまとめた構成です。画面のDOM/CSS・保存データ・iframe通信は既存版と互換を保ち、ロジックだけを役割別の外部ファイルへ分離しています。

## 起動方法

Windowsでは [start.bat](./start.bat) をダブルクリックしてください。ローカルHTTPサーバーを起動し、Hubを開きます。終了は開いたターミナルで `Ctrl+C` です。

`file://` で直接開かず、必ずローカルHTTP経由で利用してください。Worker、ONNXモデル、Service Worker、Hub内iframeの連携を安定して動かすためです。

## 入口

- `index.html` — メインのシミュレータ
- `F/index.html` — 譜面管理
- `hub/index.html` — Simulatorと譜面管理をiframeで統合するHub
- `Benri/index.html` — テンプレ確率計算
- `Load PPT/index.html` — PPT/画像スキャン用ページ

## 構成

```text
styles/                 Simulatorの固定スタイル
simulator/app/          UI、設定、ゲーム、スキャン、共有、起動処理
simulator/workers/      現行Cold Clear AI Worker
extensions/ppt-scanner/ YouTube動画から局面を取り込むブラウザ拡張機能
F/                      譜面管理
hub/                    統合HubとPWA資産
Load PPT/tetris.onnx    盤面認識モデル
tools/serve.ps1         依存なしのローカル静的サーバー
```

## 互換性の方針

- 既存の画面ID/class、CSS値、URL共有形式、`localStorage`キー、Hubの`postMessage`契約を維持します。
- 旧AI選択肢は廃止し、テンプレ対応のCold Clear実装だけを使用します。
- `LICENSE` のMPL 2.0表記とAI由来のクレジットは維持します。

## Credits

This project uses [Cold Clear](https://github.com/MinusKelvin/cold-clear) by MinusKelvin and contributors under MPL-2.0, including modified Rust search, evaluation and board code, the Rust integration, and a JavaScript port. PC search uses [sfinder-cpp](https://github.com/knewjade/sfinder-cpp) by knewjade under MIT. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the public [license/source page](licenses/index.html) for the full notices, covered files and source downloads.

## Cold Clear implementation

The simulator's active AI and viewer analysis compile the modified Cold Clear
normal/Standard Rust core into WASM. The separate MPL-2.0 JavaScript port is
also used by REN search and regression comparisons. Both include source
access and attribution in the public distribution. See
[simulator/COLD_CLEAR_PORT.md](simulator/COLD_CLEAR_PORT.md) for the source
mapping, scope, licensing note, and test command.

## 探索・分析

1P・2Pどちらも、プレイ中の「探索」をホバーまたはタップすると、PC探索・AI探索・REN探索を縦に表示します。シミュレータの探索対象は常に1Pです。
NEXTは既定で10個（現在ミノと別）です。旧既定の8個は初回読み込みで10個へ移行します。
PC/AI/RENのガイドどおりに設置すると次の手が表示され、異なる配置にした場合は解除されます。探索の開始・進行・終了の説明メッセージは表示しません。Drawの経路表示はデバッグモードだけで有効です。

- **PC探索**：既存のsfinder-cpp WASMを使用。現在ミノ＋NEXT10個＋HOLDから、PCになる手順を探します。`P`のショートカットも維持しています。
- **AI探索**：対戦用と同じCold ClearのRust/WASMを独立した探索状態で使用し、既知のミノまでの予定手順を表示します。既定の思考時間は50msです。
- **REN探索**：現在の局面から毎手ライン消去が続く手順を全探索します。HOLD、空HOLD、HOLD禁止とSRSの回転入れに対応。指定済みのミノ列は画面外の続きも読み、未生成のランダムミノは仮定しません。最初の消去は0 RENです。探索中に同じ項目を再選択すると中止でき、見つかった途中結果があればその手順を表示します。ビューワーでは途中結果を「暫定」、完了した結果だけ「最大」と表示します。

ビューワーでは「分析」からPC探索・REN探索・既存のAI分析を開けます。
PC/RENの手順は元のリプレイを変更せず、スライダーや矢印で確認できます。
途中の「シミュレータ」は、その手を置く前の盤面・HOLD・ミノ列を練習へ渡します。
2PのリプレイではPC/RENの対象プレイヤーを切り替えられます。既存のAI分析はP1を対象とします。

solution-finder本家にも[renコマンド](https://github.com/knewjade/solution-finder/blob/main/docs/source/contents/ren/main.rst)があります。
本ブラウザ版では、既存Cold Clear JSポートの合法手生成と盤面処理を使い、同じ目的の探索をWorkerで実行します。
同一状態の記憶と残りブロック数による上界で探索を減らしますが、評価値やビーム幅による候補の切り捨てはしません。
長い探索も分割実行し、中止や元の盤面の操作を妨げません。

検証: `node tools/test-ren-search.cjs`（全列挙との照合）、`node tools/test-position-search.cjs <preview-url>`（実Worker、10手PC、19 REN、ガイド、PC/スマホUI、練習への引き継ぎ）。

### PC guide details

During a 1P or 2P game, press `P` (or select **PC探索**) to check P1's live board,
the current mino, the visible NEXT queue, and HOLD for a perfect-clear route.
When one is found, the next placement is shown with a translucent dashed
outline on the board. If the indicated mino is placed in exactly that
location, the next step is displayed automatically after the lock. A different
placement, HOLD result, or board change discards the route safely. The
**PC探索** binding is configurable from the 1P controls (default: `P`).

The first version searches the bottom 24 rows, only uses known NEXT minos,
and waits until HOLD is available. It does not control the player or alter the
Cold Clear AI.

The browser bundle is built from the MIT-licensed `sfinder-cpp` core in
`third_party/sfinder-cpp-master/`; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

### Rebuilding the PC WASM bundle

Activate an Emscripten SDK, then run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build-pc-solver-wasm.ps1
```

This regenerates `simulator/pc-solver/sfinder-pc.js` and
`simulator/pc-solver/sfinder-pc.wasm`. Smoke tests are available with:

```text
node tools/test-pc-solver-wasm.js
```
