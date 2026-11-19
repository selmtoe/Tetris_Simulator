"""プレイヤー1人ぶんの集計（対戦相手ごとの戦績・PPS）。

PPS（1秒あたりに置いたミノ数）は、解析結果の1手ごとの時刻（autosave の labels）から出す。
1手の時間 = そのミノが操作中になってから次のミノに移るまで（NEXT が進んだ時刻の差）。

数える手（2026-11-19 に変更。体感の速さの順番と全員一致した方式）:
- 試合の最初の 30 手は数えない。開幕は決めておいた形を置くので誰でも速く、
  試合数が少ない人・短い試合が多い人ほど PPS が高く出ていた。
- ライン消去した手は数えない（labels の設置情報の消去ライン数で判定）。消去の演出時間は
  消した手そのものの時間に入るので、消去の多い人ほど遅く出ていた。
- 3 秒を超える手（REN の対面で待つ「カサカサ」、解析の切れ目など）は数えない。
- PPS = 数えた手数 ÷ 数えた時間（全試合まとめて）。数えた手が 200 手未満なら参考値。
- 試合の最後の手（勝敗が決まった後の時間を含む）と開始の合図は、もともと labels の
  place に入らないので数えない。
"""
from __future__ import annotations

import json
import os
import threading
from pathlib import Path

SKIP_OPENING_MOVES = 30         # 試合の最初のこの手数は数えない
WAIT_SECONDS = 3.0              # これより長い1手は「待ち」として数えない
MIN_RELIABLE_PIECES = 200       # 数えた手がこれより少なければ参考値（画面では括弧付き）
MIN_PIECES_PER_MATCH = 10       # 試合ごとの PPS はこれ以上数えた試合だけ
CACHE_VERSION = 2


class MoveTimes:
    """autosave ごとの「1手の時間（ミリ秒）」をファイルにも残して使い回す。"""

    def __init__(self, cache_file: Path | None):
        self.cache_file = cache_file
        self._lock = threading.Lock()
        self._data: dict | None = None
        self._dirty = False

    def _load(self) -> dict:
        if self._data is None:
            self._data = {}
            if self.cache_file is not None:
                try:
                    loaded = json.loads(self.cache_file.read_text(encoding='utf-8'))
                    if isinstance(loaded, dict):
                        self._data = loaded
                except (OSError, json.JSONDecodeError):
                    pass
        return self._data

    def get(self, autosave: Path) -> dict | None:
        """{'p1': [ms, ...], 'p2': [...]}。読めなければ None。"""
        try:
            stamp = autosave.stat().st_mtime_ns
        except OSError:
            return None
        key = str(autosave)
        with self._lock:
            hit = self._load().get(key)
        if hit and hit.get('stamp') == stamp and hit.get('v') == CACHE_VERSION:
            return hit
        try:
            payload = json.loads(autosave.read_text(encoding='utf-8-sig'))
        except (OSError, json.JSONDecodeError):
            return None
        # p1 / p2: 置いた手すべての時間（ミリ秒）。p1Counted / p2Counted: PPS に数える手だけ
        entry: dict = {'stamp': stamp, 'v': CACHE_VERSION}
        for side in ('p1', 'p2'):
            labels = ((payload.get('labels') or {}).get(side)) or []
            moves = [row for row in labels
                     if isinstance(row, list) and len(row) > 3 and row[3] == 'place']
            times, counted = [], []
            for number, row in enumerate(moves):
                try:
                    duration = max(0, int(row[1]) - int(row[0]))
                except (TypeError, ValueError):
                    continue
                times.append(duration)
                placement = row[7] if len(row) > 7 and isinstance(row[7], list) else []
                try:
                    cleared = int(placement[4]) if len(placement) > 4 else 0
                except (TypeError, ValueError):
                    cleared = 0
                if number >= SKIP_OPENING_MOVES and cleared == 0 and duration <= WAIT_SECONDS * 1000:
                    counted.append(duration)
            entry[side] = times
            entry[side + 'Counted'] = counted
        with self._lock:
            self._load()[key] = entry
            self._dirty = True
        return entry

    def flush(self) -> None:
        with self._lock:
            if not self._dirty or self.cache_file is None or self._data is None:
                return
            try:
                self.cache_file.parent.mkdir(parents=True, exist_ok=True)
                temp = self.cache_file.with_suffix('.tmp')
                temp.write_text(json.dumps(self._data, separators=(',', ':')), encoding='utf-8')
                os.replace(temp, self.cache_file)
                self._dirty = False
            except OSError:
                pass


def _quantile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    position = fraction * (len(ordered) - 1)
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def pps_summary(per_match: list[list[float]], raw_per_match: list[list[float]]) -> dict:
    """per_match: 試合ごとの「数える手」の時間（秒）。raw_per_match: 試合ごとの全手の時間（秒）。"""
    pooled = [value for match in per_match for value in match]
    raw = [value for match in raw_per_match for value in match]
    match_values = [len(match) / sum(match) for match in per_match
                    if len(match) >= MIN_PIECES_PER_MATCH and sum(match) > 0]
    return {
        'value': round(len(pooled) / sum(pooled), 3) if sum(pooled) > 0 else None,
        'raw': round(len(raw) / sum(raw), 3) if sum(raw) > 0 else None,
        'pieces': len(pooled),
        'allPieces': len(raw),
        'reliable': len(pooled) >= MIN_RELIABLE_PIECES,
        'minPieces': MIN_RELIABLE_PIECES,
        'matches': len(match_values),
        'perMatch': {
            'low': round(_quantile(match_values, 0.1), 2),
            'median': round(_quantile(match_values, 0.5), 2),
            'high': round(_quantile(match_values, 0.9), 2),
        } if match_values else None,
    }


def player_stats(name: str, sides: list[dict], move_times: MoveTimes) -> dict:
    """sides: その人が出た試合の一覧。

    各要素は {'videoId', 'videoTitle', 'index', 'side', 'opponent', 'winner',
    'autosave'}。winner は 'p1' / 'p2' / 'draw' / ''（未判定）。
    """
    totals = {'matches': 0, 'wins': 0, 'losses': 0, 'draws': 0, 'unknown': 0}
    opponents: dict[str, dict] = {}
    own_times: list[list[float]] = []
    own_raw: list[list[float]] = []
    opponent_times: dict[str, list[list[float]]] = {}
    own_by_opponent: dict[str, list[list[float]]] = {}
    for item in sides:
        side = item['side']
        other = 'p2' if side == 'p1' else 'p1'
        winner = str(item.get('winner') or '')
        result = 'unknown'
        if winner == side:
            result = 'wins'
        elif winner == other:
            result = 'losses'
        elif winner == 'draw':
            result = 'draws'
        totals['matches'] += 1
        totals[result] += 1
        opponent = str(item.get('opponent') or '') or '（名前なし）'
        row = opponents.setdefault(opponent, {'name': opponent, 'matches': 0, 'wins': 0,
                                              'losses': 0, 'draws': 0, 'unknown': 0})
        row['matches'] += 1
        row[result] += 1
        autosave = item.get('autosave')
        times = move_times.get(Path(autosave)) if autosave else None
        if times:
            mine = [value / 1000 for value in times.get(side + 'Counted') or []]
            theirs = [value / 1000 for value in times.get(other + 'Counted') or []]
            all_mine = [value / 1000 for value in times.get(side) or []]
            if all_mine:
                own_raw.append(all_mine)
                own_times.append(mine)
                own_by_opponent.setdefault(opponent, []).append(mine)
            if theirs:
                opponent_times.setdefault(opponent, []).append(theirs)
    move_times.flush()
    own = pps_summary(own_times, own_raw)

    def counted_pps(matches: list[list[float]]) -> float | None:
        kept = [value for match in matches for value in match]
        return round(len(kept) / sum(kept), 2) if kept and sum(kept) > 0 else None

    rows = []
    for opponent, row in opponents.items():
        decided = row['wins'] + row['losses']
        row['winRate'] = round(row['wins'] / decided, 3) if decided else None
        row['pps'] = counted_pps(own_by_opponent.get(opponent) or [])
        row['opponentPps'] = counted_pps(opponent_times.get(opponent) or [])
        rows.append(row)
    rows.sort(key=lambda item: (-item['matches'], item['name']))
    decided = totals['wins'] + totals['losses']
    totals['winRate'] = round(totals['wins'] / decided, 3) if decided else None
    # 開幕（名前を付けた1巡目）。名前の無いものは「未登録」にまとめる
    opening_counts: dict[str, dict] = {}
    for item in sides:
        if 'opening' not in item:
            continue
        label = str(item.get('opening') or '') or '未登録'
        entry = opening_counts.setdefault(label, {'name': label, 'count': 0, 'wins': 0, 'losses': 0})
        entry['count'] += 1
        side = item['side']
        winner = str(item.get('winner') or '')
        if winner == side:
            entry['wins'] += 1
        elif winner in ('p1', 'p2'):
            entry['losses'] += 1
    openings = sorted(opening_counts.values(), key=lambda entry: (entry['name'] == '未登録', -entry['count']))
    return {'name': name, 'totals': totals, 'pps': own, 'opponents': rows,
            'openings': openings}
