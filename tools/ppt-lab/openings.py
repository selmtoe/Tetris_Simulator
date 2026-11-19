"""開幕（1巡目）の分類。

1巡目 = 最初に置かれた I/J/L/O/S/T/Z 各1個（ホールドで順番が入れ替わっても、
種類ごとに最初に置いたものが1巡目のミノ）。その7個の置き場所（マス）が
完全に同じものを1つの「開幕」にまとめる。左右反転（x を反転し S⇔Z・J⇔L）も同じとみなす。

ライン消去は「無かったこと」にして座標を取る。消した段は消さずに残し、その後に置いた
ミノは、消える前の盤面の座標で数える（消した段をまたぐミノは段ごとに分かれて置かれる）。
7個そろう前に置いた2巡目のミノ（同じ種類の2個目）も、表示用に extras として残す。
せり上がりがあった試合は座標がずれるので分類しない。

名前は利用者が付ける（opening_names 表）。名前の付いた開幕は、以後の解析にも自動で付く。
"""
from __future__ import annotations

import hashlib
import json
import threading
from pathlib import Path

VERSION = 2
PIECES = 'IJLOSTZ'
MIRROR_PIECE = {'S': 'Z', 'Z': 'S', 'J': 'L', 'L': 'J', 'I': 'I', 'O': 'O', 'T': 'T'}
WIDTH = 10

# 解析結果の placement（piece, x, y, rotation）→ 4マスの (段の差, 列の差)。
# 段は y（上から数えた行）を下からの段に直した基準からの差。5万手の盤面の差から求めた（例外なし）。
OFFSETS = {
    ('I', 0): ((0, 0), (0, 1), (0, 2), (0, 3)), ('I', 1): ((-2, 2), (-1, 2), (0, 2), (1, 2)),
    ('I', 2): ((-1, 0), (-1, 1), (-1, 2), (-1, 3)), ('I', 3): ((-2, 1), (-1, 1), (0, 1), (1, 1)),
    ('J', 0): ((0, -1), (0, 0), (0, 1), (1, -1)), ('J', 1): ((-1, 0), (0, 0), (1, 0), (1, 1)),
    ('J', 2): ((-1, 1), (0, -1), (0, 0), (0, 1)), ('J', 3): ((-1, -1), (-1, 0), (0, 0), (1, 0)),
    ('L', 0): ((0, -1), (0, 0), (0, 1), (1, 1)), ('L', 1): ((-1, 0), (-1, 1), (0, 0), (1, 0)),
    ('L', 2): ((-1, -1), (0, -1), (0, 0), (0, 1)), ('L', 3): ((-1, 0), (0, 0), (1, -1), (1, 0)),
    ('O', 0): ((0, 0), (0, 1), (1, 0), (1, 1)),
    ('S', 0): ((0, -1), (0, 0), (1, 0), (1, 1)), ('S', 1): ((-1, 1), (0, 0), (0, 1), (1, 0)),
    ('S', 2): ((-1, -1), (-1, 0), (0, 0), (0, 1)), ('S', 3): ((-1, 0), (0, -1), (0, 0), (1, -1)),
    ('T', 0): ((0, -1), (0, 0), (0, 1), (1, 0)), ('T', 1): ((-1, 0), (0, 0), (0, 1), (1, 0)),
    ('T', 2): ((-1, 0), (0, -1), (0, 0), (0, 1)), ('T', 3): ((-1, 0), (0, -1), (0, 0), (1, 0)),
    ('Z', 0): ((0, 0), (0, 1), (1, -1), (1, 0)), ('Z', 1): ((-1, 0), (0, 0), (0, 1), (1, 1)),
    ('Z', 2): ((-1, 0), (-1, 1), (0, -1), (0, 0)), ('Z', 3): ((-1, -1), (0, -1), (0, 0), (1, 0)),
}


def _placed_cells(before: list[str], after: list[str]) -> tuple[str, list[tuple[int, int]]] | None:
    """前の盤面から増えたマス（4マス・同じミノ）を返す。座標は (下からの段, 列)。"""
    if len(before) != len(after):
        return None
    height = len(after)
    cells: list[tuple[int, int]] = []
    letters = set()
    for row in range(height):
        old, new = before[row], after[row]
        if old == new:
            continue
        for col in range(min(len(old), len(new), WIDTH)):
            if old[col] == new[col]:
                continue
            if old[col] != '_' or new[col] == '_':
                return None          # 消えたマス・書き換わったマスがある（消去・せり上がり・読み違い）
            cells.append((height - 1 - row, col))
            letters.add(new[col])
    if len(cells) != 4 or len(letters) != 1:
        return None
    return letters.pop(), sorted(cells)


def _cells_from_placement(placement: dict, height: int) -> list[tuple[int, int]] | None:
    try:
        piece = str(placement.get('piece') or '').upper()
        rotation = int(placement.get('rotation') or 0) % 4
        x = int(placement.get('x'))
        y = int(placement.get('y'))
    except (TypeError, ValueError):
        return None
    offsets = OFFSETS.get((piece, rotation))
    if offsets is None:
        return None
    base = height - 1 - y
    cells = sorted((base + dr, x + dc) for dr, dc in offsets)
    if any(not (0 <= r < height and 0 <= c < WIDTH) for r, c in cells):
        return None
    return cells


def _occupied(board: list[str]) -> set[tuple[int, int]]:
    height = len(board)
    return {(height - 1 - row, col) for row in range(height)
            for col in range(min(len(board[row]), WIDTH)) if board[row][col] != '_'}


def opening_of(steps: list[dict]) -> dict:
    """1人ぶんの手順から1巡目の7個の置き場所を取る（座標はライン消去が無かった盤面のもの）。

    返り値: {'pieces': {ミノ: [[段, 列], ...]}, 'extras': [[ミノ, [[段, 列], ...]], ...]}
            か {'reason': 分類しない理由}
    """
    previous = None
    virtual: list[int] = []          # いまの盤面の段 → 消去が無かった盤面の段
    next_row = 0
    found: dict[str, list[list[int]]] = {}
    extras: list[list] = []
    for step in steps or []:
        board = step.get('board')
        if not isinstance(board, list) or not board:
            continue
        action = step.get('action')
        if action == 'init':
            previous = board
            continue
        if previous is None:
            previous = ['_' * WIDTH] * len(board)
        if not virtual:
            virtual = list(range(len(board)))
            next_row = len(board)
        if action != 'place':
            previous = board
            continue
        placement = step.get('placement') or {}
        if board == previous and not placement.get('piece'):
            continue                 # 盤面が変わらない空の手（開始直後に入ることがある）
        if int((step.get('garbage') or {}).get('lines') or 0) > 0 or int(step.get('shiftLines') or 0) > 0:
            return {'reason': 'garbage'}
        cleared = int(placement.get('clearedLines') or 0)
        height = len(board)
        if cleared:
            cells = _cells_from_placement(placement, height)
            if cells is None:
                return {'reason': 'diff'}
            piece = str(placement.get('piece')).upper()
            filled = _occupied(previous)
            if filled & set(cells):
                return {'reason': 'diff'}
            filled |= set(cells)
            full = sorted({r for r, _ in cells if all((r, c) in filled for c in range(WIDTH))}, reverse=True)
            if len(full) != cleared:
                return {'reason': 'diff'}
        else:
            placed = _placed_cells(previous, board)
            if placed is None:
                return {'reason': 'diff'}
            letter, cells = placed
            piece = str(placement.get('piece') or letter).upper()[:1]
            if piece not in PIECES:
                piece = letter
            full = []
        mapped = sorted([virtual[r], c] for r, c in cells)
        if piece in found:
            extras.append([piece, mapped])
        else:
            found[piece] = mapped
        # 消した段を「いまの盤面」から外す（消去の無い盤面には残る）
        for r in full:
            del virtual[r]
            virtual.append(next_row)
            next_row += 1
        previous = board
        if len(found) == len(PIECES):
            return {'pieces': {name: found[name] for name in PIECES}, 'extras': extras}
    return {'reason': 'short'}


def _signature(pieces: dict) -> str:
    return '|'.join(name + ':' + ','.join(f'{r}.{c}' for r, c in sorted(map(tuple, pieces[name])))
                    for name in PIECES)


def _mirror_cells(cells) -> list[list[int]]:
    return sorted([r, WIDTH - 1 - c] for r, c in cells)


def _mirror(pieces: dict) -> dict:
    return {MIRROR_PIECE[name]: _mirror_cells(cells) for name, cells in pieces.items()}


def canonical(pieces: dict, extras: list | None = None) -> tuple[str, dict, bool, list]:
    """左右反転と区別しない形にそろえる。(キー, そろえた置き場所, 反転したか, そろえた extras)。"""
    plain = {name: sorted(map(list, pieces[name])) for name in PIECES}
    mirrored = _mirror(plain)
    a, b = _signature(plain), _signature(mirrored)
    flipped = b < a
    if flipped:
        plain, a = mirrored, b
    extra_out = []
    for piece, cells in extras or []:
        if flipped:
            extra_out.append([MIRROR_PIECE.get(piece, piece), _mirror_cells(cells)])
        else:
            extra_out.append([piece, sorted(map(list, cells))])
    key = hashlib.sha1(a.encode('ascii')).hexdigest()[:16]
    return key, plain, flipped, extra_out


def key_of(pieces: dict) -> str:
    return canonical(pieces)[0]


def recovered_file(manifest: Path, index: int) -> Path | None:
    folder = Path(manifest).parent / f'match_{index:03d}' / 'simulator'
    try:
        for item in folder.iterdir():
            if item.name.endswith('_recovered.json'):
                return item
    except OSError:
        return None
    return None


class OpeningCache:
    """試合ごとの開幕キーを、recovered.json の更新時刻つきで覚える。"""

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
            self._data.setdefault('shapes', {})
        return self._data

    def get(self, recovered: Path) -> dict:
        """{'p1': {'key', 'flipped', 'extras'} | {'reason'}, 'p2': ...}"""
        try:
            stamp = recovered.stat().st_mtime_ns
        except OSError:
            return {}
        name = str(recovered)
        with self._lock:
            hit = self._load()['files'].get(name)
            if hit and hit.get('stamp') == stamp:
                return hit.get('sides') or {}
        try:
            data = json.loads(recovered.read_text(encoding='utf-8'))
        except (OSError, json.JSONDecodeError):
            return {}
        sides: dict = {}
        shapes: dict = {}
        for side in ('p1', 'p2'):
            result = opening_of(data.get(side) or [])
            if 'pieces' in result:
                key, pieces, flipped, extras = canonical(result['pieces'], result.get('extras'))
                sides[side] = {'key': key, 'flipped': flipped, 'extras': extras}
                shapes[key] = pieces
            else:
                sides[side] = {'reason': result.get('reason', '')}
        with self._lock:
            store = self._load()
            store['files'][name] = {'stamp': stamp, 'sides': sides}
            store['shapes'].update(shapes)
            self._dirty = True
        return sides

    def shape(self, key: str) -> dict | None:
        with self._lock:
            return self._load()['shapes'].get(key)

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
