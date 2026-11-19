"""Cut every quality-warning MATCH into its own clip and build a study page.

The unit is the whole match, not the flagged interval: each card plays the match
from its detected gameplay start to its end and lists every flagged interval.

The detected start is already the first gameplay frame (GO-exit rule), so the
clip must not add any pre-roll: otherwise the GO! animation reappears at the top
of every clip.

Usage:
    python tools/build_warning_matches.py
"""
from __future__ import annotations

import argparse
import html
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "review" / "warning-matches"
CLIPS = OUT / "clips"
FFMPEG = ROOT / "tools" / "video-recovery-runtime" / "bin" / "ffmpeg.exe"
FFPROBE = ROOT / "tools" / "video-recovery-runtime" / "bin" / "ffprobe.exe"
TAIL_SECONDS = 0.0

MAIN_VIDEO = Path(
    r"C:\Users\hirom\AppData\Local\PPTLab\media\video-799b6882cb7d40f0\[yD_ZukVrgHY].mkv")


def main_sources() -> list[dict]:
    run = ROOT / "frozen" / "runs" / "goexit-360p30-stride1-8warn"
    sources = []
    if (run / "quality-warnings.json").exists() and MAIN_VIDEO.exists():
        sources.append({"group": "main", "label": "360p30 56試合",
                        "video": str(MAIN_VIDEO),
                        "warnings": str(run / "quality-warnings.json"),
                        "matches": str(run / "batch-results.json")})
    return sources


def labsite_sources() -> list[dict]:
    sources = []
    batches = ROOT / "scratch" / "labsite" / "batches"
    media = ROOT / "scratch" / "labsite" / "media"
    if not batches.is_dir():
        return sources
    for directory in sorted(batches.iterdir()):
        warning = directory / "quality-warnings.json"
        results = directory / "batch-results.json"
        video = media / f"{directory.name}.mp4"
        if warning.exists() and results.exists() and video.exists():
            sources.append({"group": "labsite", "label": directory.name,
                            "video": str(video),
                            "warnings": str(warning), "matches": str(results)})
    return sources


def probe_duration(path: Path) -> float:
    result = subprocess.run(
        [str(FFPROBE), "-v", "error", "-show_entries", "format=duration",
         "-of", "csv=p=0", str(path)], capture_output=True, text=True)
    try:
        return float(result.stdout.strip().splitlines()[0])
    except (ValueError, IndexError):
        return 0.0


def cut(video: Path, start: float, duration: float, target: Path) -> bool:
    target.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(FFMPEG), "-hide_banner", "-loglevel", "error",
        "-ss", f"{max(0.0, start):.3f}", "-i", str(video),
        "-t", f"{duration:.3f}", "-an",
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "26",
        "-pix_fmt", "yuv420p", "-profile:v", "high", "-level", "4.0",
        "-movflags", "+faststart", "-y", str(target),
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  ffmpeg failed: {result.stderr.strip()[:200]}")
        return False
    return target.exists() and target.stat().st_size > 5000


def collect(sources: list[dict]) -> list[dict]:
    entries = []
    for source in sources:
        warnings = json.loads(Path(source["warnings"]).read_text(encoding="utf-8-sig"))
        results = json.loads(Path(source["matches"]).read_text(encoding="utf-8-sig"))
        ranges = {}
        for match in results.get("matches", []):
            span = match.get("source_range") or {}
            if span.get("start_seconds") is not None:
                ranges[int(match["index"])] = (float(span["start_seconds"]),
                                               float(span["end_seconds"]))
        for match in warnings.get("matches", []):
            quality = match.get("quality_warning") or {}
            if not quality.get("suspicious"):
                continue
            index = int(match["index"])
            if index not in ranges:
                continue
            start, end = ranges[index]
            issues = [{
                "player": int(issue["player"]),
                "first": int(issue["first_phase"]),
                "last": int(issue["last_phase"]),
                "phases": int(issue["consecutive_phases"]),
                "minimum": float(issue["minimum_match_ratio"]),
            } for issue in (quality.get("issues") or [])]
            entries.append({
                "group": source["group"], "label": source["label"],
                "video": source["video"], "match": index,
                "start": start, "end": end, "issues": issues,
                "players": sorted({issue["player"] for issue in issues}),
                "minimum": min(issue["minimum"] for issue in issues),
            })
    return entries


def render(entries: list[dict]) -> str:
    labels = sorted({entry["label"] for entry in entries})
    options = "\n".join(f'<option value="{html.escape(name)}">{html.escape(name)}</option>'
                        for name in labels)
    cards = []
    for entry in entries:
        rows = []
        for issue in entry["issues"]:
            cls = "bad" if issue["minimum"] < 0.6 else "warn"
            rows.append(
                f'<li><span class="tag {cls}">P{issue["player"]}</span>'
                f' ph {issue["first"]}–{issue["last"]} ・ {issue["phases"]}フェーズ'
                f' ・ 最低一致率 {issue["minimum"]:.2f}</li>')
        rows = "\n".join(rows)
        cards.append(f"""
        <article class="card" data-label="{html.escape(entry['label'])}"
                 data-players="{','.join('P' + str(p) for p in entry['players'])}"
                 data-ratio="{entry['minimum']:.3f}" data-group="{entry['group']}">
          <header>
            <strong>{html.escape(entry['label'])} · 試合{entry['match']:02d}</strong>
            <span class="muted">{entry['end'] - entry['start']:.1f}秒 / {len(entry['issues'])}区間</span>
          </header>
          <video src="clips/{entry['slug']}.mp4" preload="metadata" controls playsinline></video>
          <ul class="issues">{rows}</ul>
          <footer>ゲーム内 {entry['start']:.2f}–{entry['end']:.2f}s</footer>
        </article>""")
    body = "\n".join(cards)
    return f"""<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>品質警告が出た試合</title>
<style>
 body {{ margin:0; font-family:-apple-system,"Segoe UI","Noto Sans JP",sans-serif;
        background:#f6f7f9; color:#1b1f24; }}
 header.top {{ position:sticky; top:0; z-index:5; background:#fff; border-bottom:1px solid #e3e6ea;
               padding:12px 20px; display:flex; gap:16px; align-items:center; flex-wrap:wrap; }}
 h1 {{ font-size:16px; margin:0; }}
 select, input {{ font:inherit; padding:4px 8px; border:1px solid #cfd4da; border-radius:6px; background:#fff; }}
 main {{ padding:20px; display:grid; gap:16px;
         grid-template-columns:repeat(auto-fill,minmax(420px,1fr)); }}
 .card {{ background:#fff; border:1px solid #e3e6ea; border-radius:10px; overflow:hidden; }}
 .card header {{ display:flex; gap:10px; align-items:center; padding:10px 12px; flex-wrap:wrap; }}
 .card video {{ width:100%; display:block; background:#000; aspect-ratio:16/9; }}
 .issues {{ margin:0; padding:8px 12px 8px 28px; font-size:12px; color:#3d444c; }}
 .issues li {{ margin:3px 0; }}
 .card footer {{ padding:6px 12px 10px; font-size:11px; color:#78818c; }}
 .tag {{ font-size:10px; font-weight:700; padding:1px 5px; border-radius:4px; color:#fff; }}
 .tag.bad {{ background:#c0392b; }} .tag.warn {{ background:#d68910; }}
 .muted {{ color:#78818c; font-size:12px; }}
 .count {{ margin-left:auto; font-size:12px; color:#5b636d; }}
</style></head><body>
<header class="top">
  <h1>品質警告が出た試合</h1>
  <label>動画 <select id="label"><option value="">すべて</option>{options}</select></label>
  <label>手番 <select id="player"><option value="">両方</option>
    <option value="P1">P1に警告</option><option value="P2">P2に警告</option></select></label>
  <label>最低一致率 <input id="ratio" type="range" min="0.3" max="0.7" step="0.01" value="0.7"></label>
  <span id="count" class="count"></span>
</header>
<main id="grid">{body}</main>
<script>
 const cards=[...document.querySelectorAll('.card')];
 const state={{label:'',player:'',ratio:0.7}};
 function apply(){{
   let shown=0;
   for(const card of cards){{
     const okLabel=!state.label||card.dataset.label===state.label;
     const okPlayer=!state.player||card.dataset.players.includes(state.player);
     const okRatio=parseFloat(card.dataset.ratio)<=state.ratio;
     const visible=okLabel&&okPlayer&&okRatio;
     card.style.display=visible?'':'none';
     if(visible) shown++;
   }}
   document.getElementById('count').textContent=`${{shown}} / ${{cards.length}} 試合`;
 }}
 document.getElementById('label').onchange=e=>{{state.label=e.target.value;apply();}};
 document.getElementById('player').onchange=e=>{{state.player=e.target.value;apply();}};
 document.getElementById('ratio').oninput=e=>{{state.ratio=parseFloat(e.target.value);apply();}};
 apply();
</script></body></html>
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", choices=["main", "labsite"])
    parser.add_argument("--force", action="store_true",
                        help="re-cut clips that already exist")
    args = parser.parse_args()
    sources = main_sources() + labsite_sources()
    if args.only == "main":
        sources = main_sources()
    elif args.only == "labsite":
        sources = labsite_sources()
    if not sources:
        print("no sources")
        return 1
    entries = collect(sources)
    print(f"{len(entries)} warning matches from {len(sources)} videos")
    durations: dict[str, float] = {}
    made = 0
    for entry in entries:
        video = Path(entry["video"])
        key = str(video)
        if key not in durations:
            durations[key] = probe_duration(video)
        slug = f"{video.stem}-m{entry['match']:02d}"
        slug = "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in slug)
        entry["slug"] = slug
        target = CLIPS / f"{slug}.mp4"
        if args.force and target.exists():
            target.unlink()
        start = max(0.0, entry["start"])
        duration = entry["end"] + TAIL_SECONDS - start
        limit = durations[key]
        if limit > 0:
            duration = min(duration, limit - start)
        if cut(video, start, max(1.0, duration), target):
            made += 1
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "index.html").write_text(render(entries), encoding="utf-8")
    total = sum((CLIPS / f"{entry['slug']}.mp4").stat().st_size
                for entry in entries if (CLIPS / f"{entry['slug']}.mp4").exists())
    print(f"clips={made}/{len(entries)} total={total/1e6:.1f}MB")
    print(f"page: {OUT / 'index.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
