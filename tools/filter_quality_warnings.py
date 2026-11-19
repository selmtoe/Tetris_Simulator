"""品質警告から「ライン消去アニメーション中の区間」を取り除く後処理。

  python filter_quality_warnings.py --run <batch-dir> [--grace 2.4] [--write]

- 解析は一切やり直さない。`simulator/*_tetris_recovered.json` の
  手ごとの clearedLines を使い、消去直後（既定2.4秒）に完全に収まる
  不一致区間だけを警告から外す。
- --write を付けると quality-warnings.json と batch-results.json を更新する。
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def load(path: Path):
    return json.loads(path.read_text(encoding='utf-8'))


def clear_windows(moves, grace: float, base: float):
    """消去直後の時間窓（動画内の絶対秒）を返す。"""
    windows = []
    for move in moves:
        placement = move.get('placement') or {}
        if int(placement.get('clearedLines') or 0) <= 0:
            continue
        start = base + float(move.get('time') or 0.0)
        windows.append((start, start + grace))
    return windows


def issue_times(issue):
    return float(issue.get('source_start_seconds') or 0.0), float(issue.get('source_end_seconds') or 0.0)


def filter_match(run: Path, index: int, match: dict, grace: float):
    issues = list((match.get('quality_warning') or {}).get('issues') or [])
    if not issues:
        return issues, []
    base = float((match.get('source_range') or {}).get('start_seconds') or 0.0)
    players = {}
    for player in ('p1', 'p2'):
        path = run / f'match_{index:03d}' / 'simulator' / '[yD_ZukVrgHY]_tetris_recovered.json'
        data = load(path) if path.is_file() else {}
        players[player] = clear_windows(data.get(player) or [], grace, base)
    kept, dropped = [], []
    for issue in issues:
        windows = players.get('p%d' % int(issue.get('player') or 1), [])
        start, end = issue_times(issue)
        covered = any(start >= w0 - 0.05 and end <= w1 + 0.05 for w0, w1 in windows)
        (dropped if covered else kept).append(issue)
    return kept, dropped


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', required=True)
    ap.add_argument('--grace', type=float, default=2.4)
    ap.add_argument('--write', action='store_true')
    args = ap.parse_args()
    run = Path(args.run)

    totals = {'matches': 0, 'issues_before': 0, 'issues_after': 0, 'warnings_before': 0, 'warnings_after': 0}
    updated = {}
    for name in ('batch-results.json', 'quality-warnings.json'):
        path = run / name
        if not path.is_file():
            continue
        data = load(path)
        changed = False
        for match in data.get('matches', []):
            q = match.get('quality_warning') or {}
            issues = q.get('issues') or []
            if not issues:
                continue
            kept, dropped = filter_match(run, int(match['index']), match, args.grace)
            if not dropped:
                continue
            q['issues'] = kept
            q['suspicious'] = bool(kept)
            match['quality_warning'] = q
            changed = True
            if name == 'batch-results.json':
                totals['matches'] += 1
                totals['issues_before'] += len(issues)
                totals['issues_after'] += len(kept)
                if dropped:
                    totals['warnings_before'] += 1
                    if kept:
                        totals['warnings_after'] += 1
        if changed:
            updated[name] = data
            if args.write:
                path.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding='utf-8')

    print('対象試合 %d / 区間 %d -> %d / 警告 %d -> %d' % (
        totals['matches'], totals['issues_before'], totals['issues_after'],
        totals['warnings_before'], totals['warnings_after']))
    if not args.write:
        print('（--write を付けるとファイルを更新します）')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
