"""解析モデルを配布用の `.tetracemodel`（ZIP）にまとめる。

使い方:
    python tools/ppt-lab/pack-model.py --model tools/ppt-lab/models/fast320
    python tools/ppt-lab/pack-model.py --model <dir> --out dist/models

中身は「manifest.json + マニフェストに書かれた *.onnx」だけ。
Lab の「モデルを追加」ボタンはこのファイルを受け取って検証・展開する
（展開と検証は `models_registry.py` 側）。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

SCHEMA = 'tetrace-model/v1'
REQUIRED = ('scene', 'board')


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest().upper()


def verify(model_dir: Path) -> tuple[dict, list[str]]:
    manifest_path = model_dir / 'manifest.json'
    if not manifest_path.is_file():
        raise SystemExit(f'manifest.json がありません: {manifest_path}')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8-sig'))
    if manifest.get('schema') != SCHEMA:
        raise SystemExit(f'manifest.json の schema が {SCHEMA} ではありません: {manifest.get("schema")}')
    for key in ('id', 'name', 'version'):
        if not str(manifest.get(key) or '').strip():
            raise SystemExit(f'manifest.json に {key} がありません')
    files: dict[str, str] = dict(manifest.get('files') or {})
    for key in REQUIRED:
        if key not in files:
            raise SystemExit(f'manifest.json の files に {key} がありません')
    problems: list[str] = []
    hashes = dict(manifest.get('sha256') or {})
    for role, name in files.items():
        path = model_dir / name
        if not path.is_file():
            problems.append(f'{role}: ファイルがありません（{name}）')
            continue
        actual = sha256_of(path)
        expected = str(hashes.get(name) or '').upper()
        if expected and expected != actual:
            problems.append(f'{role}: sha256 が一致しません（{name}）')
        hashes[name] = actual
    manifest['sha256'] = hashes
    return manifest, problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, required=True, help='モデルのフォルダ（manifest.json がある場所）')
    parser.add_argument('--out', type=Path, default=Path('dist/models'), help='出力先フォルダ')
    parser.add_argument('--keep-builtin', action='store_true',
                        help='配布物にも builtin: true を残す（既定は外して、導入先で削除できるようにする）')
    args = parser.parse_args()

    model_dir = args.model
    manifest, problems = verify(model_dir)
    if problems:
        print('検証で問題が見つかりました:')
        for item in problems:
            print('  -', item)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    # 配布物は「導入先で削除できる通常モデル」として扱う（builtin を外す）。
    if not args.keep_builtin:
        manifest.pop('builtin', None)
    stem = str(manifest['name']).strip().replace(' ', '_').replace('/', '_')
    target = args.out / f'{stem}.tetracemodel'

    files: dict[str, str] = dict(manifest.get('files') or {})
    extra = [p for p in model_dir.glob('*.onnx') if p.name not in files.values()]
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.writestr('manifest.json', json.dumps(manifest, ensure_ascii=False, indent=2))
        for name in files.values():
            archive.write(model_dir / name, arcname=name)
        for path in extra:
            archive.write(path, arcname=path.name)

    size = target.stat().st_size
    print(f'作成しました: {target} ({size / 1024:.1f} KB)')
    print(f'  モデル: {manifest["name"]} / {manifest["id"]} v{manifest["version"]}')
    for role, name in files.items():
        print(f'  {role}: {name}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
