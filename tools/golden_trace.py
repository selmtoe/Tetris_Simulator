#!/usr/bin/env python3
"""Golden regression for recovered board traces.

Warning counts alone hide regressions: "8 warnings -> 8 warnings" happened while
a different set of matches broke, and "8 -> 0" happened while 2,540 of 8,279
moves changed.  This tool freezes the *trace itself* and compares it.

  capture  <batch-dir> --suite NAME --mode detect|manifest --out golden.json
  compare  <golden.json> <batch-dir | other-golden.json> [--report out.json]

A golden file stores, per match and per player, one entry per phase:
  move  : "<piece><rot>@x,y c<cleared> g<lines>:<masks>"  (what was played)
  board : 12-hex sha1 of board + fullBoard                 (what it produced)
plus the match's source range, quality warning flag and validator result.

Exit code of `compare`: 0 identical, 1 differences, 2 not comparable
(different suite/mode/video), unless --allow-diff is given (then 0).
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
import sys
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import replay_validator as rv  # noqa: E402

SCHEMA = "tetris-golden-trace/v1"


def _move(st) -> str:
    pl = st.get("placement") or {}
    g = st.get("garbage") or {}
    piece = pl.get("piece", "")
    head = (f"{piece}{pl.get('rotation', 0)}@{pl.get('x', 0)},{pl.get('y', 0)}"
            f"c{pl.get('clearedLines', 0)}") if piece else f"-{st.get('action', '')}"
    if g.get("lines"):
        head += f" g{g['lines']}:" + ",".join(str(m) for m in g.get("holeMasks", []))
    if pl.get("holdUsed"):
        head += " h"
    return head


def _board_hash(st) -> str:
    h = hashlib.sha1()
    h.update("".join(st.get("board") or []).encode("ascii", "replace"))
    h.update(b"|")
    h.update("".join(st.get("fullBoard") or []).encode("ascii", "replace"))
    return h.hexdigest()[:12]


def _chain(entries) -> str:
    h = hashlib.sha1()
    for e in entries:
        h.update(f"{e['move']}#{e['board']};".encode())
    return h.hexdigest()[:16]


def capture(batch_dir, suite, mode, label="", note=""):
    br_path = os.path.join(batch_dir, "batch-results.json")
    br = rv.load_json(br_path) if os.path.exists(br_path) else {"matches": []}
    by_index = {m.get("index"): m for m in br.get("matches", [])}
    out = {
        "schema": SCHEMA, "suite": suite, "mode": mode, "label": label, "note": note,
        "capturedAt": datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds"),
        "source": br.get("source", ""), "batchDir": os.path.abspath(batch_dir),
        "elapsedSeconds": br.get("elapsed_seconds"), "matches": {},
    }
    for name, rec in rv.iter_traces(batch_dir):
        idx = int(name.split("_")[-1]) if name.startswith("match_") else None
        meta = by_index.get(idx, {})
        # None = unknown (no batch-results.json); compare() skips unknown fields.
        sr = meta.get("source_range")
        qw = meta.get("quality_warning")
        entry = {
            "status": meta.get("status", ""),
            "range": [round(sr["start_seconds"], 3), round(sr["end_seconds"], 3)] if sr else None,
            "warning": bool(qw.get("suspicious", False)) if qw else None,
        }
        if rec is None:
            entry["missing"] = True
            out["matches"][name] = entry
            continue
        data, issues = rv.validate_trace(rec, explain=False)
        entry["validatorErrors"] = sum(1 for i in issues if i.severity == "error")
        entry["engineUnresolved"] = sum(bool(s.get("recoveryUnresolved"))
                                        for p in ("p1", "p2") for s in data.get(p) or [])
        for p in ("p1", "p2"):
            phases = [{"t": round(float(s.get("time", 0)), 3), "move": _move(s), "board": _board_hash(s)}
                      for s in data.get(p) or []]
            entry[p] = {"chain": _chain(phases), "phases": phases}
        out["matches"][name] = entry
    out["totals"] = summarize(out)
    return out


def summarize(g):
    ms = g["matches"].values()
    return {
        "matches": len(g["matches"]),
        "warnings": sum(bool(m.get("warning")) for m in ms),
        "warningsKnown": all(m.get("warning") is not None for m in ms if not m.get("missing")),
        "moves": sum(len(m[p]["phases"]) for m in ms for p in ("p1", "p2") if p in m),
        "validatorErrors": sum(m.get("validatorErrors", 0) for m in ms),
        "missing": sum(bool(m.get("missing")) for m in ms),
    }


def load_golden_or_capture(path, suite, mode):
    if os.path.isfile(path):
        g = rv.load_json(path)
        if g.get("schema") != SCHEMA:
            raise SystemExit(f"{path}: not a golden file")
        return g
    return capture(path, suite, mode, label="(candidate)")


def _player_diff(a, b):
    """Return dict describing differences of one player's phase list."""
    am = [e["move"] for e in a["phases"]]
    bm = [e["move"] for e in b["phases"]]
    first = None
    for i, (x, y) in enumerate(zip(a["phases"], b["phases"])):
        if x["move"] != y["move"] or x["board"] != y["board"]:
            first = {"phase": i, "t_golden": x["t"], "t_candidate": y["t"],
                     "golden": f"{x['move']} [{x['board']}]", "candidate": f"{y['move']} [{y['board']}]"}
            break
    if first is None and len(am) != len(bm):
        i = min(len(am), len(bm))
        first = {"phase": i, "golden": "(end)" if i >= len(am) else am[i],
                 "candidate": "(end)" if i >= len(bm) else bm[i]}
    sm = difflib.SequenceMatcher(a=[f"{e['move']}#{e['board']}" for e in a["phases"]],
                                 b=[f"{e['move']}#{e['board']}" for e in b["phases"]], autojunk=False)
    same = sum(blk.size for blk in sm.get_matching_blocks())
    return {"first": first, "golden_phases": len(am), "candidate_phases": len(bm),
            "unchanged": same, "changed": max(len(am), len(bm)) - same}


def compare(golden, cand):
    problems = []
    for key in ("suite", "mode"):
        if golden.get(key) != cand.get(key) and cand.get("label") != "(candidate)":
            problems.append(f"{key}: golden={golden.get(key)!r} candidate={cand.get(key)!r}")
    gs, cs = os.path.basename(golden.get("source", "")), os.path.basename(cand.get("source", ""))
    if gs and cs and gs != cs:
        problems.append(f"source video: golden={gs} candidate={cs}")
    res = {"comparable": not problems, "problems": problems, "matches": {}, "summary": {}}
    total_moves = changed_moves = identical = 0
    w_added, w_removed, boundary = [], [], []
    for name in sorted(set(golden["matches"]) | set(cand["matches"])):
        g, c = golden["matches"].get(name), cand["matches"].get(name)
        if g is None or c is None or g.get("missing") or c.get("missing"):
            res["matches"][name] = {"status": "missing-in-" + ("golden" if not g or g.get("missing") else "candidate")}
            continue
        r = {"status": "identical"}
        if None not in (g.get("range"), c.get("range")) and g.get("range") != c.get("range"):
            r["range"] = {"golden": g.get("range"), "candidate": c.get("range")}
            boundary.append(name)
        if None not in (g.get("warning"), c.get("warning")) and g.get("warning") != c.get("warning"):
            (w_added if c.get("warning") else w_removed).append(name)
            r["warning"] = f"{g.get('warning')} -> {c.get('warning')}"
        if c.get("validatorErrors", 0) > g.get("validatorErrors", 0):
            r["validator"] = f"{g.get('validatorErrors', 0)} -> {c.get('validatorErrors', 0)}"
        for p in ("p1", "p2"):
            total_moves += len(g[p]["phases"])
            if g[p]["chain"] == c[p]["chain"]:
                continue
            d = _player_diff(g[p], c[p])
            r[p] = d
            changed_moves += d["changed"]
        if any(k in r for k in ("p1", "p2")):
            r["status"] = "trace-changed"
        elif len(r) > 1:
            r["status"] = "meta-changed"
        else:
            identical += 1
        res["matches"][name] = r
    res["summary"] = {
        "matches": len(res["matches"]), "identical": identical,
        "goldenMoves": total_moves, "changedMoves": changed_moves,
        "warningsGolden": golden["totals"]["warnings"] if golden["totals"].get("warningsKnown", True) else None,
        "warningsCandidate": cand["totals"]["warnings"] if cand["totals"].get("warningsKnown", True) else None,
        "warningsAdded": w_added, "warningsRemoved": w_removed,
        "boundaryChanged": boundary,
        "validatorErrorsCandidate": cand["totals"]["validatorErrors"],
        "missing": [n for n, r in res["matches"].items() if r["status"].startswith("missing")],
    }
    return res


def print_compare(res, golden_label, cand_label, limit=20):
    s = res["summary"]
    print(f"golden={golden_label}  candidate={cand_label}")
    for p in res["problems"]:
        print(f"  NOT COMPARABLE: {p}")
    print(f"  identical matches : {s['identical']}/{s['matches']}")
    print(f"  changed moves     : {s['changedMoves']}/{s['goldenMoves']}")
    unk = lambda v: "unknown" if v is None else v  # noqa: E731
    print(f"  warnings          : {unk(s['warningsGolden'])} -> {unk(s['warningsCandidate'])}"
          f"  (+{','.join(s['warningsAdded']) or '-'}  -{','.join(s['warningsRemoved']) or '-'})")
    print(f"  validator errors  : {s['validatorErrorsCandidate']}")
    if s["boundaryChanged"]:
        print(f"  boundary changed  : {', '.join(s['boundaryChanged'])}")
    if s["missing"]:
        print(f"  missing           : {', '.join(s['missing'])}")
    shown = 0
    for name, r in res["matches"].items():
        if r["status"] == "identical" or shown >= limit:
            continue
        shown += 1
        bits = [r["status"]]
        for p in ("p1", "p2"):
            if p in r:
                f = r[p]["first"] or {}
                bits.append(f"{p}: {r[p]['changed']} moves changed, first #{f.get('phase')} "
                            f"t={f.get('t_golden', '?')} {f.get('golden')} -> {f.get('candidate')}")
        for k in ("warning", "validator", "range"):
            if k in r:
                bits.append(f"{k} {r[k]}")
        print(f"  {name}: " + " | ".join(bits))


def is_clean(res):
    s = res["summary"]
    return (res["comparable"] and s["identical"] == s["matches"] and not s["missing"]
            and s["validatorErrorsCandidate"] == 0)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("batch_dir")
    c.add_argument("--suite", required=True)
    c.add_argument("--mode", required=True, choices=["detect", "manifest"])
    c.add_argument("--out", required=True)
    c.add_argument("--label", default="")
    c.add_argument("--note", default="")
    k = sub.add_parser("compare")
    k.add_argument("golden")
    k.add_argument("candidate", help="batch dir or golden json")
    k.add_argument("--suite", help="suite name when candidate is a batch dir (default: golden's)")
    k.add_argument("--mode", help="mode when candidate is a batch dir (default: golden's)")
    k.add_argument("--report")
    k.add_argument("--allow-diff", action="store_true")
    args = ap.parse_args(argv)

    if args.cmd == "capture":
        g = capture(args.batch_dir, args.suite, args.mode, args.label, args.note)
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(g, fh, ensure_ascii=False, separators=(",", ":"))
        t = g["totals"]
        print(f"captured {args.out}: matches={t['matches']} moves={t['moves']} "
              f"warnings={t['warnings']} validatorErrors={t['validatorErrors']} missing={t['missing']}")
        return 0

    golden = rv.load_json(args.golden)
    if os.path.isfile(args.candidate):
        cand = rv.load_json(args.candidate)
    else:
        cand = capture(args.candidate, args.suite or golden["suite"], args.mode or golden["mode"],
                       label=os.path.basename(os.path.normpath(args.candidate)))
    res = compare(golden, cand)
    print_compare(res, golden.get("label") or args.golden, cand.get("label") or args.candidate)
    if args.report:
        with open(args.report, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(res, fh, ensure_ascii=False, indent=1)
    if not res["comparable"]:
        return 2
    return 0 if (args.allow_diff or is_clean(res)) else 1


if __name__ == "__main__":
    sys.exit(main())
