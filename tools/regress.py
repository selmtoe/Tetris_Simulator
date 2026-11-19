#!/usr/bin/env python3
"""One command: build -> deploy -> snapshot -> full runs -> validate -> compare -> record.

    python tools/regress.py run --label v55-foo --note "what changed"
    python tools/regress.py run --label recheck-v48 --skip-build --no-deploy   # re-run deployed exe
    python tools/regress.py run --label v55-foo --suites main56,lab4          # + 4 Lab videos x 12
    python tools/regress.py promote v55-foo                                    # make it the golden baseline
    python tools/regress.py suites                                             # list suites / baseline

Guarantees (HANDOFF.md section 3):
  * refuses to build when tools/video-recovery-source has uncommitted changes
  * build success is judged by the output exe's mtime, not PowerShell's exit code
  * the exe being replaced is archived in frozen/engines/ before deploying
  * snapshot_engine.py runs after every deploy
  * main56 (all 56 matches, --detect) always runs; outputs go to a fresh dir
  * every TR_* variable is removed from the engine's environment except
    TR_NVDEC_QUEUE_FRAME_STRIDE=1 and the ones passed with --env
  * the replay validator runs on every trace; any error fails the run and, if
    this run deployed a new exe, the previous exe is restored
  * traces are compared with the golden baseline (same suite, same mode)
  * light results go to frozen/runs/<label>/<suite>/, the trace golden to
    frozen/golden/<label>/, and one line to frozen/REGRESSION-LOG.jsonl

Exit code: 0 identical to baseline, 1 trace differences, 3 validator errors /
missing matches / engine failure, 2 refused to start.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import golden_trace as gt  # noqa: E402
import replay_validator as rv  # noqa: E402

SRC = ROOT / "tools" / "video-recovery-source"
RUNTIME = ROOT / "tools" / "video-recovery-runtime"
BIN = RUNTIME / "bin"
DEPLOYED = BIN / "TetrisVideoRecovery_stream.exe"
BUILD_DIR = ROOT / "scratch" / "end3" / "build"
WORK = Path(os.environ["REGRESS_WORK"]) if os.environ.get("REGRESS_WORK") else ROOT / "scratch" / "regress"
GOLDEN = ROOT / "frozen" / "golden"
SUITES = GOLDEN / "suites.json"
RUNS = ROOT / "frozen" / "runs"
ENGINES = ROOT / "frozen" / "engines"
LOG = ROOT / "frozen" / "REGRESSION-LOG.jsonl"
JST = timezone(timedelta(hours=9))
BUILD_FLAGS = ["-RangeBatch", "-NextGeneration", "-PerfectClearGuard"]
KEEP_FILES = ("batch-results.json", "quality-warnings.json", "segments.matches.json")


def now():
    return datetime.now(JST).isoformat(timespec="seconds")


def say(msg):
    print(f"[{datetime.now(JST):%H:%M:%S}] {msg}", flush=True)


def sha256(path: Path) -> str:
    if not path.is_file():
        return ""
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*args) -> str:
    r = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return r.stdout.strip()


def load_suites():
    with SUITES.open(encoding="utf-8-sig") as fh:
        return json.load(fh)


def resolve(p: str) -> Path:
    q = Path(p)
    return q if q.is_absolute() else ROOT / q


def runtime_files(cfg, suite="main56"):
    """The files actually handed to the engine (not a guess). A suite may
    override entries, e.g. the Lab goldens were produced with another scene model."""
    rt = dict(cfg["runtime"])
    rt.update(cfg["suites"].get(suite, {}).get("runtime", {}))
    if cfg.get("forceScene"):
        rt["scene"] = cfg["forceScene"]
    return {
        "scene": resolve(rt["scene"]), "board": resolve(rt["board"]), "ini": resolve(rt["ini"]),
        "next": resolve(rt["next"]),  # loaded implicitly by the exe from models/
    }


def runtime_shas(engine: Path, cfg, suite="main56"):
    out = {"engine": sha256(engine)}
    out.update({k: sha256(v) for k, v in runtime_files(cfg, suite).items()})
    return out


def expand_suites(names, cfg):
    groups = cfg.get("groups", {})
    out = []
    for n in names:
        for s in groups.get(n, [n]):
            if s not in cfg["suites"]:
                raise SystemExit(f"unknown suite: {s}")
            if s not in out:
                out.append(s)
    if "main56" not in out:
        out.insert(0, "main56")  # rule 5: always the full 56
    return out


def engine_env(extra):
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("TR_")}
    env["TR_NVDEC_QUEUE_FRAME_STRIDE"] = "1"
    # 2026-11-12: 3 workers overflow the 8 GB GPU (~2.5-3 GB each) and run 4-6x slower;
    # 2 workers did all 56 in 402 s with identical traces (frozen/runs/v6-combo-w2).
    env["TR_BATCH_WORKERS"] = "2"
    for kv in extra:
        k, _, v = kv.partition("=")
        if not k.startswith("TR_"):
            raise SystemExit(f"--env expects TR_NAME=value, got {kv!r}")
        env[k] = v
    return env, {k: v for k, v in env.items() if k.startswith("TR_")}


# ---------------------------------------------------------------- steps
def step_build(label):
    # Tracked source only: third_party/ etc. were never under git (see README).
    dirty = git("status", "--porcelain", "--untracked-files=no", "--", str(SRC.relative_to(ROOT)))
    if dirty:
        raise SystemExit("refused: tools/video-recovery-source has uncommitted changes "
                         "(commit before building):\n" + dirty)
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    out = BUILD_DIR / f"{label}.exe"
    started = time.time()
    log = WORK / label / "build.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    say(f"build -> {out.relative_to(ROOT)} (log {log.relative_to(ROOT)})")
    cmd = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
           str(SRC / "build_cuda.ps1"), *BUILD_FLAGS,
           "-OutputDirectory", str(BUILD_DIR), "-OutputName", out.name]
    with log.open("w", encoding="utf-8", errors="replace") as fh:
        rc = subprocess.run(cmd, cwd=ROOT, stdout=fh, stderr=subprocess.STDOUT).returncode
    text = log.read_text(encoding="utf-8", errors="replace")
    # build_cuda.ps1 may exit 1 on success (stderr warnings): trust the file.
    if not out.is_file() or out.stat().st_mtime < started or "Built " not in text:
        raise SystemExit(f"build failed (rc={rc}); see {log}")
    say(f"built (rc={rc}, ignored) sha={sha256(out)[:12]}")
    return out


def archive_current(label_hint):
    """Make sure the exe we are about to replace can be restored."""
    sha = sha256(DEPLOYED)
    if not sha:
        return None
    for f in os.listdir(ENGINES):
        if f.startswith(sha[:12]) and sha256(ENGINES / f) == sha:
            return ENGINES / f
    dst = ENGINES / f"{sha[:12]}_predeploy-{label_hint}.exe"
    shutil.copy2(DEPLOYED, dst)
    say(f"archived previously deployed exe -> {dst.relative_to(ROOT)}")
    return dst


def step_deploy(new_exe, label, note):
    previous = archive_current(label)
    shutil.copy2(new_exe, DEPLOYED)
    if sha256(DEPLOYED) != sha256(new_exe):
        raise SystemExit("deploy copy mismatch")
    say(f"deployed {new_exe.name} -> bin (previous: {previous.name if previous else 'none'})")
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "snapshot_engine.py"),
                        "--label", label, "--note", note or "regress.py"],
                       cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise SystemExit("snapshot_engine.py failed:\n" + r.stdout + r.stderr)
    say("snapshot recorded in frozen/ENGINE-SNAPSHOTS.jsonl")
    return previous


def run_suite(engine, suite, cfg, out_dir, env):
    s = cfg["suites"][suite]
    files = runtime_files(cfg, suite)
    video = resolve(s["video"])
    if not video.is_file():
        raise SystemExit(f"{suite}: video not found: {video}")
    if out_dir.exists():
        raise SystemExit(f"{suite}: output dir already exists (would mix match_XXX): {out_dir}")
    out_dir.mkdir(parents=True)
    if s["mode"] == "detect":
        cmd = [str(engine), str(video), "--detect", str(out_dir), str(files["scene"]),
               str(files["board"]), str(files["ini"])]
        if s.get("limit"):
            cmd.append(str(s["limit"]))
    else:
        cmd = [str(engine), str(video), str(resolve(s["manifest"])), str(out_dir),
               str(files["board"]), str(files["ini"])]
    log = out_dir.parent / f"{suite}.log"
    say(f"{suite}: running {s['mode']} on {video.name} -> {out_dir.relative_to(ROOT)}")
    t0 = time.time()
    with log.open("w", encoding="utf-8", errors="replace") as fh:
        rc = subprocess.run(cmd, cwd=ROOT, env=env, stdout=fh, stderr=subprocess.STDOUT).returncode
    el = time.time() - t0
    say(f"{suite}: engine rc={rc} in {el:.0f}s")
    return rc, el


def evaluate_suite(suite, cfg, out_dir, label, baseline):
    s = cfg["suites"][suite]
    val = rv.validate_batch(str(out_dir), explain=True)
    with (out_dir / "replay-validation.json").open("w", encoding="utf-8", newline="\n") as fh:
        json.dump(val, fh, ensure_ascii=False, indent=1)
    cand = gt.capture(str(out_dir), suite, s["mode"], label=label)
    expected = s.get("matches")
    gpath = GOLDEN / baseline / f"{suite}.golden.json" if baseline else None
    res = None
    if gpath and gpath.is_file():
        golden = rv.load_json(gpath)
        res = gt.compare(golden, cand)
        gt.print_compare(res, golden.get("label", baseline), label)
        with (out_dir / "golden-compare.json").open("w", encoding="utf-8", newline="\n") as fh:
            json.dump(res, fh, ensure_ascii=False, indent=1)
    else:
        say(f"{suite}: no golden for baseline {baseline!r}; nothing to compare")
    tot = val["totals"]
    return {
        "suite": suite, "mode": s["mode"], "matches": tot.get("matches", 0),
        "expectedMatches": expected,
        "warnings": cand["totals"]["warnings"], "moves": cand["totals"]["moves"],
        "validatorErrors": tot.get("errors", 0), "missingTraces": tot.get("missing_trace", 0),
        "validatorCodes": {k[5:]: v for k, v in tot.items() if k.startswith("code:")},
        "compare": res["summary"] if res else None, "comparable": res["comparable"] if res else None,
    }, cand


def save_results(label, suite, out_dir, cand):
    dst = RUNS / label / suite
    dst.mkdir(parents=True, exist_ok=True)
    for name in (*KEEP_FILES, "replay-validation.json", "golden-compare.json"):
        if (out_dir / name).is_file():
            shutil.copy2(out_dir / name, dst / name)
    gdir = GOLDEN / label
    gdir.mkdir(parents=True, exist_ok=True)
    with (gdir / f"{suite}.golden.json").open("w", encoding="utf-8", newline="\n") as fh:
        json.dump(cand, fh, ensure_ascii=False, separators=(",", ":"))


def apply_next_model(cfg, label, model):
    """Point every suite at a private models dir whose next_hold.onnx is `model`.

    The engine loads next_hold.onnx from the board model's directory, so the
    deployed models/ folder is never modified (HANDOFF rule 1)."""
    src = RUNTIME / "models"
    dst = WORK / label / "models"
    dst.mkdir(parents=True, exist_ok=True)
    for f in src.iterdir():
        if f.is_file() and f.name != "next_hold.onnx":
            shutil.copy2(f, dst / f.name)
    shutil.copy2(resolve(model), dst / "next_hold.onnx")
    cfg["runtime"]["board"] = str(dst / "tetris.onnx")
    cfg["runtime"]["next"] = str(dst / "next_hold.onnx")
    for suite in cfg["suites"].values():
        suite.get("runtime", {}).pop("board", None)
        suite.get("runtime", {}).pop("next", None)
    say(f"next model override: {model} sha={sha256(dst / 'next_hold.onnx')[:12]} -> {dst.relative_to(ROOT)}")


def cmd_run(args):
    cfg = load_suites()
    baseline = args.baseline or cfg.get("baseline")
    suites = expand_suites(args.suites.split(","), cfg)
    label = args.label
    if (WORK / label).exists() or (RUNS / label).exists() or (GOLDEN / label).exists():
        raise SystemExit(f"refused: label {label!r} already used (scratch/regress, frozen/runs or frozen/golden)")
    if args.skip_build and not args.exe and not args.no_deploy:
        say("note: --skip-build without --exe re-runs the deployed exe (no deploy, no snapshot)")
        args.no_deploy = True
    missing = [n for n in suites if not resolve(cfg["suites"][n]["video"]).is_file()]
    missing += [str(f) for n in suites for f in runtime_files(cfg, n).values() if not f.is_file()]
    if missing:
        raise SystemExit("refused: missing inputs: " + ", ".join(missing))
    env, tr_env = engine_env(args.env)
    say(f"label={label} baseline={baseline} suites={','.join(suites)} env={tr_env}")
    if args.dry_run:
        return 0

    record = {"savedAt": now(), "label": label, "note": args.note, "baseline": baseline,
              "git": git("rev-parse", "HEAD"), "gitDirty": git("status", "--porcelain") != "",
              "env": tr_env, "suites": [], "deployed": False}
    WORK.joinpath(label).mkdir(parents=True)
    if args.next_model:
        apply_next_model(cfg, label, args.next_model)
        record["nextModelOverride"] = args.next_model
    if args.scene_model:
        # main56 only: Lab suites keep their own (older) scene model for comparability
        cfg["runtime"]["scene"] = str(resolve(args.scene_model))
        record["sceneModelOverride"] = args.scene_model
        say(f"scene model override: {args.scene_model} sha={sha256(resolve(args.scene_model))[:12]}")
        if args.scene_all:
            # every suite, including the Lab ones (their goldens were made with an older scene model,
            # so boundaries will differ; compare warnings / validator errors, not moves)
            cfg["forceScene"] = str(resolve(args.scene_model))
            record["sceneModelAllSuites"] = True

    if args.skip_build:
        engine = resolve(args.exe) if args.exe else DEPLOYED
    else:
        engine = step_build(label)
    if not engine.is_file():
        raise SystemExit(f"engine not found: {engine}")

    previous = None
    run_engine = engine
    if not args.no_deploy and engine.resolve() != DEPLOYED.resolve():
        previous = step_deploy(engine, label, args.note)
        record["deployed"] = True
        run_engine = DEPLOYED
    elif engine.resolve() != DEPLOYED.resolve():
        # Must sit next to the DLLs/models; use a temp name, never the deployed one.
        run_engine = BIN / f"_regress_{label}.exe"
        shutil.copy2(engine, run_engine)
        if not args.skip_build:
            # A fresh build is always recorded, even when it is not deployed.
            r = subprocess.run([sys.executable, str(ROOT / "tools" / "snapshot_engine.py"),
                                "--label", label, "--note", (args.note or "regress.py") + " [not deployed]",
                                "--exe", str(engine)],
                               cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
            if r.returncode != 0:
                raise SystemExit("snapshot_engine.py failed: " + r.stdout + r.stderr)
            say("snapshot recorded (not deployed) in frozen/ENGINE-SNAPSHOTS.jsonl")
    record["runtime"] = runtime_shas(run_engine, cfg)
    if baseline:
        bg = GOLDEN / baseline / "main56.golden.json"
        if bg.is_file():
            brt = rv.load_json(bg).get("runtime", {})
            changed = [k for k, v in record["runtime"].items() if brt.get(k) and brt[k] != v]
            record["runtimeChangedVsBaseline"] = changed
            say(f"runtime vs baseline: {'changed ' + ','.join(changed) if changed else 'all identical'}")

    worst = 0
    try:
        for suite in suites:
            out_dir = WORK / label / suite
            rc, el = run_suite(run_engine, suite, cfg, out_dir, env)
            summary, cand = evaluate_suite(suite, cfg, out_dir, label, baseline)
            cand["runtime"] = runtime_shas(run_engine, cfg, suite)
            gpath = GOLDEN / baseline / f"{suite}.golden.json" if baseline else None
            if gpath and gpath.is_file():
                brt = rv.load_json(gpath).get("runtime", {})
                diff = [k for k, v in cand["runtime"].items() if brt.get(k) and brt[k] != v]
                summary["runtimeChangedVsBaseline"] = diff
                if diff:
                    say(f"{suite}: NOTE runtime differs from baseline golden: {','.join(diff)}")
            cand["env"] = tr_env
            summary.update({"engineExit": rc, "elapsedSeconds": round(el, 1)})
            save_results(label, suite, out_dir, cand)
            record["suites"].append(summary)
            bad = (summary["validatorErrors"] or summary["missingTraces"] or
                   (summary["expectedMatches"] and summary["matches"] != summary["expectedMatches"]))
            if bad:
                worst = 3
                say(f"{suite}: FAIL validatorErrors={summary['validatorErrors']} "
                    f"missing={summary['missingTraces']} matches={summary['matches']}/{summary['expectedMatches']}")
            elif summary["compare"] and not (summary["comparable"] and
                                             summary["compare"]["identical"] == summary["compare"]["matches"]):
                worst = max(worst, 1)
    finally:
        if run_engine.name.startswith("_regress_"):
            run_engine.unlink(missing_ok=True)

    if worst == 3 and previous is not None:
        shutil.copy2(previous, DEPLOYED)
        record["restored"] = previous.name
        say(f"validator/engine failure: restored previous exe {previous.name}")
    record["result"] = {0: "identical", 1: "trace-changed", 3: "failed"}[worst]
    with LOG.open("a", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(record, ensure_ascii=False) + "\n")
    with (RUNS / label / "regress-record.json").open("w", encoding="utf-8", newline="\n") as fh:
        json.dump(record, fh, ensure_ascii=False, indent=1)
    say(f"result={record['result']}  record -> frozen/REGRESSION-LOG.jsonl, frozen/runs/{label}/")
    if worst == 1 and record["deployed"]:
        prev = previous.relative_to(ROOT) if previous else "?"
        print(f"  traces changed. To roll back:  copy {prev} -> {DEPLOYED.relative_to(ROOT)}")
    return worst


def cmd_promote(args):
    cfg = load_suites()
    gdir = GOLDEN / args.label
    if not (gdir / "main56.golden.json").is_file():
        raise SystemExit(f"no main56 golden for {args.label}")
    g = rv.load_json(gdir / "main56.golden.json")
    if g["totals"]["validatorErrors"] or g["totals"]["missing"] or g["totals"]["matches"] != 56:
        raise SystemExit(f"refused: {args.label} main56 is not clean: {g['totals']}")
    old = cfg.get("baseline")
    cfg["baseline"] = args.label
    cfg.setdefault("history", []).append({"at": now(), "from": old, "to": args.label, "reason": args.reason})
    with SUITES.open("w", encoding="utf-8", newline="\n") as fh:
        json.dump(cfg, fh, ensure_ascii=False, indent=1)
    print(f"baseline: {old} -> {args.label}")
    return 0


def cmd_suites(_args):
    cfg = load_suites()
    print(f"baseline: {cfg.get('baseline')}")
    for n, s in cfg["suites"].items():
        print(f"  {n:20s} {s['mode']:8s} matches={s.get('matches')} {s['video']}")
    for n, g in cfg.get("groups", {}).items():
        print(f"  group {n}: {','.join(g)}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--label", required=True)
    r.add_argument("--note", default="")
    r.add_argument("--suites", default="main56", help="comma list of suites or groups (main56 always included)")
    r.add_argument("--baseline", help="golden label to compare with (default: suites.json baseline)")
    r.add_argument("--skip-build", action="store_true")
    r.add_argument("--exe", help="with --skip-build: test this exe instead of building")
    r.add_argument("--no-deploy", action="store_true", help="run from a temp name; bin exe untouched, no snapshot")
    r.add_argument("--env", action="append", default=[], metavar="TR_NAME=value")
    r.add_argument("--scene-model", help="test this scene-classifier onnx for main56 (suites with their own scene model keep it)")
    r.add_argument("--scene-all", action="store_true", help="with --scene-model: use it for every suite, not only main56")
    r.add_argument("--next-model", help="test this NEXT/HOLD onnx without touching models/: a copy of "
                   "models/ with only next_hold.onnx replaced is made under scratch/regress/<label>/models")
    r.add_argument("--dry-run", action="store_true")
    p = sub.add_parser("promote")
    p.add_argument("label")
    p.add_argument("--reason", required=True)
    sub.add_parser("suites")
    args = ap.parse_args(argv)
    try:
        return {"run": cmd_run, "promote": cmd_promote, "suites": cmd_suites}[args.cmd](args)
    except SystemExit as e:
        if isinstance(e.code, str):
            print(e.code, file=sys.stderr)
            return 2
        raise


if __name__ == "__main__":
    sys.exit(main())
