#!/usr/bin/env python3
"""Replay validator: flag board changes that no legal move can explain.

This is an *independent* reference implementation of the engine's board rules
(tetris_engine.cpp: shape / fits / lock / legalMoves / boardWithManualGarbage).
It replays every exported phase and checks that

    board[i] == garbageRise(clear(prev.board + placement[i]), garbage[i])

where the placement must be a lock that the real piece could reach from spawn
(SRS kicks, left/right, soft drop), the placed piece must be the previous
phase's active piece, and the garbage rows must be well formed.

It does NOT look at the video or the observed board.  Its purpose is the P0
safety net from HANDOFF.md: a recovery may be wrong, but it must never contain a
board that "warps" without a legal explanation.

Usage:
    python tools/replay_validator.py <batch-dir-or-recovered.json> [...]
        [--out report.json] [--quiet]

Exit code: 0 = no errors, 1 = at least one error, 2 = input problem.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter
from dataclasses import dataclass, field

W, H, VISIBLE = 10, 40, 20
EMPTY, GARBAGE = "_", "G"
PIECES = "IOTLJSZ"
PIECE_SET = frozenset(PIECES)  # never use `in PIECES`: "" in "IOTLJSZ" is True

# --------------------------------------------------------------------------
# Piece geometry (mirrors TetrisEngine::shape / rotationOffsets exactly).
# --------------------------------------------------------------------------
_BASE = {
    "I": ([(0, 0), (1, 0), (2, 0), (3, 0)], 1.5, 0.5),
    "O": ([(0, 0), (1, 0), (0, -1), (1, -1)], 0.5, -0.5),
    "T": ([(0, 0), (-1, 0), (0, -1), (1, 0)], 0.0, 0.0),
    "L": ([(-1, 0), (0, 0), (1, 0), (1, -1)], 0.0, 0.0),
    "J": ([(0, 0), (-1, 0), (1, 0), (-1, -1)], 0.0, 0.0),
    "S": ([(1, -1), (-1, 0), (0, 0), (0, -1)], 0.0, 0.0),
    "Z": ([(0, 0), (1, 0), (0, -1), (-1, -1)], 0.0, 0.0),
}


def _cround(v: float) -> int:
    # std::lround: half away from zero
    return int(v + 0.5) if v >= 0 else -int(-v + 0.5)


def _shape(piece: str, rotation: int):
    base, cx, cy = _BASE[piece]
    rotation &= 3
    if piece == "O" or rotation == 0:
        return tuple(base)
    out = []
    for bx, by in base:
        x, y = bx - cx, by - cy
        for _ in range(rotation):
            x, y = -y, x
        out.append((_cround(x + cx), _cround(y + cy)))
    return tuple(out)


SHAPES = {p: [_shape(p, r) for r in range(4)] for p in PIECES}

_KICK_CW = {
    "I": {0: [(0, 0), (-2, 0), (1, 0), (-2, -1), (1, 2)],
          1: [(0, 0), (-1, 0), (2, 0), (-1, 2), (2, -1)],
          2: [(0, 0), (2, 0), (-1, 0), (2, 1), (-1, -2)],
          3: [(0, 0), (1, 0), (-2, 0), (1, -2), (-2, 1)]},
    "*": {0: [(0, 0), (-1, 0), (-1, 1), (0, -2), (-1, -2)],
          1: [(0, 0), (1, 0), (1, -1), (0, 2), (1, 2)],
          2: [(0, 0), (1, 0), (1, 1), (0, -2), (1, -2)],
          3: [(0, 0), (-1, 0), (-1, -1), (0, 2), (-1, 2)]},
}
_KICK_CCW = {
    "I": {0: [(0, 0), (-1, 0), (2, 0), (-1, 2), (2, -1)],
          1: [(0, 0), (2, 0), (-1, 0), (2, 1), (-1, -2)],
          2: [(0, 0), (1, 0), (-2, 0), (1, -2), (-2, 1)],
          3: [(0, 0), (-2, 0), (1, 0), (-2, -1), (1, 2)]},
    "*": {0: [(0, 0), (1, 0), (1, 1), (0, -2), (1, -2)],
          1: [(0, 0), (1, 0), (1, -1), (0, 2), (1, 2)],
          2: [(0, 0), (-1, 0), (-1, 1), (0, -2), (-1, -2)],
          3: [(0, 0), (-1, 0), (-1, -1), (0, 2), (-1, 2)]},
}


def _kicks(piece: str, frm: int, to: int):
    table = _KICK_CW if (to - frm) % 4 == 1 else _KICK_CCW
    return table["I" if piece == "I" else "*"][frm & 3]


# --------------------------------------------------------------------------
# Board operations. A board is a list of 40 strings (row 0 = top).
# --------------------------------------------------------------------------
def occupied(c: str) -> bool:
    return c != EMPTY


def fits(board, cells, x: int, y: int) -> bool:
    for dx, dy in cells:
        nx, ny = x + dx, y + dy
        if nx < 0 or nx >= W or ny >= H:
            return False
        if ny >= 0 and occupied(board[ny][nx]):
            return False
    return True


def lock(board, piece: str, rotation: int, x: int, y: int):
    """Return (fullBoard, clearedBoard, clearedLines)."""
    rows = [list(r) for r in board]
    for dx, dy in SHAPES[piece][rotation & 3]:
        nx, ny = x + dx, y + dy
        if 0 <= ny < H:
            rows[ny][nx] = piece
    full = ["".join(r) for r in rows]
    kept = [r for r in full if not all(occupied(c) for c in r)]
    cleared = H - len(kept)
    return full, [EMPTY * W] * cleared + kept, cleared


def apply_garbage(board, lines: int, hole_masks):
    lines = max(0, min(lines, H))
    if lines == 0:
        return list(board)
    out = list(board[lines:])
    for row in range(lines):
        mask = hole_masks[row] if row < len(hole_masks) else (1 << 4)
        mask = _normalized_mask(mask)
        out.append("".join(EMPTY if mask & (1 << x) else GARBAGE for x in range(W)))
    return out


def _normalized_mask(mask: int) -> int:
    # Mirrors normalizedHoleMask(): keep only in-board bits, never a solid row.
    mask &= (1 << W) - 1
    return mask if mask else (1 << 4)


def spawn(piece: str):
    return (3, 20) if piece == "I" else (4, 20)


def reachable_locks(board, piece: str):
    """Set of frozenset(cells) reachable by TetrisEngine::legalMoves (complete soft drop)."""
    rots = SHAPES[piece]
    sx, sy = spawn(piece)
    # y=20/19/18 as before, then (v79) up to 4 more rows when all are blocked,
    # mirroring legalMovesImpl's TR_SPAWN_EXTRA_ROWS fallback (sy must stay >= 2).
    extra = max(0, min(8, int(os.environ.get("TR_SPAWN_EXTRA_ROWS", "4") or 0)))
    if sum(1 for row in board for cell in row if occupied(cell)) < 80:
        extra = 0  # only a genuinely high stack (mirrors legalMovesImpl)
    for attempt in range(3 + extra):
        if attempt >= 3 and sy < 2:
            return set()
        if fits(board, rots[0], sx, sy):
            break
        sy -= 1
    else:
        return set()
    seen = {(sx, sy, 0)}
    queue = [(sx, sy, 0)]
    locks = set()

    def enqueue(x, y, r):
        if fits(board, rots[r], x, y) and (x, y, r) not in seen:
            seen.add((x, y, r))
            queue.append((x, y, r))

    i = 0
    while i < len(queue):
        x, y, r = queue[i]
        i += 1
        cells = rots[r]
        hy = y
        while fits(board, cells, x, hy + 1):
            hy += 1
        landed = frozenset((x + dx, hy + dy) for dx, dy in cells)
        if any(cy >= VISIBLE for _, cy in landed):
            locks.add(landed)
        enqueue(x - 1, y, r)
        enqueue(x + 1, y, r)
        if hy != y:
            enqueue(x, hy, r)
        if piece != "O":
            for nr in ((r + 1) & 3, (r + 3) & 3):
                for ox, oy in _kicks(piece, r, nr):
                    if fits(board, rots[nr], x + ox, y - oy):
                        enqueue(x + ox, y - oy, nr)
                        break
        enqueue(x, y + 1, r)  # soft drop
    return locks


def placed_cells(piece, rotation, x, y):
    return frozenset((x + dx, y + dy) for dx, dy in SHAPES[piece][rotation & 3])


def board_diff(a, b):
    return [(x, y) for y in range(H) for x in range(W) if a[y][x] != b[y][x]]


# --------------------------------------------------------------------------
# Validation
# --------------------------------------------------------------------------
# Error codes break the replay chain (the board is not what the rules say).
# Warning codes are suspicious but the board is still consistent.
ERRORS = {
    "BOARD_UNEXPLAINED": "board differs from prev.board + placement + clear + garbage",
    "FULLBOARD_MISMATCH": "fullBoard differs from prev.board + placement",
    "CLEAR_COUNT_MISMATCH": "clearedLines differs from the replayed line clear",
    "PLACEMENT_OVERLAP": "placement overlaps the previous board or leaves the field",
    "PLACEMENT_FLOATING": "placement is not resting (could still fall)",
    "PLACEMENT_UNREACHABLE": "no legal path from spawn reaches this lock",
    "PLACEMENT_HIDDEN": "placement has no cell in the visible 20 rows",
    "PIECE_NOT_ACTIVE": "placed piece is not the previous phase's active piece",
    "BAD_PIECE": "unknown piece letter or rotation",
    "BAD_BOARD": "board is not 40 rows x 10 cells of known letters",
}
WARNINGS = {
    "GARBAGE_MASK_COUNT": "garbage.holeMasks length differs from garbage.lines",
    "GARBAGE_MASK_SHAPE": "garbage hole mask is not exactly one hole",
    "SHIFT_LINES_MISMATCH": "shiftLines differs from garbage.lines",
    "QUEUE_SEQUENCE_INCONSISTENT": "placed mino cannot come from the NEXT order under HOLD rules",
}
VALID_CHARS = PIECE_SET | {EMPTY, GARBAGE}


@dataclass
class Issue:
    player: str
    phase: int
    time: float
    code: str
    detail: str = ""
    explanation: str = ""  # for BOARD_UNEXPLAINED: could any legal move explain it?

    @property
    def severity(self):
        return "error" if self.code in ERRORS else "warning"

    def as_dict(self):
        d = {"player": self.player, "phase": self.phase, "time": self.time,
             "severity": self.severity, "code": self.code, "detail": self.detail}
        if self.explanation:
            d["explanation"] = self.explanation
        return d


def _board_ok(rows):
    return (isinstance(rows, list) and len(rows) == H and
            all(isinstance(r, str) and len(r) == W and set(r) <= VALID_CHARS for r in rows))


def _explain(prev_board, target, candidates):
    """Search for *any* legal lock + garbage rise that yields `target`."""
    for piece in candidates:
        if piece not in PIECE_SET:
            continue
        for cells in reachable_locks(prev_board, piece):
            rows = [list(r) for r in prev_board]
            for cx, cy in cells:
                if 0 <= cy < H:
                    rows[cy][cx] = piece
            full = ["".join(r) for r in rows]
            kept = [r for r in full if not all(occupied(c) for c in r)]
            cleared = [EMPTY * W] * (H - len(kept)) + kept
            for g in range(0, 13):
                if cleared[g:] != target[:H - g]:
                    continue
                bottom = target[H - g:]
                if all(r.count(EMPTY) >= 1 and set(r) <= {EMPTY, GARBAGE} for r in bottom):
                    return f"explainable by {piece} lock {sorted(cells)} + {g} garbage"
    return "no legal lock of active/hold/next + garbage explains it (warp)"


def _spawn_sequence(phases):
    """Spawn order rebuilt from the per-phase NEXT windows (None if not chainable)."""
    first = next((p for p in phases if len(p.get("next") or "") >= 5), None)
    if first is None:
        return None, None
    seq, heads, cur = list(first["next"][:5]), [], 0
    for p in phases:
        win = (p.get("next") or "")[:5]
        if len(win) < 5:
            heads.append(None)
            continue
        for adv in range(4):
            known = seq[cur + adv:cur + adv + 5]
            if all(win[i] == known[i] for i in range(len(known))):
                seq.extend(win[len(known):])
                cur += adv
                break
        else:
            return None, None
        heads.append(cur)
    return seq, heads


def queue_consistency(player: str, phases):
    """Warn where a placed mino is impossible given NEXT order and HOLD rules.

    Turn j (placement at phase j) happens during phase j-1; the mino that
    spawned for it is seq[head(j-1) - 1].  With NEXT advancing by one, the
    placed mino must be the spawned one or the current HOLD (a swap); with an
    advance of two it must be the mino after the spawned one (first HOLD).
    HOLD starts unknown and is learned from the first swap."""
    seq, heads = _spawn_sequence(phases)
    out = []
    if seq is None:
        return out
    hold = None
    for j in range(2, len(phases)):
        piece = (phases[j].get("placement") or {}).get("piece") or ""
        h0, h1 = heads[j - 1], heads[j]
        if not piece or h0 is None or h1 is None or h0 < 1:
            hold = None
            continue
        spawned, adv = seq[h0 - 1], h1 - h0
        if adv == 1 and piece == spawned:
            continue
        if adv == 1 and (hold is None or hold == piece):
            hold = spawned
            continue
        if adv == 2 and h0 < len(seq) and piece == seq[h0] and hold in (None, "-"):
            hold = spawned
            continue
        out.append(Issue(player, j, float(phases[j].get("time", 0.0)), "QUEUE_SEQUENCE_INCONSISTENT",
                         f"placed={piece} spawned={spawned} hold={hold} next_advance={adv}"))
        hold = None
    return out


def validate_player(player: str, phases, explain: bool = True):
    issues = queue_consistency(player, phases)
    if not phases:
        return issues
    for i, st in enumerate(phases):
        t = float(st.get("time", 0.0))
        for key in ("board", "fullBoard"):
            if not _board_ok(st.get(key)):
                issues.append(Issue(player, i, t, "BAD_BOARD", key))
        if i == 0:
            continue
        prev = phases[i - 1]
        if not (_board_ok(prev.get("board")) and _board_ok(st.get("board"))):
            continue
        pb = prev["board"]
        pl = st.get("placement") or {}
        piece = pl.get("piece", "")
        g = st.get("garbage") or {}
        g_lines = int(g.get("lines", 0))
        masks = list(g.get("holeMasks", []))

        if int(st.get("shiftLines", g_lines)) != g_lines:
            issues.append(Issue(player, i, t, "SHIFT_LINES_MISMATCH",
                                f"shiftLines={st.get('shiftLines')} garbage.lines={g_lines}"))
        if g_lines and len(masks) != g_lines:
            issues.append(Issue(player, i, t, "GARBAGE_MASK_COUNT", f"lines={g_lines} masks={masks}"))
        for m in masks:
            nm = _normalized_mask(int(m))
            if bin(nm).count("1") != 1:
                issues.append(Issue(player, i, t, "GARBAGE_MASK_SHAPE", f"mask={m}"))

        expected_full = list(pb)
        after_lock = list(pb)
        if piece:
            rot = pl.get("rotation", 0)
            x, y = pl.get("x", 0), pl.get("y", 0)
            if piece not in PIECE_SET or not isinstance(rot, int) or not 0 <= rot <= 3:
                issues.append(Issue(player, i, t, "BAD_PIECE", f"piece={piece!r} rot={rot!r}"))
                continue
            # The init phase has no active piece (the mino that spawned at GO is
            # never in the NEXT window), so any mino may be placed first.
            if prev.get("active", "") and piece != prev.get("active", ""):
                issues.append(Issue(player, i, t, "PIECE_NOT_ACTIVE",
                                    f"placed={piece} prev.active={prev.get('active')!r} prev.hold={prev.get('hold')!r}"))
            cells = SHAPES[piece][rot]
            if not fits(pb, cells, x, y):
                issues.append(Issue(player, i, t, "PLACEMENT_OVERLAP", f"{piece} r{rot} ({x},{y})"))
            else:
                if fits(pb, cells, x, y + 1):
                    issues.append(Issue(player, i, t, "PLACEMENT_FLOATING", f"{piece} r{rot} ({x},{y})"))
                pc = placed_cells(piece, rot, x, y)
                if not any(cy >= VISIBLE for _, cy in pc):
                    issues.append(Issue(player, i, t, "PLACEMENT_HIDDEN", f"{piece} r{rot} ({x},{y})"))
                elif pc not in reachable_locks(pb, piece):
                    issues.append(Issue(player, i, t, "PLACEMENT_UNREACHABLE", f"{piece} r{rot} ({x},{y})"))
            expected_full, after_lock, cleared = lock(pb, piece, rot, x, y)
            if cleared != int(pl.get("clearedLines", 0)):
                issues.append(Issue(player, i, t, "CLEAR_COUNT_MISMATCH",
                                    f"replayed={cleared} recorded={pl.get('clearedLines')}"))
            if _board_ok(st.get("fullBoard")) and expected_full != st["fullBoard"]:
                d = board_diff(expected_full, st["fullBoard"])
                issues.append(Issue(player, i, t, "FULLBOARD_MISMATCH", f"{len(d)} cells e.g. {d[:4]}"))
        expected = apply_garbage(after_lock, g_lines, masks)
        if expected != st["board"]:
            d = board_diff(expected, st["board"])
            iss = Issue(player, i, t, "BOARD_UNEXPLAINED",
                        f"action={st.get('action')} {len(d)} cells differ e.g. {d[:4]}")
            if explain:
                nxt = (prev.get("next") or "")[:1]
                iss.explanation = _explain(pb, st["board"], dict.fromkeys(
                    [prev.get("active", ""), prev.get("hold", ""), nxt]))
            issues.append(iss)
    return issues


# --------------------------------------------------------------------------
# Input discovery (os.listdir only: video IDs contain [ ] which glob mangles)
# --------------------------------------------------------------------------
def load_json(path):
    with open(path, encoding="utf-8-sig") as fh:
        return json.load(fh)


def find_recovered(match_dir):
    sim = os.path.join(match_dir, "simulator")
    if not os.path.isdir(sim):
        return None
    for name in sorted(os.listdir(sim)):
        if name.endswith("_recovered.json"):
            return os.path.join(sim, name)
    return None


def iter_traces(path):
    """Yield (match_name, recovered_json_path)."""
    if os.path.isfile(path):
        yield os.path.basename(os.path.dirname(os.path.dirname(path))) or path, path
        return
    names = sorted(n for n in os.listdir(path) if n.startswith("match_"))
    if not names and find_recovered(path):
        yield os.path.basename(path), find_recovered(path)
        return
    for name in names:
        f = find_recovered(os.path.join(path, name))
        yield name, f


def validate_trace(path, explain=True):
    data = load_json(path)
    issues = []
    for player in ("p1", "p2"):
        issues += validate_player(player, data.get(player) or [], explain=explain)
    return data, issues


def validate_batch(path, explain=True):
    report = {"schema": "tetris-replay-validation/v1", "input": os.path.abspath(path),
              "matches": [], "totals": {}}
    totals = Counter()
    for name, rec in iter_traces(path):
        entry = {"match": name, "trace": rec}
        if rec is None:
            entry["missing"] = True
            totals["missing_trace"] += 1
            report["matches"].append(entry)
            continue
        data, issues = validate_trace(rec, explain=explain)
        entry["phases"] = {p: len(data.get(p) or []) for p in ("p1", "p2")}
        entry["engineUnresolved"] = sum(bool(s.get("recoveryUnresolved"))
                                        for p in ("p1", "p2") for s in data.get(p) or [])
        entry["errors"] = sum(1 for x in issues if x.severity == "error")
        entry["warnings"] = len(issues) - entry["errors"]
        entry["codes"] = dict(Counter(x.code for x in issues))
        entry["issues"] = [x.as_dict() for x in issues]
        report["matches"].append(entry)
        totals["matches"] += 1
        totals["phases"] += sum(entry["phases"].values())
        totals["errors"] += entry["errors"]
        totals["warnings"] += entry["warnings"]
        totals["matches_with_errors"] += entry["errors"] > 0
        for c, n in entry["codes"].items():
            totals["code:" + c] += n
    report["totals"] = dict(totals)
    return report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="batch output dir, match dir, or *_recovered.json")
    ap.add_argument("--out", help="write the JSON report here (single input only)")
    ap.add_argument("--no-explain", action="store_true", help="skip the warp/label search")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    worst = 0
    for inp in args.inputs:
        if not os.path.exists(inp):
            print(f"not found: {inp}", file=sys.stderr)
            return 2
        rep = validate_batch(inp, explain=not args.no_explain)
        if args.out:
            with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
                json.dump(rep, fh, ensure_ascii=False, indent=1)
        tot = rep["totals"]
        print(f"{inp}: matches={tot.get('matches', 0)} phases={tot.get('phases', 0)} "
              f"errors={tot.get('errors', 0)} warnings={tot.get('warnings', 0)} "
              f"missing={tot.get('missing_trace', 0)}")
        if not args.quiet:
            for m in rep["matches"]:
                for iss in m.get("issues", []):
                    print(f"  {m['match']} {iss['player']} #{iss['phase']} t={iss['time']:.3f} "
                          f"{iss['severity'].upper()} {iss['code']}: {iss['detail']}"
                          + (f" -> {iss['explanation']}" if iss.get("explanation") else ""))
        if tot.get("errors", 0) or tot.get("missing_trace", 0):
            worst = 1
    return worst


if __name__ == "__main__":
    sys.exit(main())
