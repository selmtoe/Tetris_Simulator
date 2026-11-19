"""品質警告試合のNEXTを「1枠ずつ」分解して確認・修正できるHTMLを作る。

  python build_slot_review.py --run <batch-dir> --video <mkv> --out <dir> [--model <onnx>]

- 警告が出た試合について、不一致開始の前後と PLACE SKIP 周辺のフェーズを対象に
  NEXT 5枠を1枠ずつ切り出す
- 各枠に「記録された値」と「現行モデルの推論」を並べ、正しいラベルを選べる
- 修正はブラウザ内に保持し、JSONで保存 → merge_slot_corrections.py で学習データに追加
"""
from __future__ import annotations

import argparse
import base64
import io
import json
import subprocess
import tempfile
from pathlib import Path

from PIL import Image

REF_W, REF_H = 850.0, 478.0
SLOTS = {
    'p1': {'x': 309, 'y': [72, 105, 138, 171, 204], 'w': 46, 'h': 27},
    'p2': {'x': 727, 'y': [72, 105, 138, 171, 204], 'w': 46, 'h': 27},
}
NAMES = ['-', 'I', 'O', 'T', 'L', 'J', 'S', 'Z']


def ffmpeg_path() -> str:
    p = Path(__file__).resolve().parents[1] / 'video-recovery-runtime' / 'bin' / 'ffmpeg.exe'
    return str(p) if p.is_file() else 'ffmpeg'


def load(path: Path):
    return json.loads(path.read_text(encoding='utf-8'))


def frame_scale(frame: Image.Image):
    scale = frame.width / 1920.0
    cw = scale * 1920.0
    return cw / REF_W, (cw * 9 / 16) / REF_H


def slot_box(frame: Image.Image, player: str, slot: int):
    fx, fy = frame_scale(frame)
    spec = SLOTS[player]
    y = spec['y'][slot]
    return (round(spec['x'] * fx), round(y * fy),
            round((spec['x'] + spec['w']) * fx), round((y + spec['h']) * fy))


def grab(video: Path, seconds: float, out: Path) -> bool:
    cmd = [ffmpeg_path(), '-hide_banner', '-loglevel', 'error', '-ss', f'{seconds:.3f}',
           '-i', str(video), '-frames:v', '1', '-y', str(out)]
    try:
        subprocess.run(cmd, check=True, timeout=120)
    except Exception:
        return False
    return out.is_file()


HTML = """<!doctype html><html lang="ja"><head><meta charset="utf-8"><title>NEXT 1枠ずつ確認</title>
<style>
body{font-family:system-ui,'Meiryo',sans-serif;background:#14161c;color:#e6e8ee;margin:0;padding-bottom:70px}
header{position:sticky;top:0;background:#1c1f27;padding:10px 14px;border-bottom:1px solid #303542;z-index:9}
h1{font-size:15px;margin:0 0 6px}
button{background:#2b3040;color:#e6e8ee;border:1px solid #3c4356;border-radius:6px;padding:3px 8px;cursor:pointer;font-size:12px}
button.on{background:#3b5bdb;border-color:#4c6ef5}
#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:8px;padding:12px}
.card{background:#1b1e26;border:2px solid #2b3040;border-radius:8px;padding:6px}
.card.bad{border-color:#c92a2a}.card.fixed{border-color:#2f9e44}
img{width:100%;image-rendering:pixelated;border-radius:4px;background:#000}
.meta{font-size:11px;color:#9aa3b2;margin:4px 0;line-height:1.4}
.rec{color:#ffd43b;font-weight:700}.pred{color:#69db7c;font-weight:700}
.picks{display:flex;flex-wrap:wrap;gap:3px}
.picks button{padding:1px 5px;font-size:11px}
#bar{position:fixed;bottom:0;left:0;right:0;background:#1c1f27;border-top:1px solid #303542;padding:8px 12px;font-size:12px;display:flex;gap:10px;align-items:center}
</style></head><body>
<header>
  <h1>NEXT 1枠ずつ確認 — 試合 __MATCH__（__COUNT__枠）</h1>
  <div style="font-size:12px;color:#9aa3b2">記録＝解析が出した値 / 推論＝現行CNN。違っていれば正しいラベルを押してください。</div>
</header>
<div id="grid"></div>
<div id="bar">
  <span id="n">修正 0 件</span>
  <button id="save">修正JSONを保存</button>
  <span style="color:#9aa3b2">保存したJSONを私に渡してください（学習データに追加します）</span>
</div>
<script>
const ITEMS = __ITEMS__;
const fixes = {};
function render(){
  const grid = document.getElementById('grid');
  const frag = document.createDocumentFragment();
  ITEMS.forEach((it, i) => {
    const card = document.createElement('div');
    const current = (i in fixes) ? fixes[i] : it.recorded;
    card.className = 'card' + (current !== it.predicted ? ' bad' : '') + ((i in fixes) ? ' fixed' : '');
    const img = document.createElement('img');
    img.src = 'data:image/png;base64,' + it.image;
    const meta = document.createElement('div');
    meta.className = 'meta';
    meta.innerHTML = `#${it.phase} 枠${it.slot} t=${it.seconds.toFixed(2)}s<br>` +
      `記録 <span class="rec">${it.recorded}</span> / 推論 <span class="pred">${it.predicted}</span>`;
    const picks = document.createElement('div');
    picks.className = 'picks';
    ['-','I','O','T','L','J','S','Z'].forEach(c => {
      const b = document.createElement('button');
      b.textContent = c === '-' ? 'なし' : c;
      if (c === current) b.classList.add('on');
      b.onclick = () => { fixes[i] = c; render(); document.getElementById('n').textContent = `修正 ${Object.keys(fixes).length} 件`; };
      picks.appendChild(b);
    });
    card.append(img, meta, picks);
    frag.appendChild(card);
  });
  grid.replaceChildren(frag);
}
document.getElementById('save').onclick = () => {
  const out = {};
  Object.entries(fixes).forEach(([i, label]) => {
    const it = ITEMS[Number(i)];
    out[`${it.match}-${it.player}-${it.phase}-${it.slot}`] = {label, recorded: it.recorded, predicted: it.predicted, file: it.file};
  });
  const blob = new Blob([JSON.stringify(out, null, 1)], {type:'application/json'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'slot-corrections-__MATCH__.json';
  a.click();
};
render();
</script></body></html>
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', required=True)
    ap.add_argument('--video', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--model', default=r'tools\video-recovery-runtime\models\next_hold.onnx')
    ap.add_argument('--before', type=int, default=20)
    ap.add_argument('--after', type=int, default=10)
    args = ap.parse_args()
    import numpy as np
    import onnxruntime as ort

    run = Path(args.run)
    out_dir = Path(args.out)
    crops_dir = out_dir / 'crops'
    out_dir.mkdir(parents=True, exist_ok=True)
    crops_dir.mkdir(parents=True, exist_ok=True)
    manifest = load(run / 'batch-results.json')
    session = ort.InferenceSession(args.model, providers=['CPUExecutionProvider'])
    in_name = session.get_inputs()[0].name
    video = Path(args.video)
    index_lines = []

    with tempfile.TemporaryDirectory() as tmp:
        frame_png = Path(tmp) / 'f.png'
        for match in manifest['matches']:
            idx = match['index']
            q = match.get('quality_warning') or {}
            if not q.get('suspicious'):
                continue
            start_base = float((match.get('source_range') or {}).get('start_seconds') or 0.0)
            auto = run / f'match_{idx:03d}' / 'autosave' / 'latest_editing_autosave.v2.json'
            if not auto.is_file():
                continue
            LAB = load(auto).get('labels') or {}
            issues = q.get('issues') or []
            items = []
            for player in ('p1', 'p2'):
                rows = LAB.get(player) or []
                if not rows:
                    continue
                pl_issues = [i for i in issues if str(i.get('player')) == player[-1]]
                onset = min((int(i['first_phase']) for i in pl_issues), default=None)
                selected = set()
                if onset is not None:
                    selected.update(range(max(0, onset - args.before),
                                          min(len(rows), onset + args.after + 1)))
                for k, r in enumerate(rows):
                    if str(r[3]) == 'place_skipped':
                        selected.update(range(max(0, k - 2), min(len(rows), k + 3)))
                for k in sorted(selected):
                    r = rows[k]
                    seconds = start_base + (float(r[0]) + float(r[1])) / 2000.0
                    if not grab(video, seconds, frame_png):
                        continue
                    frame = Image.open(frame_png).convert('RGB')
                    next_str = str(r[5] or '')
                    for slot in range(5):
                        box = slot_box(frame, player, slot)
                        crop = frame.crop(box).resize((48, 48), Image.BILINEAR)
                        arr = np.asarray(crop, dtype=np.float32).transpose(2, 0, 1) / 127.5 - 1.0
                        out = session.run(None, {in_name: arr[None, ...]})[0]
                        predicted = NAMES[int(out.argmax(axis=1)[0])]
                        recorded = next_str[slot] if slot < len(next_str) else '-'
                        name = f'{idx:03d}-{player}-{k:03d}-{slot+1}.png'
                        crop.resize((144, 144), Image.NEAREST).save(crops_dir / name)
                        buf = io.BytesIO()
                        crop.resize((144, 144), Image.NEAREST).save(buf, format='PNG')
                        items.append({
                            'match': idx, 'player': player.upper(), 'phase': k, 'slot': slot + 1,
                            'seconds': round(seconds, 2), 'recorded': recorded,
                            'predicted': predicted, 'file': f'crops/{name}',
                            'image': base64.b64encode(buf.getvalue()).decode('ascii'),
                        })
            if not items:
                continue
            html = HTML.replace('__ITEMS__', json.dumps(items, ensure_ascii=False)).replace(
                '__MATCH__', str(idx)).replace('__COUNT__', str(len(items)))
            target = out_dir / f'slot-review-match{idx:03d}.html'
            target.write_text(html, encoding='utf-8')
            print('wrote', target, len(items), '枠', flush=True)
            index_lines.append({'match': idx, 'file': target.name, 'slots': len(items)})

    (out_dir / 'index.json').write_text(json.dumps(index_lines, ensure_ascii=False, indent=1), encoding='utf-8')
    index_html = ['<!doctype html><html lang="ja"><meta charset="utf-8"><body style="font-family:system-ui;background:#14161c;color:#e6e8ee;padding:16px">',
                  '<h1 style="font-size:16px">品質警告試合のNEXT 1枠ずつ確認</h1><ul>']
    for row in index_lines:
        index_html.append(f'<li><a style="color:#8ce99a" href="{row["file"]}">試合 {row["match"]}</a> — {row["slots"]} 枠</li>')
    index_html.append('</ul></body></html>')
    (out_dir / 'index.html').write_text('\n'.join(index_html), encoding='utf-8')
    print('wrote', out_dir / 'index.html')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
