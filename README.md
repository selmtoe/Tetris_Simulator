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

This project uses AI parameters from [Cold Clear](https://github.com/MinusKelvin/cold-clear) by MinusKelvin, provided under the terms of the [Mozilla Public License 2.0](https://www.mozilla.org/MPL/2.0/). The AI parameter portion follows MPL 2.0.

## Cold Clear implementation

The simulator's AI is a simulator-owned JavaScript port of Cold Clear's
normal/Standard mode, not a copy of the supplied Rust files. It keeps a
long-lived search DAG between moves and uses the reference SRS, hold, 7-bag,
Standard evaluation, and state-reuse design. See
[simulator/COLD_CLEAR_PORT.md](simulator/COLD_CLEAR_PORT.md) for the source
mapping, scope, licensing note, and test command.

## PC guide

During a 1P game, press `P` (or select **PC探索**) to check the live board,
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
