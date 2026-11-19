"""Summarise the quality warnings of every analysed Lab-site video.

Usage: python tools/labsite_summary.py [batch-root]
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else r"scratch\labsite\batches")


def main() -> int:
    rows = []
    for directory in sorted(ROOT.iterdir()):
        warning_file = directory / "quality-warnings.json"
        if not warning_file.exists():
            continue
        data = json.loads(warning_file.read_text(encoding="utf-8"))
        matches = data.get("matches", [])
        done = [m for m in matches if m.get("status") == "complete"]
        suspicious = [int(m["index"]) for m in done
                      if (m.get("quality_warning") or {}).get("suspicious")]
        issues = sum(len((m.get("quality_warning") or {}).get("issues") or [])
                     for m in done)
        players = {}
        for m in done:
            for issue in (m.get("quality_warning") or {}).get("issues") or []:
                players[issue["player"]] = players.get(issue["player"], 0) + 1
        rows.append((directory.name, len(done), len(suspicious), suspicious, issues, players))
    print(f"{'video':14s} {'matches':>7s} {'warn':>5s} {'issues':>7s}  {'P1/P2':>8s}  matches")
    total_m = total_w = total_i = 0
    for name, count, warn, suspicious, issues, players in rows:
        total_m += count
        total_w += warn
        total_i += issues
        share = f"{players.get(1, 0)}/{players.get(2, 0)}"
        print(f"{name:14s} {count:7d} {warn:5d} {issues:7d}  {share:>8s}  {suspicious}")
    print(f"{'TOTAL':14s} {total_m:7d} {total_w:5d} {total_i:7d}")
    if total_m:
        print(f"warning rate: {100.0 * total_w / total_m:.1f}% of matches, "
              f"{total_i / total_m:.2f} issues per match")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
