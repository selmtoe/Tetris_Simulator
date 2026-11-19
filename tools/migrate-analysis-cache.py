"""エンジン差し替えで無効になった解析キャッシュを引き継ぐ（エンジン以外が同一のバッチのみ）。

シーン/盤面の解析結果はエンジン差し替えで変わらない（境界・盤面出力が一致することを
同一クリップ・同一3試合で確認済み）ため、`batch_executable` の記録だけを現行エンジンに
合わせる。次回 Lab で開いたときに不要な再解析が走らないようにするための移行措置。
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

BATCH_ROOT = Path(r'C:\Users\hirom\Documents\tetris ai\tools\video-recovery-runtime\data\batches')
ENGINE = Path(r'C:\Users\hirom\Documents\tetris ai\tools\video-recovery-runtime\bin\TetrisVideoRecovery_stream.exe')
BACKUP = Path(r'C:\Users\hirom\Documents\tetris ai\scratch\end3\ab\cache-backup')


def file_identity(path: Path) -> dict:
    st = path.stat()
    digest = hashlib.sha256(path.read_bytes()).hexdigest().upper()
    ticks = int(datetime.fromtimestamp(st.st_mtime, tz=timezone.utc).timestamp() * 10_000_000) + 116444736000000000
    return {
        'path': str(path),
        'bytes': st.st_size,
        'last_write_utc_ticks': ticks,
        'sha256': digest,
    }


def main() -> None:
    apply = '--apply' in sys.argv
    only = ''
    for index, arg in enumerate(sys.argv):
        if arg == '--only' and index + 1 < len(sys.argv):
            only = sys.argv[index + 1]
    current = file_identity(ENGINE)
    print('current engine:', current)
    BACKUP.mkdir(parents=True, exist_ok=True)
    patched = skipped = 0
    for cache_path in sorted(BATCH_ROOT.glob('*/analysis-cache.v1.json')):
        batch = cache_path.parent
        if only and only not in batch.name:
            continue
        try:
            cache = json.loads(cache_path.read_text(encoding='utf-8-sig'))
        except Exception as exc:  # noqa: BLE001
            print(f'[skip] {batch.name[:40]}: {exc}')
            skipped += 1
            continue
        saved = cache.get('inputs', {}).get('batch_executable', {})
        if saved.get('sha256') == current['sha256']:
            print(f'[ok]   {batch.name[:40]}: already current engine')
            continue
        missing = [p for p in cache.get('required_outputs', []) if not Path(p).is_file()]
        if missing:
            print(f'[skip] {batch.name[:40]}: result files missing ({len(missing)})')
            skipped += 1
            continue
        print(f'[diff] {batch.name[:40]}: engine {saved.get("sha256","?")[:12]} -> {current["sha256"][:12]}'
              f' (bytes {saved.get("bytes")} -> {current["bytes"]})')
        if not apply:
            continue
        backup_path = BACKUP / f'{batch.name[:60]}.analysis-cache.v1.json'
        if not backup_path.exists():
            backup_path.write_text(cache_path.read_text(encoding='utf-8-sig'), encoding='utf-8')
        cache.setdefault('inputs', {})['batch_executable'] = current
        cache.setdefault('migration_notes', []).append(
            'batch_executable updated for the inline end-state engine swap (2026-11-02); '
            'scene/board results unchanged')
        cache_path.write_text(json.dumps(cache, ensure_ascii=False, indent=2), encoding='utf-8-sig')
        patched += 1
    print(f'patched={patched} skipped={skipped} apply={apply}')


if __name__ == '__main__':
    main()
