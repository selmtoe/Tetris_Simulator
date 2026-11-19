# tools/ppt-setup — 配布する人向けの手引き

このフォルダーは、**PPT Lab（ぷよテト動画解析ラボ）を他の人に配るためのセットアップ一式**です。
受け取る側は `install.cmd` をダブルクリックするだけで、必要なものがそろいます。
この README は「配る側」が、何を同梱し、何を同梱せず、どんなライセンス義務を負うかを
判断するための資料です。

---

## 1. ファイル一覧

| ファイル | 役割 | 配布物に含める |
| --- | --- | --- |
| `install.cmd` | ダブルクリック用の入口。`install.ps1` を呼ぶだけの薄いラッパー | 含める |
| `install.ps1` | インストーラ本体。8 ステップ・冪等・DryRun・ログ出力 | 含める |
| `launch.cmd` | 起動用。`pythonw` でサーバーを起動し、ブラウザーで画面を開く | 含める（`install.ps1` が自動生成・更新） |
| `uninstall.ps1` | アンインストーラ。ショートカット・タスク・任意でデータを削除 | 含める |
| `README.md` | このファイル | 含める |
| `cuda/` | pip で取得した CUDA / cuDNN の展開先（自動生成） | **含めない**（受け取る PC で取得） |
| `python/` | embeddable Python を展開した場合のみ（自動生成） | **含めない** |
| `downloads/` | ダウンロードした zip / exe の一時置き場（自動生成） | **含めない** |
| `backup/` | 差し替え前の ffmpeg / DLL の退避先（自動生成） | **含めない** |
| `licenses/` | ffmpeg の LGPL ライセンス文と入手元の控え（自動生成） | 含める（生成されていれば） |
| `state.json` | 構成の記録（Python の場所・バージョンなど。自動生成） | 含めない（個人情報を含みうる） |

`cuda/` は 1〜2 GB、`backup/` は数百 MB になることがあります。
配布 ZIP を作るときは、これらを削除してから固めてください。

---

## 2. 受け取る側の体験（1 クリック）

1. ZIP を展開して `tools\ppt-setup\install.cmd` をダブルクリック
2. 日本語の進捗が 8 ステップで表示される
3. 足りないものだけ自動で入る（Python / CUDA / cuDNN / ffmpeg LGPL）
4. 最後に ✅ / ❌ の一覧が出る。❌ には「対処」と「入手先 URL」が併記される
5. デスクトップの「PPT Lab」をダブルクリック → ブラウザーが開く

途中で失敗しても、**「この後やること」**が番号付きで出るので、
利用者は自分で直せます。何度実行しても壊れません（冪等）。

---

## 3. 何をダウンロードするか

インストーラが**利用者の PC 上で**取得するもの（＝配布 ZIP には入れない）:

| 対象 | 入手元 | 目安サイズ | ライセンス | 再配布 |
| --- | --- | --- | --- | --- |
| Python 3.11.9（64bit） | <https://www.python.org/ftp/python/3.11.9/python-3.11.9-amd64.exe> | 26 MB | PSF License | 可（表示保持） |
| Python 3.11.9 embeddable | <https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip> | 11 MB | PSF License | 可（表示保持） |
| get-pip.py | <https://bootstrap.pypa.io/get-pip.py> | 2 MB | MIT | 可 |
| cuBLAS (CUDA 13) | `pip install "nvidia-cublas>=13,<14"` | 約 420 MB | NVIDIA CUDA EULA | 条件付き |
| CUDA Runtime (13) | `pip install "nvidia-cuda-runtime>=13,<14"` | 約 5 MB | NVIDIA CUDA EULA | 条件付き |
| cuDNN 9 | `pip install nvidia-cudnn-cu13==9.12.0.46` | 約 400〜600 MB | NVIDIA cuDNN SLA | 条件付き |
| cuFFT（CUDA 13 系は 12.x） | `pip install "nvidia-cufft>=12,<14"` | 約 200 MB | NVIDIA CUDA EULA | 条件付き |
| FFmpeg 9.0 LGPL 共有ビルド | <https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip> | 73 MiB | LGPL v2.1 以降 | 可（条件あり） |

> **パッケージ名の注意（実測済み）**
> `nvidia-cublas-cu13` / `nvidia-cuda-runtime-cu13` / `nvidia-cuda-nvrtc-cu13` という名前も
> PyPI に存在しますが、**中身は 0.0.1 のダミー**です。実際の CUDA 13 版は
> サフィックス無しの `nvidia-cublas` / `nvidia-cuda-runtime` / `nvidia-cufft` です。
> cuDNN だけは `nvidia-cudnn-cu13` が本物です（9.12.0.46 が実在し、この PC の
> `cudnn64_9.dll` の FileVersion と一致します）。
> NVRTC は `nvidia-cublas` の依存として自動で入るため、個別指定していません。
> cuFFT は `nvidia-cufft-cu12`（11.x 系）ではなく **`nvidia-cufft`（12.x）** を使ってください。

ポイント:

- **Python は公式インストーラを優先**します。`InstallAllUsers=0` なので管理者権限は不要です。
  どうしても入らない環境向けに、レジストリを触らない embeddable zip も選べます。
- **CUDA / cuDNN は PyPI の NVIDIA 公式 wheel** を使うので、NVIDIA アカウントが要りません。
  （cuDNN の公式サイト配布物はアカウント必須なので、フォールバックとして案内だけしています）
- **cuFFT（`cufft64_12.dll`）は 3 段構え**です。`bin` に同梱されていればそれを使い、
  無ければシステムの CUDA Toolkit からコピー、それも無い PC だけ
  `pip install nvidia-cufft`（12.x）で取得して `bin` にコピーします。
  配布物に同梱しておけば、そもそもダウンロードは発生しません。
- **ffmpeg は LGPL の「shared」ビルド**を選びます。`shared` でないと `avcodec-63.dll` などが
  入らず、既存の C++ 実行ファイルが動きません。

---

## 4. バージョンを合わせる考え方（重要）

このアプリは、**同梱バイナリが要求する DLL 名**に厳密に依存します。
インストーラは次の順で「既存に合わせる」動きをします。

1. `onnxruntime_providers_cuda.dll` の中の**インポート名**を読み取る
   （例: `cublas64_13` / `cublasLt64_13` / `cudnn64_9` / `cufft64_12`）
2. その名前から **CUDA のメジャー（13）と cuDNN のメジャー（9）** を決める
3. `bin` に無ければ、システムの CUDA Toolkit（`...\CUDA\v13.x\bin\x64`）からコピー
4. それでも無ければ `pip install --target tools\ppt-setup\cuda "nvidia-cublas>=13,<14" ...`
5. cuDNN は**既存 `cudnn64_9.dll` の FileVersion に固定**（例 `==9.12.0.46`）。
   手元に cuDNN が無い PC では、動作確認済みの `9.12.0.46` に固定します
6. 最後に**インポート名と `bin` の DLL を突き合わせて表示**し、足りない名前に ❌ と対処を出す

したがって、**この配布物の前提は CUDA 13 系 / cuDNN 9.12 系**です。
`onnxruntime_providers_cuda.dll` を差し替えた場合は、この前提も見直してください。

cuFFT は「bin → システムの CUDA Toolkit → pip（`nvidia-cufft`）→ ❌」の順で調達します。
CUDA Toolkit を入れていない配布先でも、pip から 12.x を取って `bin` にコピーできるので、
そこで止まりません（配布物に同梱しておけば取得自体が発生しません）。

FFmpeg は soname（`avcodec-63` / `avformat-63` / `avutil-61`）が一致するビルドだけを採用します。
一致しない場合は差し替えを中止し、既存を維持します（起動不能を避けるため）。

---

## 5. 配布前チェックリスト

- [ ] `install.cmd -DryRun` が **終了コード 0** で終わる（❌ が必須項目に無い）
- [ ] `install.ps1` / `uninstall.ps1` が **UTF-8 BOM 付き**である
      （BOM が無いと Windows PowerShell 5.1 が CP932 と誤認して日本語が壊れます）
- [ ] `install.cmd` / `launch.cmd` が **BOM 無し**で、**コメントも ASCII のみ**である
      （cmd.exe は .cmd を OEM コードページで読むため、日本語は化けます。
      日本語のメッセージは `install.ps1` 側で出します）
- [ ] `cuda/` `python/` `downloads/` `backup/` `state.json` を削除した
- [ ] `ffmpeg.exe` が LGPL 版になっている
      （`tools\video-recovery-runtime\bin\ffmpeg.exe -version` の `configuration:` に
      `--enable-gpl` が無いこと）
- [ ] `licenses/FFmpeg-LICENSE.txt` と `FFmpeg-LGPL-NOTICE.txt` を同梱した
- [ ] `THIRD_PARTY_NOTICES.md` / `LICENSE` を同梱した
- [ ] 別の PC（CUDA 未導入）で一度、実際にインストールして起動を確認した

`-DryRun` の実行例:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\ppt-setup\install.ps1 -DryRun
```

---

## 6. ライセンス上の注意

### FFmpeg

- **LGPL 共有ビルド**（BtbN）を採用しています。DLL のまま動的リンクで使う限り、
  アプリ本体のソース公開義務は発生しません。
- 配布時は次を同梱してください。
  - `licenses/FFmpeg-LICENSE.txt`（インストーラが自動で保存）
  - `licenses/FFmpeg-LGPL-NOTICE.txt`（入手元 URL とソース入手先）
- **GPL ビルド（gyan.dev の full_build など）を配布しないでください。**
  GPL はソース提供義務があり、配布物全体のライセンスにも影響します。
- なお、`tools\ppt-lab\server.py` などは ffmpeg を「別プロセスとして実行」するだけなので、
  LGPL の動的リンク条件で問題ありません。

### CUDA / cuBLAS / cuDNN

- NVIDIA のライセンスは**再配布に条件**があります（同梱した場合、EULA の同梱や
  バージョン通知など）。このインストーラは**利用者の PC で取得する方式**にしてあるため、
  配布 ZIP に NVIDIA のバイナリを含めません。
- どうしても同梱したい場合は、NVIDIA の EULA / SLA を読み、
  `licenses\` に全文を入れてください（`配布用\テトリス動画解析\licenses\CUDA-EULA.txt` が参考になります）。

### ONNX Runtime / モデル

- ONNX Runtime は MIT です。`THIRD_PARTY_NOTICES.md` の記載を維持してください。
- `models\*.onnx` はこのプロジェクトの成果物です。`docs\` の記載に従ってください。

---

## 7. 設計メモ（メンテナ向け）

- **冪等**: すべての処理が「あるか確認 → 無ければ実行」。既存ファイルは
  `backup\<tag>` に退避してから置き換えます。
- **DryRun / -WhatIf / -Diagnose**: どれも「変更しない」モードです。
  `-Diagnose` は `launch.cmd` の失敗時に自動で呼ばれます。
  DryRun の最後には**ダウンロード予定の一覧（URL・サイズ・ライセンス）**を出すので、
  配布前に「何を取得させるか」を確認できます。
- **ログ**: `%LOCALAPPDATA%\PPTLab\logs\install-YYYYMMDD-HHMMSS.log`（UTF-8 BOM）。
  コンソールが CP932 でも読めるように、ログは常に UTF-8 です。
- **終了コード**: `0` 成功 / `2` 必須ファイル不足 / `3` 利用者が中止 / `1` 予期しないエラー。
  `install.cmd` はこの値を見て、必要なときだけウィンドウを止めます。
- **表示**: コンソールが UTF-8 でなければ ✅/❌ を `[OK]`/`[NG]` に自動で落とします
  （`PPTLAB_ASCII=1` で強制も可）。
- **文字コード**: `.ps1` / `.md` は UTF-8 **BOM 付き**、`.cmd` は UTF-8 **BOM 無し**。
  さらに `.cmd` は **ASCII のみ**（日本語コメントも書かない）。cmd.exe は OEM コードページで
  読むため、日本語は必ず化けます。日本語のメッセージは `install.ps1` 側で出してください。
- **PowerShell 5.1 互換**: 三項演算子 `? :`、`??`、`Join-Path` の 3 引数、
  `Add-Type` 以外の新しい構文は使っていません。`@(...)` で作った配列に `.Add()` は
  できません（`ArrayList` を使うこと）。

---

## 8. 配布者がハマりやすい点

### Q. 受け取った人に「スクリプトの実行が無効」と言われた

`install.cmd` 経由なら `-ExecutionPolicy Bypass` が付くので起きません。
`install.ps1` を直接右クリック実行した場合に起きます。`install.cmd` を使うよう案内してください。

### Q. SmartScreen で止められる

署名がないためです。配布時は「詳細情報 → 実行」を案内するか、
コード署名証明書の利用を検討してください（`docs\INSTALL.md` の Q2 参照）。

### Q. pip の取得が遅い / 失敗する

`cuda/` を作り直すと数 GB の再取得になります。配布前に `cuda/` を消すのは、
**配布 ZIP を小さくするため**であり、手元の検証環境では残しておくと再実行が速くなります。

### Q. pip で「そんなパッケージは無い」と言われる

PyPI の名前は `-cu13` の有無で意味が変わります（実測済み）。

| 名前 | 実態 |
| --- | --- |
| `nvidia-cublas` / `nvidia-cuda-runtime` | **本物**（13.8.0.4 / 13.4.92 などが実在） |
| `nvidia-cublas-cu13` / `nvidia-cuda-runtime-cu13` | 存在するが **0.0.1 のダミー** |
| `nvidia-cudnn-cu13` | **本物**（9.12.0.46 などが実在） |
| `nvidia-cufft` | **本物**（12.4.0.43 など。CUDA 13 系の cuFFT） |
| `nvidia-cufft-cu12` | 11.x 系なので **使わない** |
| `nvidia-cuda-nvrtc-cu13` | ダミー（NVRTC は `nvidia-cublas` の依存として入る） |

`install.ps1` の `Step-Cuda` はこの名前を使っています。将来 NVIDIA が命名を変えた場合は、
`Step-Cuda` の `$pipPackages` / `$cufftPackage` を組み立てている数行だけを直してください。

### Q. cuFFT（cufft64_12.dll）は入れなくていい？

`onnxruntime_providers_cuda.dll` は `cufft64_12` をインポートしているので、
**ファイルとしては必要**です。インストーラは次の順で調達します。

1. `bin` に同梱されていればそれを使う
2. 無ければシステムの CUDA Toolkit（`...\CUDA\v13.x\bin\x64`）からコピー
3. それも無ければ `pip install "nvidia-cufft>=12,<14"` で取得し、`cufft64_12.dll` /
   `cufftw64_12.dll` を `bin` にコピー
4. どれも駄目なら ❌ と「別 PC の `bin` からコピー」を表示

3 のおかげで、**CUDA Toolkit を入れていない配布先でも止まりません**。
`-DryRun` では、1〜2 で足りない場合に限り `nvidia-cufft` がダウンロード予定に載ります。

### Q. `avcodec` のメジャーが違うと言われる

`onnxruntime` や `TetrisVideoRecovery_stream.exe` を更新して soname が変わった可能性があります。
`install.ps1` の `$script:FfmpegUrlDefault` を、同じ soname を持つ LGPL 共有ビルドへ変更してください
（例: FFmpeg 9.0 系 → `avcodec-63`）。

---

## 9. 変更履歴

| 版 | 日付 | 内容 |
| --- | --- | --- |
| 1.0.0 | 2026-11-03 | 初版。8 ステップ・冪等・DryRun・LGPL 差し替え・ショートカット・任意 RAM ディスク |
| 1.0.1 | 2026-11-03 | pip のパッケージ名を実在する `nvidia-cublas` / `nvidia-cuda-runtime` に修正。cuFFT の存在チェックを追加。インポート名との突き合わせ表示を追加。`.cmd` を ASCII のみに変更。DryRun にダウンロード予定一覧を追加。失敗時の CPU モード代替の案内を追加 |
| 1.0.2 | 2026-11-03 | cuFFT の調達に pip `nvidia-cufft`（12.x）を追加（bin → システム CUDA → pip → ❌ の順）。CUDA Toolkit 未導入の配布先でも止まらないようにした。DryRun のダウンロード予定と README に cuFFT を追記 |

---

## 10. 関連ドキュメント

- `docs\INSTALL.md` — 受け取る人向けの手順と「うまくいかないとき」
- `docs\PPT-LAB.md` — アプリ本体の説明
- `tools\ppt-lab\README.md` — 画面と API の説明
- `THIRD_PARTY_NOTICES.md` — 同梱物のライセンス一覧
