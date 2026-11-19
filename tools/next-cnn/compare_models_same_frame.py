"""同じ画像に対して旧NEXTモデルと新NEXTモデルの読みを直接比較する。

  python compare_models_same_frame.py --diffs <next-diffs.json> --old-run <dir> --new-run <dir>
                                      --video <mkv> --out <dir> [--per-run 3]

- next-diffs.json（旧と新で読みが違った連続区間）を入力にする
- 旧解析のフェーズ時刻でフレームを切り出し、NEXT 5枠を両モデルに通す
- フェーズ分割の影響を受けずに「認識精度」だけを比べられる
"""
from __future__ import annotations

import argparse
import base64
import io
import json
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

REF_W, REF_H = 850.0, 478.0
SLOT = {
    '1': {'x': 309, 'y': [72, 105, 138, 171, 204], 'w': 46, 'h': 27},
    '2': {'x': 727, 'y': [72, 105, 138, 171, 204], 'w': 46, 'h': 27},
}
NAMES = ['-', 'I', 'O', 'T', 'L', 'J', 'S', 'Z']


def ffmpeg_path() -> str:
    candidate = Path(__file__).resolve().parents[1] / 'video-recovery-runtime' / 'bin' / 'ffmpeg.exe'
    return str(candidate) if candidate.is_file() else 'ffmpeg'


def grab(video: Path, seconds: float, out: Path) -> bool:
    cmd = [ffmpeg_path(), '-hide_banner', '-loglevel', 'error', '-ss', f'{seconds:.3f}',
           '-i', str(video), '-frames:v', '1', '-y', str(out)]
    try:
        subprocess.run(cmd, check=True, timeout=120)
    except Exception:
        return False
    return out.is_file()


def frame_to_ref(frame: Image.Image):
    scale = frame.width / 1920.0
    cw, ch = scale * 1920.0, scale * 1920.0 * 9 / 16
    return cw / REF_W, ch / REF_H


def slot_crops(frame: Image.Image, player: str):
    fx, fy = frame_to_ref(frame)
    spec = SLOT[player]
    crops = []
    for y in spec['y']:
        box = (round(spec['x'] * fx), round(y * fy),
               round((spec['x'] + spec['w']) * fx), round((y + spec['h']) * fy))
        crops.append(frame.crop(box).resize((48, 48), Image.BILINEAR))
    return crops


def to_input(crops):
    arr = np.stack([np.asarray(c, dtype=np.float32).transpose(2, 0, 1) for c in crops])
    return arr / 127.5 - 1.0


def read_model(session, crops):
    import onnxruntime as ort  # noqa: F401
    name = session.get_inputs()[0].name
    out = session.run(None, {name: to_input(crops)})[0]
    return ''.join(NAMES[int(i)] for i in out.argmax(axis=1))


def rows_of(path: Path, player: str):
    d = json.loads(path.read_text(encoding='utf-8'))
    rows = (d.get('labels') or {}).get(player) or []
    out = []
    for r in rows:
        try:
            out.append({'start': float(r[0]) / 1000.0, 'end': float(r[1]) / 1000.0,
                        'piece': r[2], 'next': str(r[5] or '')})
        except Exception:
            continue
    return out


HTML = """<!doctype html><html lang="ja"><head><meta charset="utf-8"><title>同一画像で旧モデル vs 新モデル</title>
<style>
body{font-family:system-ui,'Meiryo',sans-serif;background:#14161c;color:#e6e8ee;margin:0;padding:0 0 80px}
header{position:sticky;top:0;background:#1c1f27;padding:10px 14px;border-bottom:1px solid #303542;z-index:9}
h1{font-size:15px;margin:0 0 6px}
button{background:#2b3040;color:#e6e8ee;border:1px solid #3c4356;border-radius:6px;padding:4px 9px;cursor:pointer;font-size:12px}
button.on{background:#3b5bdb;border-color:#4c6ef5}
#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(320px,1fr));gap:10px;padding:12px}
.card{background:#1b1e26;border:2px solid #2b3040;border-radius:8px;padding:8px}
.card.old{border-color:#2f9e44}.card.new{border-color:#e8590c}.card.both{border-color:#868e96}
img{display:block;width:100%;border-radius:4px;background:#000}
.meta{font-size:12px;color:#9aa3b2;margin:6px 0;line-height:1.6}
.o{color:#69db7c;font-weight:700;letter-spacing:2px}
.n{color:#ffa94d;font-weight:700;letter-spacing:2px}
.picks{display:flex;gap:6px;margin-top:6px}
</style></head><body>
<header>
  <h1>同じ画像で 旧モデル vs 新モデル（__COUNT__ 件）</h1>
  <div style="font-size:12px;color:#9aa3b2">
    旧解析のフェーズ時刻で切り出した画像に、両モデルを直接適用した結果です（タイミングの影響なし）。
    <button id="save" style="margin-left:10px">判定をJSONで保存（<span id="n">0</span>件）</button>
  </div>
</header>
<div id="grid"></div>
<script>
const ITEMS = __ITEMS__;
const verdict = {};
function render(){
  const grid = document.getElementById('grid');
  const frag = document.createDocumentFragment();
  ITEMS.forEach((it, i) => {
    const card = document.createElement('div');
    card.className = 'card' + (verdict[i]==='old'?' old':verdict[i]==='new'?' new':verdict[i]==='both'?' both':'');
    const img = document.createElement('img');
    img.src = 'data:image/png;base64,' + it.image;
    const meta = document.createElement('div');
    meta.className = 'meta';
    meta.innerHTML = `試合 ${it.match} P${it.player} 旧解析フェーズ ${it.phase}（${it.phase_end}まで） t=${it.seconds.toFixed(2)}s<br>` +
      `画像のNEXT（人間用）: 上から ${it.human || '－'}<br>` +
      `<span class="o">旧モデル ${it.old_model}</span><br>` +
      `<span class="n">新モデル ${it.new_model}</span><br>` +
      `解析の読み: 旧 ${it.old_text} / 新 ${it.new_text}`;
    const picks = document.createElement('div');
    picks.className = 'picks';
    [['old','旧モデルが正しい'],['new','新モデルが正しい'],['both','どちらも違う']].forEach(([k,label]) => {
      const b = document.createElement('button');
      b.textContent = label;
      if (verdict[i]===k) b.classList.add('on');
      b.onclick = () => { verdict[i]=k; render(); document.getElementById('n').textContent = Object.keys(verdict).length; };
      picks.appendChild(b);
    });
    card.append(img, meta, picks);
    frag.appendChild(card);
  });
  grid.replaceChildren(frag);
}
document.getElementById('save').onclick = () => {
  const out = {};
  Object.entries(verdict).forEach(([i,v]) => {
    const it = ITEMS[Number(i)];
    out[`${it.match}-${it.player}-${it.phase}`] = {verdict:v, old_model:it.old_model, new_model:it.new_model};
  });
  const blob = new Blob([JSON.stringify(out,null,1)],{type:'application/json'});
  const a = document.createElement('a'); a.href = URL.createObjectURL(blob);
  a.download = 'model-verdicts.json'; a.click();
};
render();
</script></body></html>
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--diffs', required=True)
    parser.add_argument('--old-run', required=True)
    parser.add_argument('--new-run', required=True)
    parser.add_argument('--video', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--old-model', default=r'tools\video-recovery-runtime\models\next_hold.before-20261107.onnx')
    parser.add_argument('--new-model', default=r'scratch\next360\final-v3\next_hold.candidate.onnx')
    parser.add_argument('--per-run', type=int, default=3)
    args = parser.parse_args()
    import onnxruntime as ort
    old_sess = ort.InferenceSession(args.old_model, providers=['CPUExecutionProvider'])
    new_sess = ort.InferenceSession(args.new_model, providers=['CPUExecutionProvider'])
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    diffs = json.loads(Path(args.diffs).read_text(encoding='utf-8'))
    video = Path(args.video)
    old_root = Path(args.old_run)
    new_root = Path(args.new_run)
    old_manifest = json.loads((old_root / 'batch-results.json').read_text(encoding='utf-8'))
    start_base = {m['index']: float((m.get('source_range') or {}).get('start_seconds') or 0.0)
                  for m in old_manifest['matches']}

    items = []
    with tempfile.TemporaryDirectory() as tmp:
        frame_png = Path(tmp) / 'f.png'
        for d in diffs:
            match, player = d['match'], d['player']
            folder = f'match_{match:03d}'
            old_auto = old_root / folder / 'autosave' / 'latest_editing_autosave.v2.json'
            new_auto = new_root / folder / 'autosave' / 'latest_editing_autosave.v2.json'
            if not (old_auto.is_file() and new_auto.is_file()):
                continue
            o = rows_of(old_auto, f'p{player}')
            n = rows_of(new_auto, f'p{player}')
            phase_end = d.get('phase_end', d['phase'])
            picks = [d['phase']]
            for k in range(d['phase'] + 1, min(phase_end + 1, len(o))):
                if len(picks) >= args.per_run:
                    break
                picks.append(k)
            for k in picks:
                if k >= len(o) or k >= len(n):
                    continue
                seconds = start_base.get(match, 0.0) + (o[k]['start'] + o[k]['end']) / 2.0
                if not grab(video, seconds, frame_png):
                    continue
                frame = Image.open(frame_png).convert('RGB')
                crops = slot_crops(frame, player)
                old_read = read_model(old_sess, crops)
                new_read = read_model(new_sess, crops)
                strip = frame.crop((round(SLOT[player]['x'] * frame_to_ref(frame)[0]) - 6,
                                    round(50 * frame_to_ref(frame)[1]),
                                    round((SLOT[player]['x'] + SLOT[player]['w']) * frame_to_ref(frame)[0]) + 6,
                                    round(240 * frame_to_ref(frame)[1])))
                strip = strip.resize((strip.width * 3, strip.height * 3), Image.NEAREST)
                buf = io.BytesIO()
                strip.save(buf, format='PNG')
                items.append({
                    'match': match, 'player': player, 'phase': k, 'phase_end': phase_end,
                    'seconds': round(seconds, 2),
                    'old_model': old_read, 'new_model': new_read,
                    'old_text': o[k]['next'], 'new_text': n[k]['next'],
                    'image': base64.b64encode(buf.getvalue()).decode('ascii'),
                })
    html = HTML.replace('__ITEMS__', json.dumps(items, ensure_ascii=False)).replace('__COUNT__', str(len(items)))
    target = out_dir / 'same-frame-comparison.html'
    target.write_text(html, encoding='utf-8')
    (out_dir / 'same-frame-items.json').write_text(json.dumps(items, ensure_ascii=False, indent=1), encoding='utf-8')
    print('wrote', target, len(items), '件', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
