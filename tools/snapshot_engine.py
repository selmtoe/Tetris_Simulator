"""ビルドと実行条件のスナップショットを残す。

  python tools/snapshot_engine.py --label v48-goexit

やること:
  1. 現在 bin にあるエンジン実行ファイルの SHA-256 を計算
  2. frozen/engines/<sha12>_<label>.exe として保存（同名ならスキップ）
  3. モデル・設定・gitコミットをまとめて frozen/ENGINE-SNAPSHOTS.jsonl に1行追記

これにより「どのビルドが、どのモデル・設定・ソースで作られたか」が常に残る。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from datetime import datetime, timezone, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'tools' / 'video-recovery-runtime' / 'bin'
MODELS = ROOT / 'tools' / 'video-recovery-runtime' / 'models'
CONFIG = ROOT / 'tools' / 'video-recovery-runtime' / 'config'
SCENE = ROOT / 'tools' / 'ppt-lab' / 'models' / 'fast320' / 'scene-classifier.onnx'
ARCHIVE = ROOT / 'frozen' / 'engines'
LOG = ROOT / 'frozen' / 'ENGINE-SNAPSHOTS.jsonl'


def sha256(path: Path) -> str:
    if not path.is_file():
        return ''
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def git_head() -> str:
    try:
        out = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'],
                             capture_output=True, text=True, timeout=30)
        return out.stdout.strip()
    except Exception:
        return ''


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--label', required=True, help='このビルドの識別名（例: v48-goexit）')
    ap.add_argument('--note', default='')
    ap.add_argument('--exe', help='記録するexe（省略時は bin の配備中エンジン）')
    args = ap.parse_args()

    engine = Path(args.exe) if args.exe else BIN / 'TetrisVideoRecovery_stream.exe'
    if not engine.is_file():
        raise SystemExit(f'エンジンが見つかりません: {engine}')
    engine_sha = sha256(engine)
    ARCHIVE.mkdir(parents=True, exist_ok=True)
    target = ARCHIVE / f'{engine_sha[:12]}_{args.label}.exe'
    if not target.is_file():
        shutil.copy2(engine, target)

    record = {
        'savedAt': datetime.now(timezone(timedelta(hours=9))).isoformat(timespec='seconds'),
        'label': args.label,
        'note': args.note,
        'engine': {'sha256': engine_sha, 'archived': str(target.relative_to(ROOT)),
                   'deployed': engine.resolve() == (BIN / 'TetrisVideoRecovery_stream.exe').resolve()},
        'nextModel': sha256(MODELS / 'next_hold.onnx'),
        'boardModel': sha256(MODELS / 'tetris.onnx'),
        'sceneModel': sha256(SCENE),
        'ini': sha256(CONFIG / 'tetris_recover.ini'),
        'git': git_head(),
    }
    with LOG.open('a', encoding='utf-8') as handle:
        handle.write(json.dumps(record, ensure_ascii=False) + '\n')
    print(json.dumps(record, ensure_ascii=False, indent=1))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
