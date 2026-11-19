"""Compare the matches the current engine finds with the records published on the Lab site.

The site publishes one match record per analysed match (start/end seconds).
This script pairs them with the fresh ``segments.matches.json`` of each video and
reports how many segments agree and how far the start times moved.
"""
from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

LIBRARY = Path(r"scratch\labsite\library.json")
BATCHES = Path(r"scratch\labsite\batches")


def library_starts() -> dict[str, list[float]]:
    data = json.loads(LIBRARY.read_text(encoding="utf-8-sig"))
    result: dict[str, list[float]] = {}
    for record in data["records"]:
        if record.get("kind") != "match":
            continue
        video = record.get("youtubeId")
        start = record.get("startSeconds")
        if video is None or start is None:
            continue
        result.setdefault(video, []).append(float(start))
    return {key: sorted(value) for key, value in result.items()}


def fresh_segments(video: str) -> list[dict] | None:
    path = BATCHES / video / "segments.matches.json"
    if not path.exists():
        return None
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    return data.get("matches") or []


def pair(old: list[float], new: list[float], window: float = 5.0) -> list[float]:
    """Greedy nearest pairing; returns |delta| for the pairs found."""
    remaining = sorted(old)
    deltas = []
    for value in sorted(new):
        if not remaining:
            break
        nearest = min(remaining, key=lambda candidate: abs(candidate - value))
        if abs(nearest - value) <= window:
            deltas.append(abs(nearest - value))
            remaining.remove(nearest)
    return deltas


def main() -> int:
    starts = library_starts()
    print(f"{'video':13s} {'old':>4s} {'new':>4s} {'pairs':>6s} {'median|ds|':>11s} {'max|ds|':>8s} {'>1s':>5s}")
    total_old = total_new = total_pairs = 0
    all_deltas: list[float] = []
    for video in sorted(starts):
        matches = fresh_segments(video)
        if matches is None:
            continue
        new_starts = [float(m["analysis_start"]["seconds"]) for m in matches
                      if (m.get("analysis_start") or {}).get("seconds") is not None]
        deltas = pair(starts[video], new_starts)
        total_old += len(starts[video])
        total_new += len(new_starts)
        total_pairs += len(deltas)
        all_deltas.extend(deltas)
        over = sum(1 for d in deltas if d > 1.0)
        if deltas:
            print(f"{video:13s} {len(starts[video]):4d} {len(new_starts):4d} {len(deltas):6d} "
                  f"{statistics.median(deltas):11.3f} {max(deltas):8.3f} {over:5d}")
        else:
            print(f"{video:13s} {len(starts[video]):4d} {len(new_starts):4d} {0:6d} {'-':>11s} {'-':>8s} {0:5d}")
    if all_deltas:
        print(f"\nTOTAL old={total_old} new={total_new} pairs={total_pairs} "
              f"median={statistics.median(all_deltas):.3f}s mean={statistics.mean(all_deltas):.3f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
