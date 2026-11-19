"""Per-match forensic report for a quality warning.

For one match this writes an HTML page that shows, for every flagged interval:

* the phase table (time, action, piece, cleared lines, garbage, board ratio)
* the observed grid and the reconstructed grid side by side, cell by cell,
  with mismatching cells outlined
* a per-phase breakdown of the mismatch into
      obsG  = the image shows garbage, the replay does not
      recG  = the replay has garbage the image does not have
      other = colour/empty disagreement
* optionally a few video frames of the interval

Usage:
    python tools/forensics_match.py --run frozen/runs/goexit-360p30-stride1-8warn \
        --data "tools/video-recovery-runtime/data/batches/[yD_ZukVrgHY]" \
        --match 14 --out review/forensics/match-014 --video "<mkv>" --frames 3
    python tools/forensics_match.py --run ... --data ... --all --out review/forensics
"""
from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FFMPEG = ROOT / "tools" / "video-recovery-runtime" / "bin" / "ffmpeg.exe"

COLORS = {
    "I": "#38bdf8", "O": "#fbbf24", "T": "#a855f7", "L": "#fb923c",
    "J": "#3b82f6", "S": "#22c55e", "Z": "#ef4444", "G": "#cbd5e1",
    "_": "#111827",
}


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def ratio(step: dict) -> float:
    observed, board = step["observed"], step["board"]
    matched = 0
    for row in range(20, 40):
        for col in range(10):
            if observed[row][col] == board[row][col]:
                matched += 1
    return matched / 200.0


def decompose(step: dict) -> tuple[int, int, int]:
    observed, board = step["observed"], step["board"]
    obs_garbage = rec_garbage = other = 0
    for row in range(20, 40):
        for col in range(10):
            if observed[row][col] == board[row][col]:
                continue
            if observed[row][col] == "G":
                obs_garbage += 1
            elif board[row][col] == "G":
                rec_garbage += 1
            else:
                other += 1
    return obs_garbage, rec_garbage, other


def grid_html(cells: list[str], other: list[str] | None) -> str:
    rows = []
    for row in range(20, 40):
        cells_html = []
        for col in range(10):
            value = cells[row][col] if col < len(cells[row]) else "_"
            colour = COLORS.get(value, "#94a3b8")
            text_color = "#0b1220" if value in "IOTLJSZG" else "#475569"
            outline = ""
            if other is not None and other[row][col] != value:
                outline = "outline:2px solid #f43f5e;outline-offset:-2px;"
            cells_html.append(f'<i style="background:{colour};color:{text_color};{outline}"></i>')
        rows.append("".join(cells_html))
    return '<div class="grid">' + "".join(f'<div class="grow">{r}</div>' for r in rows) + "</div>"


def frame(video: Path, seconds: float, target: Path) -> bool:
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        str(FFMPEG), "-hide_banner", "-loglevel", "error",
        "-ss", f"{seconds:.3f}", "-i", str(video), "-frames:v", "1",
        "-vf", "scale=640:-2", "-y", str(target)], capture_output=True, text=True)
    return target.exists()


def build(run: Path, match: int, out: Path, video: Path | None, frames: int,
          data_root: Path | None = None) -> Path:
    warnings = load(run / "quality-warnings.json")
    results = load(run / "batch-results.json")
    entry = next((m for m in warnings["matches"] if int(m["index"]) == match), None)
    if entry is None:
        raise SystemExit(f"match {match} not found in {run}")
    span = next((m.get("source_range") for m in results["matches"]
                 if int(m["index"]) == match), None)
    issues = (entry.get("quality_warning") or {}).get("issues") or []
    simulator = (data_root or run) / f"match_{match:03d}" / "simulator"
    timeline_file = next((f for f in simulator.iterdir()
                          if f.name.endswith("_tetris_recovered.json")), None)
    if timeline_file is None:
        raise SystemExit(f"no recovered timeline under {simulator}")
    data = load(timeline_file)
    out.mkdir(parents=True, exist_ok=True)

    sections = []
    for index, issue in enumerate(issues):
        player = "p1" if int(issue["player"]) == 1 else "p2"
        steps = data[player]
        first = max(0, int(issue["first_phase"]) - 2)
        last = min(len(steps) - 1, int(issue["last_phase"]) + 2)
        rows = []
        details = []
        for phase in range(first, last + 1):
            step = steps[phase]
            if step.get("action") == "end":
                continue
            value = ratio(step)
            obs_g, rec_g, other_g = decompose(step)
            placement = step.get("placement") or {}
            flagged = int(issue["first_phase"]) <= phase <= int(issue["last_phase"])
            rows.append(
                f'<tr class="{"flagged" if flagged else ""}">'
                f'<td>{phase}</td><td>{step["time"]:.2f}</td>'
                f'<td>{step.get("action","")}</td>'
                f'<td>{placement.get("piece","") or "-"}@'
                f'{placement.get("x",0)},{placement.get("y",0)} r{placement.get("rotation",0)}</td>'
                f'<td>{placement.get("clearedLines",0)}</td>'
                f'<td>{step.get("garbage",{}).get("lines",0)}</td>'
                f'<td>{value:.3f}</td>'
                f'<td>{obs_g}</td><td>{rec_g}</td><td>{other_g}</td></tr>')
            details.append((phase, step, value))
        worst = min(details, key=lambda item: item[2]) if details else None
        grids = ""
        if worst is not None:
            grids = f"""
      <div class="grids">
        <figure><figcaption>観測 (phase {worst[0]})</figcaption>{grid_html(worst[1]["observed"], worst[1]["board"])}</figure>
        <figure><figcaption>復元 (phase {worst[0]})</figcaption>{grid_html(worst[1]["board"], worst[1]["observed"])}</figure>
      </div>"""
        frames_html = ""
        if video and frames > 0:
            frames_html = '<div class="frames">'
            start = float(issue["source_start_seconds"])
            end = float(issue["source_end_seconds"])
            for k in range(frames):
                at = start + (end - start) * (k / max(1, frames - 1))
                name = f"m{match:03d}-p{issue['player']}-i{index}-{k}.png"
                if frame(video, at, out / "frames" / name):
                    frames_html += (f'<figure><img src="frames/{name}">'
                                    f'<figcaption>{at:.2f}s</figcaption></figure>')
            frames_html += "</div>"
        sections.append(f"""
    <section class="issue">
      <h2>P{issue["player"]} / phase {issue["first_phase"]}–{issue["last_phase"]}
        （{issue["consecutive_phases"]}フェーズ, 最低一致率 {issue["minimum_match_ratio"]:.3f}）</h2>
      <p class="meta">元動画 {issue["source_start_seconds"]:.2f}–{issue["source_end_seconds"]:.2f}s
        ／ 対象プレイヤー {player}</p>
      <table>
        <thead><tr><th>phase</th><th>time</th><th>action</th><th>配置</th><th>消去</th>
        <th>garbage</th><th>一致率</th><th>obsG</th><th>recG</th><th>other</th></tr></thead>
        <tbody>{''.join(rows)}</tbody>
      </table>
      {grids}
      {frames_html}
    </section>""")

    title = f'match {match:03d}'
    body = "".join(sections) or "<p>この試合に警告区間はありません。</p>"
    source = (f'<p class="meta">元動画: {span["start_seconds"]:.2f}–{span["end_seconds"]:.2f}s</p>'
              if span else "")
    page = f"""<!doctype html><html lang="ja"><head><meta charset="utf-8">
<title>{title} 品質警告の詳細</title>
<style>
 body {{ margin:0; padding:20px; background:#0f172a; color:#e2e8f0;
        font-family:-apple-system,"Segoe UI","Noto Sans JP",sans-serif; }}
 h1 {{ font-size:18px; margin:0 0 6px; }}
 h2 {{ font-size:15px; margin:24px 0 6px; color:#f8fafc; }}
 .meta {{ color:#94a3b8; font-size:12px; margin:0 0 12px; }}
 table {{ border-collapse:collapse; font-size:12px; margin:8px 0 16px; }}
 th,td {{ border:1px solid #1e293b; padding:3px 8px; text-align:right; }}
 th {{ background:#111c33; color:#cbd5e1; }}
 tr.flagged td {{ background:#3b0d19; }}
 .grids,.frames {{ display:flex; gap:20px; flex-wrap:wrap; margin-bottom:16px; }}
 figure {{ margin:0; }}
 figcaption {{ font-size:11px; color:#94a3b8; margin-bottom:4px; }}
 .grid {{ display:flex; flex-direction:column; gap:2px; }}
 .grow {{ display:flex; gap:2px; }}
 .grid i {{ width:18px; height:18px; display:block; border-radius:2px; }}
 .frames img {{ width:320px; border:1px solid #1e293b; border-radius:6px; }}
 .issue {{ border-top:1px solid #1e293b; padding-top:8px; }}
</style></head><body>
<h1>{title} の品質警告</h1>
{source}
<p class="meta">観測と復元の不一致セルは赤枠。obsG=画像がゴミ / recG=復元だけゴミ / other=色・空の不一致。</p>
{body}
</body></html>"""
    target = out / f"match-{match:03d}.html"
    target.write_text(page, encoding="utf-8")
    return target


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--data", type=Path, help="directory holding match_XXX (defaults to --run)")
    parser.add_argument("--match", type=int)
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--video", type=Path)
    parser.add_argument("--frames", type=int, default=0)
    args = parser.parse_args()

    run = args.run
    args.out.mkdir(parents=True, exist_ok=True)
    if args.all:
        warnings = load(run / "quality-warnings.json")
        targets = [int(m["index"]) for m in warnings["matches"]
                   if (m.get("quality_warning") or {}).get("suspicious")]
        index_rows = []
        for match in targets:
            page = build(run, match, args.out, args.video, args.frames, args.data)
            index_rows.append(f'<li><a href="{page.name}">試合 {match:03d}</a></li>')
            print(f"wrote {page}")
        index = args.out / "index.html"
        index.write_text(
            '<!doctype html><html lang="ja"><head><meta charset="utf-8">'
            '<title>品質警告の詳細</title><style>body{background:#0f172a;color:#e2e8f0;'
            'font-family:sans-serif;padding:20px} a{color:#7dd3fc}</style></head><body>'
            '<h1>品質警告が出た試合</h1><ul>' + "".join(index_rows) + "</ul></body></html>",
            encoding="utf-8")
        print(f"index: {index}")
    else:
        if args.match is None:
            raise SystemExit("--match or --all is required")
        print(f"wrote {build(run, args.match, args.out, args.video, args.frames, args.data)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
