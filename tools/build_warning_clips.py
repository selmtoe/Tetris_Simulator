"""Cut every quality-warning interval into a short clip and build a study page.

For each match flagged with `quality_warning.suspicious` the tool extracts the
reported interval (plus a few seconds of context) from the source video, writes
it to `review/warning-clips/clips/` and generates `review/warning-clips/index.html`
with per-video / per-player filters.

Usage:
    python tools/build_warning_clips.py                 # all known sources
    python tools/build_warning_clips.py --only labsite  # one source group
"""
from __future__ import annotations

import argparse
import html
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "review" / "warning-clips"
CLIPS = OUT / "clips"
FFMPEG = ROOT / "tools" / "video-recovery-runtime" / "bin" / "ffmpeg.exe"
PADDING_SECONDS = 4.0

MAIN_VIDEO = Path(
    r"C:\Users\hirom\AppData\Local\PPTLab\media\video-799b6882cb7d40f0\[yD_ZukVrgHY].mkv")


def labsite_sources() -> list[dict]:
    sources = []
    batches = ROOT / "scratch" / "labsite" / "batches"
    media = ROOT / "scratch" / "labsite" / "media"
    if not batches.is_dir():
        return sources
    for directory in sorted(batches.iterdir()):
        warning = directory / "quality-warnings.json"
        video = media / f"{directory.name}.mp4"
        if warning.exists() and video.exists():
            sources.append({"group": "labsite", "label": directory.name,
                            "video": str(video), "warnings": str(warning)})
    return sources


def main_sources() -> list[dict]:
    """Only the run that matches the currently deployed video/settings."""
    preferred = "goexit-360p30-stride1-8warn"
    sources = []
    for run in sorted((ROOT / "frozen" / "runs").iterdir()):
        if run.name != preferred:
            continue
        warning = run / "quality-warnings.json"
        if warning.exists() and MAIN_VIDEO.exists():
            sources.append({"group": "main", "label": run.name,
                            "video": str(MAIN_VIDEO), "warnings": str(warning)})
    return sources


def probe_duration(path: Path) -> float:
    probe = ROOT / "tools" / "video-recovery-runtime" / "bin" / "ffprobe.exe"
    result = subprocess.run(
        [str(probe), "-v", "error", "-show_entries", "format=duration",
         "-of", "csv=p=0", str(path)], capture_output=True, text=True)
    try:
        return float(result.stdout.strip().splitlines()[0])
    except (ValueError, IndexError):
        return 0.0


def cut(video: Path, start: float, duration: float, target: Path) -> bool:
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() and target.stat().st_size > 10000:
        return True
    command = [
        str(FFMPEG), "-hide_banner", "-loglevel", "error",
        "-ss", f"{max(0.0, start):.3f}", "-i", str(video),
        "-t", f"{duration:.3f}",
        "-an", "-c:v", "libx264", "-preset", "veryfast", "-crf", "26",
        "-vf", "scale=640:-2", "-movflags", "+faststart", "-y", str(target),
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  ffmpeg failed: {result.stderr.strip()[:200]}")
        return False
    return target.exists() and target.stat().st_size > 5000


def collect(sources: list[dict]) -> list[dict]:
    items = []
    for source in sources:
        warning_path = Path(source["warnings"])
        data = json.loads(warning_path.read_text(encoding="utf-8-sig"))
        for match in data.get("matches", []):
            quality = match.get("quality_warning") or {}
            if not quality.get("suspicious"):
                continue
            for index, issue in enumerate(quality.get("issues") or []):
                start = float(issue.get("source_start_seconds") or 0.0)
                end = float(issue.get("source_end_seconds") or start)
                items.append({
                    "group": source["group"],
                    "label": source["label"],
                    "video": source["video"],
                    "match": int(match["index"]),
                    "issue": index,
                    "player": int(issue["player"]),
                    "first_phase": int(issue["first_phase"]),
                    "last_phase": int(issue["last_phase"]),
                    "phases": int(issue["consecutive_phases"]),
                    "minimum": float(issue["minimum_match_ratio"]),
                    "start": start,
                    "end": end,
                })
    return items


def render(items: list[dict], durations: dict[str, float]) -> str:
    videos = sorted({item["label"] for item in items})
    options = "\n".join(
        f'<option value="{html.escape(name)}">{html.escape(name)}</option>'
        for name in videos)
    cards = []
    for item in items:
        clip_start = max(0.0, item["start"] - PADDING_SECONDS)
        clip_end = item["end"] + PADDING_SECONDS
        duration = durations.get(item["video"], 0.0)
        if duration > 0:
            clip_end = min(clip_end, duration)
        clip_duration = max(1.0, clip_end - clip_start)
        item["clip"] = f"clips/{item['slug']}.mp4"
        item["clip_start"] = clip_start
        item["duration"] = clip_duration
        ratio_class = "bad" if item["minimum"] < 0.6 else "warn"
        cards.append(f"""
        <article class="card" data-label="{html.escape(item['label'])}"
                 data-player="P{item['player']}" data-ratio="{item['minimum']:.3f}"
                 data-group="{item['group']}">
          <header>
            <span class="tag {ratio_class}">P{item['player']}</span>
            <strong>{html.escape(item['label'])} · 試合{item['match']:02d}</strong>
            <span class="muted">一致率 {item['minimum']:.2f} / {item['phases']}フェーズ</span>
          </header>
          <video src="{item['clip']}" preload="metadata" controls playsinline></video>
          <footer>
            <span>元動画 {item['start']:.2f}–{item['end']:.2f}s</span>
            <span>切り出し {clip_start:.2f}s から {clip_duration:.2f}秒</span>
            <span class="muted">ph {item['first_phase']}–{item['last_phase']}</span>
          </footer>
        </article>""")
    body = "\n".join(cards)
    return f"""<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>品質警告クリップ一覧</title>
<style>
 :root {{ color-scheme: light dark; }}
 body {{ margin:0; font-family:-apple-system,"Segoe UI","Noto Sans JP",sans-serif;
        background:#f6f7f9; color:#1b1f24; }}
 header.top {{ position:sticky; top:0; z-index:5; background:#fff; border-bottom:1px solid #e3e6ea;
               padding:12px 20px; display:flex; gap:16px; align-items:center; flex-wrap:wrap; }}
 h1 {{ font-size:16px; margin:0; }}
 select, input {{ font:inherit; padding:4px 8px; border:1px solid #cfd4da; border-radius:6px; background:#fff; }}
 main {{ padding:20px; display:grid; gap:16px;
         grid-template-columns:repeat(auto-fill,minmax(380px,1fr)); }}
 .card {{ background:#fff; border:1px solid #e3e6ea; border-radius:10px; overflow:hidden;
          box-shadow:0 1px 2px rgba(0,0,0,.04); }}
 .card header {{ display:flex; gap:8px; align-items:center; padding:10px 12px; flex-wrap:wrap; }}
 .card video {{ width:100%; display:block; background:#000; aspect-ratio:16/9; }}
 .card footer {{ display:flex; gap:12px; padding:8px 12px; font-size:12px; color:#5b636d; flex-wrap:wrap; }}
 .tag {{ font-size:11px; font-weight:700; padding:2px 6px; border-radius:4px; color:#fff; }}
 .tag.bad {{ background:#c0392b; }} .tag.warn {{ background:#d68910; }}
 .muted {{ color:#78818c; font-size:12px; }}
 .count {{ margin-left:auto; font-size:12px; color:#5b636d; }}
</style></head><body>
<header class="top">
  <h1>品質警告クリップ一覧</h1>
  <label>動画 <select id="label"><option value="">すべて</option>{options}</select></label>
  <label>手番 <select id="player"><option value="">両方</option>
    <option value="P1">P1のみ</option><option value="P2">P2のみ</option></select></label>
  <label>最小一致率 <input id="ratio" type="range" min="0.3" max="0.7" step="0.01" value="0.7"></label>
  <span id="count" class="count"></span>
</header>
<main id="grid">{body}</main>
<script>
 const grid=document.getElementById('grid');
 const cards=[...grid.querySelectorAll('.card')];
 const state={{label:'',player:'',ratio:0.7}};
 function apply(){{
   let shown=0;
   for(const card of cards){{
     const okLabel=!state.label||card.dataset.label===state.label;
     const okPlayer=!state.player||card.dataset.player===state.player;
     const okRatio=parseFloat(card.dataset.ratio)<=state.ratio;
     const visible=okLabel&&okPlayer&&okRatio;
     card.style.display=visible?'':'none';
     if(visible) shown++;
   }}
   document.getElementById('count').textContent=`${{shown}} / ${{cards.length}} 件`;
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
    args = parser.parse_args()
    sources = main_sources() + labsite_sources()
    if args.only == "main":
        sources = main_sources()
    elif args.only == "labsite":
        sources = labsite_sources()
    if not sources:
        print("no sources")
        return 1
    items = collect(sources)
    print(f"{len(items)} warning intervals from {len(sources)} videos")
    durations: dict[str, float] = {}
    made = 0
    for item in items:
        video = Path(item["video"])
        if str(video) not in durations:
            durations[str(video)] = probe_duration(video)
        slug = f"{Path(video).stem}-m{item['match']:02d}-p{item['player']}-{item['first_phase']}"
        slug = "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in slug)
        item["slug"] = slug
        start = max(0.0, item["start"] - PADDING_SECONDS)
        duration = item["end"] + PADDING_SECONDS - start
        limit = durations[str(video)]
        if limit > 0:
            duration = min(duration, limit - start)
        if cut(video, start, max(1.0, duration), CLIPS / f"{slug}.mp4"):
            made += 1
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "index.html").write_text(render(items, durations), encoding="utf-8")
    total = sum((CLIPS / f"{item['slug']}.mp4").stat().st_size
                for item in items if (CLIPS / f"{item['slug']}.mp4").exists())
    print(f"clips={made}/{len(items)} total={total/1e6:.1f}MB")
    print(f"page: {OUT / 'index.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
