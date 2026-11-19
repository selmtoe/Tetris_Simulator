"""PPT Lab のブラウザ版（閲覧専用）を、公開Lab（selmtoe.github.io/Lab）用に組み立てる。

    py -3.11 tools/build-lab.py            # tools/ppt-lab/webdata を使って dist/lab に組み立てる
    py -3.11 tools/build-lab.py --export   # 先に export_web.py でデータを書き出してから組み立てる

- 画面は tools/ppt-lab/web（手元のデスクトップ版と同じもの）。データは webdata。
- 盤面ビューワー（F/）とシミュレータは、同じサイトの Tetris_Simulator のものを使う
  （window.PPT_SITE_BASE = '/Tetris_Simulator/'）。Lab にはアプリとデータだけを置く。
- 見る人の保存（テト譜・タグ）は static-api.js がその人のブラウザに入れる。
出力（dist/lab）をそのまま Lab リポジトリの中身にする。
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / 'tools/ppt-lab/web'
DATA = ROOT / 'tools/ppt-lab/webdata'
OUT = ROOT / 'dist/lab'
SITE_BASE = '/Tetris_Simulator/'
FILES = ('index.html', 'app.js', 'static-api.js', 'manifest.webmanifest',
         'favicon.ico', 'favicon.png', 'icon-192.png', 'icon-512.png')
DIRECTORIES = ('theme', 'vendor')
# 公開サイトが読み込まないもの（手元のデスクトップ版でも使っていない）。載せない
SKIP_FILES = {'theme/lab-writer.css', 'vendor/viewer-render.js'}
SKIP_DATA = ('downloads',)


def version(html: str) -> str:
    """app.js などの URL に中身のハッシュを付ける（古いキャッシュを使わせない）。"""
    def replace(match):
        url = match.group(2).split('?')[0]
        asset = OUT / url
        if asset.suffix not in ('.js', '.css') or not asset.is_file():
            return match.group(0)
        digest = hashlib.sha256(asset.read_bytes()).hexdigest()[:12]
        return f'{match.group(1)}{url}?v={digest}{match.group(3)}'
    return re.sub(r'((?:src|href)=")([^"#:]+)(")', replace, html)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--export', action='store_true', help='先に webdata を書き出す')
    args = parser.parse_args()
    if args.export:
        subprocess.run([sys.executable, str(ROOT / 'tools/ppt-lab/export_web.py')], check=True)
    if not (DATA / 'videos.json').is_file():
        print('webdata がありません。--export を付けて実行してください。')
        return 1
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    for name in FILES:
        shutil.copyfile(WEB / name, OUT / name)
    for name in DIRECTORIES:
        shutil.copytree(WEB / name, OUT / name)
    for name in SKIP_FILES:
        (OUT / name).unlink(missing_ok=True)
    shutil.copytree(DATA, OUT / 'data', ignore=lambda folder, names: [
        item for item in names if Path(folder) == DATA and item in SKIP_DATA])
    index = OUT / 'index.html'
    html = index.read_text(encoding='utf-8-sig')
    marker = (f'<script>window.PPT_STATIC=true;window.PPT_SITE_BASE={json.dumps(SITE_BASE)};'
              f'window.PPT_DATA_BASE="data/";</script>')
    html = html.replace('<head>', '<head>\n' + marker, 1)
    index.write_text(version(html), encoding='utf-8')
    (OUT / '.nojekyll').write_text('', encoding='utf-8')
    files = [path for path in OUT.rglob('*') if path.is_file()]
    size = sum(path.stat().st_size for path in files)
    print(f'built {len(files)} files, {size / 1024 / 1024:.1f} MB -> {OUT}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
