# PPT Lab（ぷよテト動画解析ラボ）インストールガイド

このガイドは、**はじめて PPT Lab を使う人**向けです。
むずかしい知識は要りません。順番に進めれば、最後にはブラウザーに解析画面が開きます。

- 所要時間: 15〜40 分（回線が速ければ 10 分ほど）
- 通信量の目安: 0.1〜1.5 GB（CUDA / cuDNN を取得する場合）
- むずかしさ: ★★☆☆☆（ダブルクリックと Enter だけで進めます）

---

## 1. これは何？

PPT Lab は、ぷよぷよテトリス（PPT）の対戦動画を読み込んで、

- 盤面の状態
- NEXT / HOLD
- 勝敗（3 クラス判定）

を自動で取り出し、ブラウザーで見られるようにするアプリです。
動画の解析そのものは GPU を使う別のプログラム（`TetrisVideoRecovery_stream.exe`）が担当し、
画面（ブラウザー）と操作は Python 製の小さなサーバーが担当します。

インストーラ（`install.cmd`）は、この 2 つが動くように**足りないものだけ**を用意します。

---

## 2. 必要なもの

| 項目 | 必要なもの | 補足 |
| --- | --- | --- |
| OS | Windows 10（1809 以降）/ Windows 11 | 64bit のみ |
| GPU | NVIDIA の GPU（GeForce など） | **無いと実用になりません**（後述） |
| メモリ | 16 GB 以上を推奨 | 動画解析はメモリをよく使います |
| 空き容量 | 8 GB 以上 | 動画のキャッシュと解析結果も入ります |
| ネット | インストール時だけ必要 | インストール後はオフラインでも動きます |
| 権限 | 管理者でなくても可 | RAM ディスクの登録だけは管理者が必要 |

### GPU が無い場合

GPU が無くてもインストール自体はできますが、**CPU モードは非常に遅い**です。
目安として、GPU なら数分の動画が CPU では数十分〜数時間かかります。
インストーラもその点を警告し、「このまま続行しますか？」と確認します。

---

## 3. ダウンロードして展開する

1. 配布された ZIP ファイルを**右クリック →「すべて展開」**します。
   - エクスプローラーの「圧縮フォルダー」のままダブルクリックして中で実行すると、
     ファイルが一時フォルダーに展開されてしまい、うまく動きません。必ず展開してください。
2. 展開したフォルダーを、**パスが短くて書き込みできる場所**に置きます。

   よい例:

   ```
   C:\PPTLab
   C:\Users\<あなたの名前>\Documents\PPTLab
   ```

   なるべく避けたい例:

   ```
   C:\Program Files\...            ← 書き込みに管理者権限が要る
   C:\Users\<名前>\OneDrive\...    ← 同期でロックされることがある
   C:\Users\<名前>\Documents\とても長いフォルダー名\さらに長い名前\...  ← パス長の上限に当たる
   ```

3. 展開したフォルダーの中に `tools\ppt-setup\install.cmd` があることを確認します。

```
PPTLab\
├─ tools\
│  ├─ ppt-setup\
│  │  ├─ install.cmd      ← これをダブルクリック
│  │  ├─ install.ps1
│  │  ├─ launch.cmd
│  │  ├─ uninstall.ps1
│  │  └─ README.md
│  ├─ ppt-lab\            ← 画面とサーバー
│  └─ video-recovery-runtime\   ← 解析エンジンと DLL
└─ docs\
   └─ INSTALL.md          ← このファイル
```

---

## 4. インストールする

### 4-1. 実行する

`tools\ppt-setup\install.cmd` を**ダブルクリック**します。

> **おすすめ**: 「管理者として実行」しておくと、RAM ディスクの登録まで一度に終わります。
> やり方は、`install.cmd` を右クリック →「管理者として実行」です。
> 管理者でなくても、ショートカット作成までは問題なく進みます。

### 4-2. 「Windows によって PC が保護されました」と出たとき

署名のないスクリプトなので、SmartScreen が警告を出すことがあります（正常な動作です）。

1. 青い画面の **「詳細情報」** をクリック
2. **「実行」** をクリック

### 4-3. 画面の流れ

黒い画面が開き、次の 8 ステップが日本語で表示されます。

| # | ステップ | 何をするか | 聞かれること |
| --- | --- | --- | --- |
| 1 | Windows と実行環境 | OS・64bit・権限・空き容量を確認 | なし |
| 2 | Python | Python 3.11 以上を探す。無ければ導入 | 入れ方（公式インストーラ / embeddable / やめる） |
| 3 | GPU | `nvidia-smi` で NVIDIA GPU を確認 | GPU が無いときだけ「CPU モードで続行しますか？」 |
| 4 | CUDA / cuDNN | 必要な DLL を確認し、無ければ補う | pip で取得してよいか（数百 MB） |
| 5 | ffmpeg | GPL ビルドを LGPL 共有ビルドへ差し替える | 差し替えてよいか（推奨: はい） |
| 6 | 最終チェック | 足りないファイルを ✅ / ❌ で一覧表示 | なし |
| 7 | ショートカット | デスクトップとスタートメニューに作成 | なし |
| 8 | RAM ディスク（任意） | タスク登録するか確認 | 「登録しますか？」（既定: いいえ） |

- 確認は **Enter だけ**で「既定（おすすめ）」のまま進みます。
- 何度実行しても壊れません。既にあるものは「既にあります」と表示して飛ばします。
- すべての記録は `%LOCALAPPDATA%\PPTLab\logs\install-YYYYMMDD-HHMMSS.log` に残ります。

### 4-4. 途中で失敗したとき

失敗しても、画面の下に **「この後やること」** が番号付きで出ます。

```
この後やること（2 件）
 1. cublas64_13.dll
    いまの状態: 未検出
    対処: tools\video-recovery-runtime\bin に手動でコピーしてください
    入手先: https://developer.nvidia.com/cuda-downloads
 2. ...
```

そのとおりに直してから、もう一度 `install.cmd` を実行してください。
`-DryRun` を付けると、**何も変更せずに**現状だけ確認できます（後述）。

---

## 5. 起動する

インストールが終わると、**デスクトップに「PPT Lab」**ができています。
ダブルクリックすると、

1. 裏でサーバーが起動し（黒い画面は出ません）
2. 10 秒ほどでブラウザー（Edge）が開き、解析画面が表示されます

URL は `http://127.0.0.1:8765/ppt-lab/` です。

### 起動しないとき

もう一度ダブルクリックしてください（二重起動はしません）。
それでも駄目なら、`tools\ppt-setup\launch.cmd` を直接ダブルクリックします。
失敗すると、自動で診断が走り、日本語で原因と対処が出ます。

### 画面の使い方

- 動画を画面にドラッグ＆ドロップすると解析が始まります。
- 解析結果は `tools\video-recovery-runtime\data\batches` に保存されます。
- 設定で「RAM ディスクを使う」をオンにすると、動画の作業領域に R: ドライブを使います。

コマンドから 1 本だけ解析したい場合は、既存のラッパーを使います（変更していません）。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run-ppt-stream-analysis.ps1 -Video "D:\videos\match.mp4"
```

---

## 6. うまくいかないとき

### Q1. `install.cmd` をダブルクリックしても一瞬で閉じる

- 展開せずに ZIP の中で実行していないか確認してください（必ず「すべて展開」）。
- `install.ps1` が `install.cmd` と同じフォルダーにあるか確認してください。
- コマンドプロンプトを開いて、次のように実行すると、エラーが読めます。

```bat
cd /d "C:\PPTLab\tools\ppt-setup"
install.cmd
```

### Q2. 「このシステムではスクリプトの実行が無効になっています」

`install.cmd` は `-ExecutionPolicy Bypass` を付けて実行するので、通常は起きません。
手動で実行するときは、次のようにしてください。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

### Q3. Python が見つからない / インストールに失敗する

1. まず手動で入れるのが確実です: <https://www.python.org/downloads/windows/>
2. 「Python 3.11.x」の **Windows installer (64-bit)** をダウンロードして実行
3. 最初の画面で **「Add python.exe to PATH」にチェック**してから Install
4. `install.cmd` をもう一度実行

自動インストールが失敗する原因の多くは、社内プロキシ・セキュリティソフト・
「アプリのインストール制限」です。その場合は手動インストールに切り替えてください。

embeddable zip を使う方法（レジストリを触らない）もあります。
インストーラの選択肢 2 を選ぶと `tools\ppt-setup\python` に展開されます。

### Q4. GPU が検出されない

順に確認してください。

1. その PC に NVIDIA の GPU が載っているか（Intel / AMD の内蔵 GPU だけでは動きません）
2. NVIDIA ドライバーが入っているか: <https://www.nvidia.com/Download/index.aspx>
3. ノート PC の場合、NVIDIA コントロールパネルで GPU が無効になっていないか
4. ドライバー更新後は **PC を再起動**してから `install.cmd` を再実行

### Q5. `cublas64_13.dll` / `cudnn64_9.dll` / `cufft64_12.dll` が見つからないと言われる

このアプリの解析エンジンは **CUDA 13 系 + cuDNN 9 系**を使います。
`onnxruntime_providers_cuda.dll` が実際に要求するのは次の 4 つです。

| DLL | 入手方法 |
| --- | --- |
| `cublas64_13.dll` / `cublasLt64_13.dll` | CUDA Toolkit、または pip の `nvidia-cublas` |
| `cudart64_13.dll` | CUDA Toolkit、または pip の `nvidia-cuda-runtime` |
| `cudnn64_9.dll` + `cudnn_*64_9.dll` | pip の `nvidia-cudnn-cu13`、または公式配布物（要アカウント） |
| `cufft64_12.dll` | 配布物の `bin` に同梱 / CUDA Toolkit / pip の `nvidia-cufft`（12.x） |

いちばん簡単な直し方（NVIDIA アカウント不要）: もう一度 `install.cmd` を実行し、
「pip で NVIDIA の CUDA / cuDNN / cuFFT を取得して bin にコピーしますか？」に **はい** と答えます。
インストーラは次の 4 つを使います（`-cu13` 付きの名前は中身が空のダミーなので使いません）。

```
nvidia-cublas           （13.x）
nvidia-cuda-runtime     （13.x）
nvidia-cudnn-cu13       （既存 cudnn64_9.dll と同じ版に固定。例 9.12.0.46）
nvidia-cufft            （12.x。bin にもシステムにも cuFFT が無いときだけ）
```

手動で入れる場合:

1. CUDA Toolkit 13.x を入れる: <https://developer.nvidia.com/cuda-downloads>
2. `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.x\bin\x64` を開く
3. `cublas64_13.dll` / `cublasLt64_13.dll` / `cudart64_13.dll` / `cufft64_12.dll` をコピー
4. `tools\video-recovery-runtime\bin` に貼り付ける
5. cuDNN は <https://developer.nvidia.com/cudnn>（要 NVIDIA アカウント）から 9 系を取得し、
   `cudnn64_9.dll` と `cudnn_*64_9.dll` を同じ `bin` に貼り付ける

> なぜ `bin` に置くのか: 解析エンジンは**自分の隣のフォルダー**を最初に探します。
> システムに入っていても、`bin` に無いと配布先の PC では動きません。

> **CUDA が無い場合の代替**: 画面（PPT Lab）は **CPU モード**でも起動できます。
> ただし解析は非常に遅くなります（目安: GPU の数十倍。数分の動画が数十分〜数時間）。
> 「まず画面だけ動かしたい」ときは、CUDA を入れずにそのまま起動してください。

### Q6. ffmpeg のライセンス警告が出る

初期状態の ffmpeg は GPL 版（`--enable-gpl`）です。個人利用は問題ありませんが、
**配布する場合は LGPL 版に差し替える**のが安全です。インストーラは差し替えを提案し、
承諾すると LGPL 共有ビルドを取得して `bin` を置き換えます（元のファイルは
`tools\ppt-setup\backup` に退避されます）。

差し替えを断った場合でも動作は変わりません。詳しくは `tools\ppt-setup\README.md`。

### Q7. pip の取得に失敗する（SSL / プロキシ）

- 社内プロキシがある場合: 環境変数 `HTTPS_PROXY` を設定してから再実行
- セキュリティソフトが `pypi.org` を止めていないか確認
- どうしても駄目なら Q5 の手動導入手順へ

```powershell
$env:HTTPS_PROXY = "http://proxy.example.local:8080"
.\install.cmd
```

### Q8. ポート 8765 が使用中と言われる

別のアプリが 8765 を使っています。`launch.cmd` は引数でポートを変えられます。

```bat
launch.cmd 8766
```

その場合、ブラウザーの URL も `http://127.0.0.1:8766/ppt-lab/` になります。

### Q9. ブラウザーは開くが画面が真っ白

1. 少し待ってから再読み込み（F5）
2. サーバーが動いているか確認: ブラウザーで `http://127.0.0.1:8765/api/health` を開く
   - `{"ok":true,...}` が出ればサーバーは正常（画面側の問題）
   - 出なければサーバーが落ちています
3. `%LOCALAPPDATA%\PPTLab\logs` の最新ログを見る
4. `tools\ppt-setup\install.ps1 -Diagnose` を実行して診断する

### Q10. 文字が「????」や「縺ゅ」になる

日本語 Windows のコンソール（CP932）では、絵文字 ✅ / ❌ が表示できないことがあります。
その場合インストーラは自動で `[OK]` / `[NG]` に切り替えます。
強制的に切り替えたいときは、環境変数 `PPTLAB_ASCII=1` を設定してから実行してください。

```bat
set PPTLAB_ASCII=1
install.cmd -DryRun
```

ログファイルは常に UTF-8 なので、メモ帳で開けば日本語と ✅/❌ が正しく読めます。

### Q11. ショートカットが作られない

- デスクトップが OneDrive で同期されていると、作られないことがあります
- その場合は `tools\ppt-setup\launch.cmd` を右クリック →「ショートカットの作成」で手動作成し、
  好きな場所へ移動してください
- スタートメニューは `%APPDATA%\Microsoft\Windows\Start Menu\Programs\PPT Lab` に作られます

### Q12. 解析が途中で止まる / メモリ不足になる

- 動画が 4K の場合は、1080p に変換してから解析すると安定します
- 空きメモリを増やす（ブラウザーのタブを閉じる）
- RAM ディスクを使っている場合、サイズを 4096 → 2048 に下げると改善することがあります
  （`tools\ppt-ramdisk-install.ps1 -SizeMB 2048`）
- 解析ログは `tools\video-recovery-runtime\data\batches` 以下に残ります

### どこを見ればいいか分からない

まずこの 3 つを確認してください。

```text
%LOCALAPPDATA%\PPTLab\logs\install-YYYYMMDD-HHMMSS.log   ← インストールの記録
%LOCALAPPDATA%\PPTLab\logs\                                 ← アプリのログ
tools\ppt-setup\state.json                                  ← いまの構成（Python の場所など）
```

診断モード（何も変更しません）:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\ppt-setup\install.ps1 -Diagnose
```

---

## 7. もう一度実行する / 更新する

`install.cmd` は**何度実行しても壊れません**。

- 既にあるものは「既にあります」と表示して飛ばします
- 足りないものだけを追加します
- ショートカットは最新の内容に更新されます

**変更せずに確認だけしたいとき**:

```bat
install.cmd -DryRun
```

これは「いま何があるか」「実行すると何をするか」を表示するだけで、
ダウンロードもコピーもショートカット作成も行いません。
この PC のように本番環境で、まず安全性を確認したいときに使います。

`-DryRun` の最後には、**ダウンロードする予定の一覧**（URL・サイズ・ライセンス）が出ます。
配布する前に「何を取得させることになるか」を確認するのに使ってください。

```text
ダウンロード予定の一覧（配布前の確認用）
[必要] cuFFT（pip: nvidia-cufft>=12,<14）（約 200 MB / NVIDIA CUDA EULA）
       https://pypi.org/project/nvidia-cufft/
[必要] FFmpeg LGPL 共有ビルド（ffmpeg / ffprobe / av*.dll）（約 77 MiB / LGPL v2.1 以降）
       https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/...
※ cuFFT は bin に同梱されていれば取得しません（無い PC だけ nvidia-cufft を取得）。
```

主なオプション:

| オプション | 意味 |
| --- | --- |
| `-DryRun` | 何も変更せず確認だけ（ダウンロード予定の一覧も出る） |
| `-Yes` | すべての確認に「はい」（無人実行向け） |
| `-NonInteractive` | 確認せず既定値で進む |
| `-SkipFfmpeg` | ffmpeg の差し替えをしない |
| `-NoShortcuts` | ショートカットを作らない |
| `-RamDisk` | RAM ディスクの登録まで行う |
| `-NoRamDisk` | RAM ディスクの確認を出さない |
| `-Port 8766` | 使うポートを変える |
| `-Diagnose` | 診断モード（`launch.cmd` の失敗時に自動で呼ばれます） |

---

## 8. アンインストール

`tools\ppt-setup\uninstall.ps1` を実行します（スタートメニューの
「PPT Lab を削除」からも起動できます）。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\ppt-setup\uninstall.ps1
```

削除するもの（すべて確認されます）:

1. デスクトップ / スタートメニューのショートカット
2. スケジュールタスク「PPT RAM disk」と R: ドライブ（管理者のみ）
3. インストーラが作った作業フォルダー（`cuda` / `python` / `downloads` / `backup`）
4. データ `%LOCALAPPDATA%\PPTLab` と解析結果（**既定では消しません**）

データを消すときだけ、`DELETE` と入力する二重確認があります。
アプリ本体（`server.py` / 解析エンジン / モデル / 設定）は残ります。
完全に消すときはフォルダーごと削除してください。

削除予定だけ確認したいとき:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\ppt-setup\uninstall.ps1 -DryRun
```

---

## 9. 手動インストール（上級者向け）

自動でうまくいかない場合は、次の表のとおりに配置してください。

| 種類 | ファイル | 置き場所 | 入手先 |
| --- | --- | --- | --- |
| 画面 | `server.py`, `web\`, `models\`, `profiles.json` | `tools\ppt-lab\` | 配布物に同梱 |
| 解析エンジン | `TetrisVideoRecovery_stream.exe`, `onnxruntime*.dll` | `tools\video-recovery-runtime\bin` | 配布物に同梱 |
| 映像 | `ffmpeg.exe`, `ffprobe.exe`, `avcodec-63.dll`, `avformat-63.dll`, `avutil-61.dll` | 同上 | [BtbN LGPL 共有ビルド](https://github.com/BtbN/FFmpeg-Builds/releases/latest) |
| CUDA | `cublas64_13.dll`, `cublasLt64_13.dll`, `cudart64_13.dll` | 同上 | [CUDA Toolkit 13.x](https://developer.nvidia.com/cuda-downloads) / pip `nvidia-cublas`, `nvidia-cuda-runtime` |
| cuFFT | `cufft64_12.dll`（無くても CPU モードで起動は可） | 同上 | 配布物に同梱 / CUDA Toolkit / pip `nvidia-cufft` |
| cuDNN | `cudnn64_9.dll`, `cudnn_*64_9.dll` | 同上 | [cuDNN 9](https://developer.nvidia.com/cudnn)（要アカウント）/ pip `nvidia-cudnn-cu13` |
| 設定 | `tetris_recover.ini` | `tools\video-recovery-runtime\config` | 配布物に同梱 |
| Python | Python 3.11 以上の 64bit | どこでも（PATH か既知の場所） | [python.org](https://www.python.org/downloads/windows/) |

配置したら、また `install.cmd -DryRun` を実行して ✅ が増えたことを確認してください。

---

## 10. ライセンスについて（かんたんに）

| ソフト | ライセンス | 配布するときの注意 |
| --- | --- | --- |
| PPT Lab 本体 | リポジトリの `LICENSE` に従う | — |
| ONNX Runtime | MIT | 著作権表示を同梱 |
| FFmpeg（LGPL ビルド） | LGPL v2.1 以降 | ライセンス文と入手元を同梱。DLL のまま動的リンクで使う |
| FFmpeg（GPL ビルド） | GPL v3 | 配布するとソース提供義務が生じる。**LGPL 版を推奨** |
| CUDA / cuBLAS / cuDNN | NVIDIA の各ライセンス | 再配布条件あり。インストーラは**利用者の PC で取得**する方式にしている |

くわしくは `tools\ppt-setup\README.md` を読んでください。

---

## 11. 用語ミニ辞典

| 言葉 | 意味 |
| --- | --- |
| CUDA | NVIDIA の GPU で計算するための仕組み。`cublas64_13.dll` などが必要 |
| cuDNN | ニューラルネットを GPU で速く動かす部品。9 系を使う |
| ONNX Runtime | 学習済みモデル（`.onnx`）を実行するエンジン |
| pip | Python の部品を取ってくる仕組み。NVIDIA の部品もここから取れる |
| LGPL / GPL | ソフトのライセンス。GPL は配布時の条件が厳しい |
| RAM ディスク | メモリ上に作る仮想ドライブ。動画の一時ファイルを置くと速い |
| 冪等（べきとう） | 何度実行しても同じ結果になること。このインストーラは冪等です |

---

困ったときは、まず `%LOCALAPPDATA%\PPTLab\logs` の最新ログを見てください。
`install.ps1 -Diagnose` も、変更せずに状態を日本語で教えてくれます。
