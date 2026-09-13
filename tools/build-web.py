"""Build the public static tools using the approved appearance, without a Lab server.

An explicit allowlist is the deployment boundary. No datasets, native CUDA
runtime, analysis results, personal records or repository metadata are copied.
"""
import argparse
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('web_appearance', ROOT / 'tools/tetris-lab/preview_appearance.py')
appearance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(appearance)

DIRECTORIES = ('F/app', 'F/styles', 'styles', 'simulator/app', 'simulator/workers', 'icons')
FILES = (
    'index.html', 'F/index.html', 'manifest.webmanifest', 'sw.js',
    'hub/index.html', 'hub/workspace.css', 'hub/js/workspace.js', 'hub/js/workflow.js', 'hub/js/recovery.js',
    'hub/manifest.json', 'hub/sw.js', 'hub/icon-192.png', 'hub/icon-512.png',
    'shared/workspace-entry.js', 'shared/tetris-event-codec.js', 'shared/cell-cnn-inference.js',
    'simulator/pc-solver/sfinder-pc.js', 'simulator/pc-solver/sfinder-pc.wasm',
    'simulator/candidate/kasane-stack-ren-candidate-harness.js',
    'simulator/candidate/kasane-stack-ren-telemetry.js',
    'Load PPT/tetris.onnx', 'Load PPT/tetris.model.json',
)
EXTENSIONS = {'.html', '.css', '.js', '.wasm', '.png', '.jpg', '.svg', '.webp', '.ico'}

# The approved tools preview supplies these labels through its Lab bridge.
# Publish the presentation alone, without the personal Lab integration.
TOOL_LABELS = {
    'view-mode-btn': '再生で確認', 'back-to-editor-btn': '盤面を編集',
    'send-to-simulator': 'ここから練習', 'viewer-simulator-btn': 'ここから練習',
    'prev-page': '前の局面', 'next-page': '次の局面', 'new-page': '局面を追加',
    'delete-page': 'この局面を削除', 'new-snapshot-case': '別の局面集を作る',
    'new-replay-case': '別の対戦記録を作る',
}
for player in ('p1', 'p2'):
    for name, label in {'next-delete-left': '先頭を削除', 'next-clear': 'NEXTを空にする', 'field-clear': '盤面を空にする'}.items():
        TOOL_LABELS[f'{player}-{name}'] = label


def approved_labels(html):
    for element_id, label in TOOL_LABELS.items():
        pattern = r'(<button\b[^>]*\bid="' + re.escape(element_id) + r'"[^>]*>)[^<]*(</button>)'
        html = re.sub(pattern, lambda match: match[1] + label + match[2], html)
    return html


def build(output):
    output = output.resolve()
    # Refuse to clean any directory other than this dedicated generated tree.
    expected = (ROOT / 'dist/pages').resolve()
    expected.relative_to(ROOT.resolve())
    if output != expected:
        raise ValueError(f'Build output must be the dedicated directory: {expected}')
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    paths = set(FILES)
    for directory in DIRECTORIES:
        paths.update(path.relative_to(ROOT).as_posix() for path in (ROOT / directory).rglob('*')
                     if path.is_file() and path.suffix in EXTENSIONS)
    for relative in sorted(paths):
        source = ROOT / relative
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if '/' + relative in appearance.SCRIPT_PATHS:
            destination.write_text(appearance.script_variant('/' + relative, source.read_text(encoding='utf-8')), encoding='utf-8')
        elif relative in ('index.html', 'F/index.html'):
            prefix = '../' if relative.startswith('F/') else './'
            role = 'viewer' if relative.startswith('F/') else 'simulator'
            html = source.read_text(encoding='utf-8')
            if role == 'viewer':
                html = approved_labels(html)
            entry = f'<script src="{prefix}shared/workspace-entry.js" data-app="{role}"></script>'
            html = html.replace('<head>', '<head>\n' + entry, 1)
            html = html.replace('</head>', f'<link rel="stylesheet" href="{prefix}shared/appearance.css"><script src="{prefix}shared/appearance.js"></script></head>', 1)
            destination.write_text(html, encoding='utf-8')
        elif relative == 'hub/index.html':
            html = source.read_text(encoding='utf-8-sig').replace('../tools/tetris-lab/preview-theme.css', '../shared/appearance.css')
            destination.write_text(html, encoding='utf-8')
        else:
            shutil.copyfile(source, destination)
    for source, target in [('preview-theme.css', 'appearance.css'), ('preview-appearance.js', 'appearance.js')]:
        shutil.copyfile(ROOT / 'tools/tetris-lab' / source, output / 'shared' / target)
    (output / '.nojekyll').write_text('', encoding='utf-8')
    manifest = {path.relative_to(output).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(output.rglob('*')) if path.is_file()}
    (output / 'build-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'Built {len(manifest)} static files in {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'dist/pages')
    build(parser.parse_args().output)
