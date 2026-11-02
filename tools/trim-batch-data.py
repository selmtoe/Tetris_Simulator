"""解析バッチのデータを軽くする。Lab の機能に必要な情報だけ残す。

消すもの（どちらも Lab の画面では使っていないことをコードで確認済み）:
  - `<match>/simulator/*_tetris_recovered.json` の `queueObservations`
      NEXT 欄を毎フレーム読み取った生ログ。Lab の再生画面が読むのは
      `simulatorData` と `urls` だけ（`tools/ppt-lab/web/app.js` の /replay/ 参照）。
      1 試合あたり 5.6MB のうち 5.5MB を占める。
  - `<match>/autosave/latest_editing_autosave.v2.previous.json`
      autosave を上書きする前の控え。Lab は `latest_editing_autosave.v2.json` だけ読む。

残すもの: `simulatorData`, `p1`, `p2`, `playerNames`, `urls`, `sourceRange` など。

使い方:
  python tools/trim-batch-data.py --dry-run
  python tools/trim-batch-data.py --apply
  python tools/trim-batch-data.py --apply --exclude d7Nj5pvqPA
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEFAULT_BATCH_ROOT = ROOT / 'video-recovery-runtime' / 'data' / 'batches'
KEEP_KEYS = ('version', 'pageFormat', 'input', 'sourceRange', 'algorithm',
             'playerNames', 'p1', 'p2', 'simulatorData', 'urls')


def human(size: float) -> str:
    return f'{size / 1024 / 1024:.1f} MB'


def trim_recovered(path: Path, apply: bool) -> tuple[int, int]:
    """戻り値: (変更前バイト数, 変更後バイト数)。apply=False なら書き換えない。"""
    before = path.stat().st_size
    try:
        data = json.loads(path.read_text(encoding='utf-8-sig'))
    except (OSError, json.JSONDecodeError):
        return before, before
    if not isinstance(data, dict) or 'queueObservations' not in data:
        return before, before
    trimmed = {key: data[key] for key in KEEP_KEYS if key in data}
    # 未知のキーは残す（将来の形式追加で壊さない）
    for key, value in data.items():
        if key not in trimmed and key != 'queueObservations':
            trimmed[key] = value
    text = json.dumps(trimmed, ensure_ascii=False, separators=(',', ':'))
    after = len(text.encode('utf-8'))
    if apply:
        temp = path.with_suffix(path.suffix + '.tmp')
        temp.write_text(text, encoding='utf-8')
        os.replace(temp, path)
    return before, after


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--batch-root', type=Path, default=DEFAULT_BATCH_ROOT)
    parser.add_argument('--batch', type=Path, default=None,
                        help='1バッチだけ処理する（解析直後の自動整理に使う）')
    parser.add_argument('--apply', action='store_true', help='実際に書き換える')
    parser.add_argument('--dry-run', action='store_true', help='何もしないで集計だけ')
    parser.add_argument('--exclude', action='append', default=[],
                        help='バッチ名に含まれていたら触らない（例: --exclude d7Nj5pvqPA）')
    args = parser.parse_args()
    apply = args.apply and not args.dry_run
    root = args.batch_root
    if args.batch is not None:
        if not args.batch.is_dir():
            print(f'バッチがありません: {args.batch}')
            return 1
        root = args.batch.parent
        single = args.batch
    else:
        single = None
    if not root.is_dir():
        print(f'バッチ置き場がありません: {root}')
        return 1

    total_before = total_after = 0
    json_files = prev_files = 0
    per_batch: list[tuple[str, int, int, int]] = []
    for batch in ([single] if single is not None else sorted(p for p in root.iterdir() if p.is_dir())):
        if any(token in batch.name for token in args.exclude):
            print(f'[skip] {batch.name[:40]}（除外指定）')
            continue
        b_before = b_after = b_json = 0
        for path in batch.rglob('*_tetris_recovered.json'):
            before, after = trim_recovered(path, apply)
            b_before += before
            b_after += after
            b_json += 1
        for path in batch.rglob('*.previous.json'):
            size = path.stat().st_size
            b_before += size
            prev_files += 1
            if apply:
                path.unlink(missing_ok=True)
        total_before += b_before
        total_after += b_after
        json_files += b_json
        if b_json or b_before:
            per_batch.append((batch.name, b_json, b_before, b_after))

    print(f'recovered.json: {json_files} ファイル / autosave控え: {prev_files} ファイル')
    for name, count, before, after in per_batch:
        if before - after < 1024 * 1024:
            continue
        print(f'  {name[:34]:34} {count:4}件  {human(before):>9} -> {human(after):>9}')
    print(f'合計: {human(total_before)} -> {human(total_after)}'
          f'（{human(total_before - total_after)} 削減予定）')
    print('適用しました' if apply else 'ドライラン（--apply で実行）')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
