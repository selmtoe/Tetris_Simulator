"""盤面のずれ（動画から読んだ盤面と、復元した盤面の差）で、壊れた試合を見つける。

エンジンの品質警告は「200マス中の一致率 < 70%（= 60マス以上のずれ）が2手続く」
ときだけ出る。開幕の数手を取りこぼして盤面が最後まで 15〜30 マスずれる試合は
これを下回らず、警告なしで残っていた（たわvsヲヲヲ 試合1 の P2 など）。

基準（2026-11-16、19配信 1,507 試合・3,004 人分の統計から決めた）:
- 1手ごとに、見えている 20 段のうち「ブロックの有無」が違うマスを数える
- ライン消去の直後 0.6 秒・全消し直後 1.6 秒・開始の1手目・終了の手は数えない
  （読めない区間。数えない手は連続を切らない）
- 最後の 4 手（トップアウトで盤面が崩れる区間）は見ない
- 10 マス以上のずれが 6 手以上続いたら「壊れている」
統計では、ずれが 6 手以上続いた試合はすべて実際に壊れていた（開幕の取りこぼしなど）。
5 手以下は、おじゃまのせり上がり中などの一時的なずれで、その後 0 に戻っていた。
"""
from __future__ import annotations

import json
import threading
from pathlib import Path

MISMATCH_CELLS = 10
CONSECUTIVE_PHASES = 6
IGNORE_LAST_PHASES = 4
CLEAR_GRACE_SECONDS = 0.6
PERFECT_CLEAR_GRACE_SECONDS = 1.6
VISIBLE_FROM_ROW = 20
VERSION = 1


def _mismatch(step: dict) -> int:
    observed = step.get('observed') or []
    board = step.get('board') or []
    count = 0
    for row_o, row_b in zip(observed[VISIBLE_FROM_ROW:], board[VISIBLE_FROM_ROW:]):
        for cell_o, cell_b in zip(row_o, row_b):
            if (cell_o != '_') != (cell_b != '_'):
                count += 1
    return count


def player_drift(timeline: list) -> dict | None:
    """1人分の手順から、いちばん長い「ずれの連続」を返す（基準未満なら None）。"""
    measured: list[tuple[float, int]] = []
    clear_until = -1.0
    perfect_until = -1.0
    for index, step in enumerate(timeline):
        try:
            time = float(step.get('time') or 0)
        except (TypeError, ValueError):
            continue
        placement = step.get('placement') or {}
        cleared = int(placement.get('clearedLines') or 0)
        if cleared > 0:
            clear_until = max(clear_until, time + CLEAR_GRACE_SECONDS)
            if all(cell == '_' for row in (step.get('board') or []) for cell in row):
                perfect_until = max(perfect_until, time + PERFECT_CLEAR_GRACE_SECONDS)
        if index == 0 or step.get('action') == 'end' or time <= clear_until or time <= perfect_until:
            continue
        measured.append((time, _mismatch(step)))
    if len(measured) > IGNORE_LAST_PHASES:
        measured = measured[:-IGNORE_LAST_PHASES]
    else:
        return None
    best: dict | None = None
    run_start = None
    run_len = 0
    run_max = 0
    for time, cells in measured:
        if cells >= MISMATCH_CELLS:
            if run_len == 0:
                run_start = time
                run_max = 0
            run_len += 1
            run_max = max(run_max, cells)
            if run_len >= CONSECUTIVE_PHASES and (best is None or run_len > best['phases']):
                best = {'start': run_start, 'phases': run_len, 'maxCells': run_max}
        else:
            run_len = 0
    return best


def match_drift(recovered: Path) -> dict:
    """*_recovered.json 1つから {player: {...}} を返す（壊れていなければ空）。"""
    try:
        data = json.loads(recovered.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError, UnicodeDecodeError):
        return {}
    out = {}
    for player, key in ((1, 'p1'), (2, 'p2')):
        found = player_drift(data.get(key) or [])
        if found:
            out[str(player)] = found
    return out


class DriftCache:
    """試合ごとの結果を、ファイルの更新時刻つきで覚える（毎回 JSON を読み直さない）。"""

    def __init__(self, path: Path):
        self.path = Path(path)
        self._lock = threading.Lock()
        self._data: dict | None = None
        self._dirty = False

    def _load(self) -> dict:
        if self._data is None:
            try:
                raw = json.loads(self.path.read_text(encoding='utf-8'))
                self._data = raw if raw.get('version') == VERSION else {}
            except (OSError, json.JSONDecodeError, AttributeError):
                self._data = {}
            self._data.setdefault('version', VERSION)
            self._data.setdefault('files', {})
        return self._data

    def get(self, recovered: Path) -> dict:
        try:
            stamp = recovered.stat().st_mtime_ns
        except OSError:
            return {}
        key = str(recovered)
        with self._lock:
            files = self._load()['files']
            hit = files.get(key)
            if hit and hit.get('stamp') == stamp:
                return hit.get('drift') or {}
        drift = match_drift(recovered)
        with self._lock:
            self._load()['files'][key] = {'stamp': stamp, 'drift': drift}
            self._dirty = True
        return drift

    def save(self) -> None:
        with self._lock:
            if not self._dirty or self._data is None:
                return
            try:
                self.path.parent.mkdir(parents=True, exist_ok=True)
                tmp = self.path.with_suffix('.tmp')
                tmp.write_text(json.dumps(self._data, ensure_ascii=False), encoding='utf-8')
                tmp.replace(self.path)
                self._dirty = False
            except OSError:
                pass


def recovered_file(manifest: Path, index: int) -> Path | None:
    folder = Path(manifest).parent / f'match_{index:03d}' / 'simulator'
    try:
        for item in folder.iterdir():
            if item.name.endswith('_recovered.json'):
                return item
    except OSError:
        return None
    return None
