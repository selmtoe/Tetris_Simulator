import hashlib, json, sys
from pathlib import Path
base = Path(__file__).resolve().parent
manifest = json.loads((base / 'MANIFEST.json').read_text(encoding='utf-8'))
bad = 0
for rel, info in manifest['files'].items():
    path = base / rel
    if not path.is_file():
        print('MISSING', rel); bad += 1; continue
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    if h.hexdigest() != info['sha256']:
        print('CHANGED', rel); bad += 1
print('OK' if not bad else f'{bad} problem(s)', len(manifest['files']), 'files')
sys.exit(1 if bad else 0)
