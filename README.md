# Tetris Simulator

テトリスのシミュレータ・譜面エディタと、ぷよぷよテトリス2の配信映像を解析する PPT Lab のリポジトリ。

| 場所 | 中身 |
|---|---|
| `index.html`, `simulator/`, `styles/` | シミュレータ（公開: GitHub Pages） |
| `F/` | 譜面ビューワー・エディタ |
| `hub/`, `shared/` | ワークスペースと共通の部品 |
| `ai-lab/`, `third_party/`, `licenses/` | AI（Cold Clear・GA）と、そのライセンス |
| `Load PPT/` | 盤面読み取りのモデル |
| `tools/ppt-lab/` | PPT Lab（配信の解析・名前確認・統計）。公開Lab は `tools/build-lab.py` で作る |
| `tools/video-recovery-source/` | 映像解析エンジン（C++） |
| `frozen/` | 配備したエンジンとモデルの記録、回帰テストの結果 |
| `docs/` | 説明書き |

公開サイトのビルドは `python tools/build-web.py`（確認は `python tools/test-web-build.py`）。
