"""Explain P1/P2 board mismatches recorded in a recovered timeline.

Reads the recovered JSON (per-phase `observed` and `board` 10x40 grids) and
prints every run of consecutive mismatch phases with the grids of the worst
phase, plus shift statistics that say whether the reconstruction is a rigid
shift of the observation (queue/lock misalignment) or a different pile.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

EMPTY = "_"


def load(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def ratio(entry: dict) -> float:
    observed = entry["observed"]
    board = entry["board"]
    matched = 0
    for row in range(20, 40):
        for col in range(10):
            if observed[row][col] == board[row][col]:
                matched += 1
    return matched / 200.0


def runs(entries: list[dict], threshold: float = 0.95) -> list[tuple[int, int]]:
    result = []
    start = None
    for index, entry in enumerate(entries):
        if entry.get("action") == "end":
            if start is not None:
                result.append((start, index - 1))
                start = None
            continue
        if ratio(entry) < threshold:
            if start is None:
                start = index
        elif start is not None:
            result.append((start, index - 1))
            start = None
    if start is not None:
        result.append((start, len(entries) - 1))
    return result


def shifted_match(observed: list[str], board: list[str], dy: int, dx: int) -> int:
    matched = 0
    for row in range(20, 40):
        for col in range(10):
            source_row = row - dy
            source_col = col - dx
            if not (20 <= source_row < 40 and 0 <= source_col < 10):
                continue
            if observed[source_row][source_col] == board[row][col]:
                matched += 1
    return matched


def print_grids(entry: dict, label: str) -> None:
    observed = entry["observed"][20:]
    board = entry["board"][20:]
    print(f"    phase {label}  time={entry['time']:.3f}  ratio={ratio(entry):.3f} "
          f"action={entry.get('action')} active={entry.get('active')} "
          f"garbage={entry.get('garbage', {}).get('lines')}")
    print("      observed            board               diff")
    for row in range(20):
        mark = "".join(" " if observed[row][col] == board[row][col] else "X"
                       for col in range(10))
        print(f"    {observed[row]}   {board[row]}   {mark}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--file", required=True, type=Path)
    parser.add_argument("--player", default="p2", choices=["p1", "p2"])
    parser.add_argument("--threshold", type=float, default=0.95)
    parser.add_argument("--limit", type=int, default=8)
    parser.add_argument("--grids", type=int, default=2)
    args = parser.parse_args()

    data = load(args.file)
    entries = data[args.player]
    found = runs(entries, args.threshold)
    print(f"{args.file.name} {args.player}: phases={len(entries)} "
          f"mismatch_runs={len(found)}")
    for start, end in found[: args.limit]:
        worst = min(range(start, end + 1), key=lambda i: ratio(entries[i]))
        print(f"\n  run phases {start}-{end} ({end - start + 1} phases) "
              f"times {entries[start]['time']:.2f}-{entries[end]['time']:.2f} "
              f"worst={worst} ratio={ratio(entries[worst]):.3f}")
        for offset in range(min(args.grids, end - start + 1)):
            index = start + offset
            print_grids(entries[index], str(index))

    # Shift statistics over every mismatching phase of the run block.
    if found:
        best: dict[tuple[int, int], int] = {}
        total = 0
        for start, end in found:
            for index in range(start, end + 1):
                entry = entries[index]
                total += 1
                for dy in range(-3, 4):
                    for dx in range(-2, 3):
                        key = (dy, dx)
                        best[key] = best.get(key, 0) + shifted_match(
                            entry["observed"], entry["board"], dy, dx)
        print("\n  shift scan (cell matches summed over mismatching phases):")
        ranked = sorted(best.items(), key=lambda item: -item[1])
        for (dy, dx), score in ranked[:5]:
            print(f"    dy={dy:+d} dx={dx:+d} -> {score / (total * 200):.3f} of cells")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
