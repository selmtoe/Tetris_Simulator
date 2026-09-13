"""Validate the generated public artifact and the exact approved appearance."""
import hashlib
from html.parser import HTMLParser
import importlib.util
import json
from pathlib import Path
import re
from urllib.parse import unquote, urljoin, urlsplit

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'dist/pages'
spec = importlib.util.spec_from_file_location('appearance', ROOT / 'tools/tetris-lab/preview_appearance.py')
appearance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(appearance)
checks = 0


def check(condition, message):
    global checks
    assert condition, message
    checks += 1


class AssetParser(HTMLParser):
    def __init__(self, page):
        super().__init__()
        self.page = page

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        url = attrs.get('src') if tag == 'script' else attrs.get('href') if tag == 'link' else None
        if not url or urlsplit(url).netloc:
            return
        resolved = urlsplit(urljoin('/Tetris_Simulator/' + self.page, url)).path
        check(resolved.startswith('/Tetris_Simulator/'), f'Root-absolute asset breaks Pages prefix: {url}')
        check((OUTPUT / unquote(resolved.removeprefix('/Tetris_Simulator/'))).is_file(), f'Missing asset: {url}')


manifest = json.loads((OUTPUT / 'build-manifest.json').read_text())
for relative, expected in manifest.items():
    data = (OUTPUT / relative).read_bytes()
    check(hashlib.sha256(data).hexdigest() == expected, f'Artifact changed: {relative}')
    check(not relative.startswith(('tools/', 'kasane/', '.git/', '配布用/')), f'Non-public subtree: {relative}')
    if relative.endswith('.html'):
        AssetParser(relative).feed(data.decode('utf-8-sig'))

for name in appearance.SCRIPT_PATHS:
    expected = appearance.script_variant(name, (ROOT / name.lstrip('/')).read_text(encoding='utf-8'))
    check((OUTPUT / name.lstrip('/')).read_text(encoding='utf-8') == expected, f'Appearance mismatch: {name}')
for native, public in [('preview-theme.css', 'appearance.css'), ('preview-appearance.js', 'appearance.js')]:
    check((ROOT / 'tools/tetris-lab' / native).read_bytes() == (OUTPUT / 'shared' / public).read_bytes(), f'Approved appearance changed: {native}')
html = (OUTPUT / 'hub/index.html').read_text(encoding='utf-8')
check('save-replay-form' not in html and 'record-list' not in html, 'Hub must not expose save/library UI')
for path in ('hub/js/workspace.js', 'hub/js/recovery.js'):
    check(not re.search(r'tetrisHub(?:Auto)?Data', (OUTPUT / path).read_text(encoding='utf-8')), 'Old saved records must remain untouched')
check((OUTPUT / 'hub/js/recovery.js').is_file(), 'Recovery must be included')
print(json.dumps({'passed': True, 'checks': checks, 'files': len(manifest)}))
