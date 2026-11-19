"""Compare the quality warnings of two batch runs.

Usage:
    python tools/compare_warnings.py <a/quality-warnings.json> <b/quality-warnings.json>
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


def load(path: Path) -> tuple[set[int], dict[int, list[dict]]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    suspects: set[int] = set()
    issues: dict[int, list[dict]] = {}
    for match in data["matches"]:
        index = int(match["index"])
        warning = match.get("quality_warning") or {}
        found = warning.get("issues") or []
        issues[index] = found
        if warning.get("suspicious"):
            suspects.add(index)
    return suspects, issues


def describe(issue: dict) -> str:
    return (f"P{issue['player']} {issue['first_phase']}-{issue['last_phase']}"
            f" n={issue['consecutive_phases']} min={issue['minimum_match_ratio']:.2f}")


def summary(issues: dict[int, list[dict]]) -> tuple[int, int]:
    total = sum(len(v) for v in issues.values())
    suspicious = sum(1 for v in issues.values() if v)
    return total, suspicious


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    left_path, right_path = Path(sys.argv[1]), Path(sys.argv[2])
    left, left_issues = load(left_path)
    right, right_issues = load(right_path)
    left_total = sum(len(v) for v in left_issues.values())
    right_total = sum(len(v) for v in right_issues.values())
    print(f"A {left_path.parent.name}: warnings={len(left)} {sorted(left)} issues={left_total}")
    print(f"B {right_path.parent.name}: warnings={len(right)} {sorted(right)} issues={right_total}")
    print(f"\nonly in A: {sorted(left - right)}")
    print(f"only in B: {sorted(right - left)}")
    print("\nper-match issue counts (A -> B):")
    for index in sorted(set(left_issues) | set(right_issues)):
        a = len(left_issues.get(index, []))
        b = len(right_issues.get(index, []))
        if a != b:
            print(f"  match {index:3d}: {a:2d} -> {b:2d}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
