"""旧解析と新解析でNEXTの認識が違った箇所を、画像つきの判定用HTMLにまとめる。

  python build_next_diff_review.py --old <batch-results.json> --new <batch-results.json>
                                   --video <mkv> --out <dir> [--max-items 400]

- 試合×プレイヤー×フェーズで NEXT 文字列を比較し、違う箇所だけを抽出
- その時刻のフレームから NEXT 5枠を切り出して HTML に埋め込む
- 判定（旧が正しい/新が正しい）は人間が行う前提。ボタンで記録して JSON 保存できる
"""
from __future__ import annotations

import argparse
import base64
import csv
import json
import subprocess
import tempfile
from pathlib import Path

from PIL import Image

REF_W, REF_H = 850.0, 478.0
REGION = {'1': (280, 55, 380, 265), '2': (700, 55, 800, 265)}
NAMES = {'I': 'I', 'O': 'O', 'T': 'T', 'L': 'L', 'J': 'J', 'S': 'S', 'Z': 'Z', '': 'なし'}


def load(path: Path):
    return json.loads(path.read_text(encoding='utf-8'))


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


def crop_png(frame_path: Path, player: str) -> bytes:
    frame = Image.open(frame_path).convert('RGB')
    scale = frame.width / 1920.0
    cw, ch = scale * 1920.0, scale * 1920.0 * 9 / 16
    rx0, ry0, rx1, ry1 = REGION[player]
    box = (round(rx0 * cw / REF_W), round(ry0 * ch / REF_H),
           round(rx1 * cw / REF_W), round(ry1 * ch / REF_H))
    crop = frame.crop(box)
    crop = crop.resize((crop.width * 3, crop.height * 3), Image.LANCZOS)
    import io
    buf = io.BytesIO()
    crop.save(buf, format='PNG')
    return buf.getvalue()


def rows_of(path: Path, player: str):
    d = load(path)
    rows = (d.get('labels') or {}).get(player) or []
    out = []
    for r in rows:
        try:
            out.append({'start': float(r[0]) / 1000.0, 'end': float(r[1]) / 1000.0, 'next': str(r[5] or '')})
        except Exception:
            continue
    return out


HTML = """<!doctype html><html lang="ja"><head><meta charset="utf-8"><title>NEXT認識の差分（旧 vs 新）</title>
<style>
body{font-family:system-ui,'Meiryo',sans-serif;background:#14161c;color:#e6e8ee;margin:0;padding-bottom:70px}
header{position:sticky;top:0;background:#1c1f27;padding:10px 14px;border-bottom:1px solid #303542;z-index:9}
h1{font-size:15px;margin:0 0 8px}
button{background:#2b3040;color:#e6e8ee;border:1px solid #3c4356;border-radius:6px;padding:4px 10px;cursor:pointer;font-size:12px}
button.on{background:#3b5bdb;border-color:#4c6ef5}
#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:10px;padding:12px}
.card{background:#1b1e26;border:2px solid #2b3040;border-radius:8px;padding:8px}
.card.old{border-color:#2f9e44}
.card.new{border-color:#e8590c}
.card.both{border-color:#868e96}
img{display:block;width:100%;border-radius:4px;background:#000}
.meta{font-size:12px;color:#9aa3b2;margin:6px 0;line-height:1.5}
.old-t{color:#69db7c;font-weight:700;font-size:15px;letter-spacing:2px}
.new-t{color:#ffa94d;font-weight:700;font-size:15px;letter-spacing:2px}
.diff{color:#ffd43b}
.picks{display:flex;gap:6px;margin-top:6px}
</style></head><body>
<header>
  <h1>NEXT認識の差分 — 旧解析 vs 新解析（__COUNT__ 件）</h1>
  <div style="font-size:12px;color:#9aa3b2">
    画像は各フェーズ中央のフレームから NEXT 5枠を切り出し。判定ボタンを押すと保存できます。
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
    card.className = 'card' + (verdict[i] === 'old' ? ' old' : verdict[i] === 'new' ? ' new' : verdict[i] === 'both' ? ' both' : '');
    const img = document.createElement('img');
    img.src = 'data:image/png;base64,' + it.image;
    const meta = document.createElement('div');
    meta.className = 'meta';
    meta.innerHTML = `試合 ${it.match} P${it.player} フェーズ ${it.phase}–${it.phase_end}（${it.run_len}件） t=${it.seconds.toFixed(2)}s<br>` +
      `<span class="old-t">旧 ${it.old}</span><br><span class="new-t">新 ${it.new}</span>`;
    const picks = document.createElement('div');
    picks.className = 'picks';
    [['old','旧が正しい'],['new','新が正しい'],['both','どちらも違う']].forEach(([k,label]) => {
      const b = document.createElement('button');
      b.textContent = label;
      if (verdict[i] === k) b.classList.add('on');
      b.onclick = () => { verdict[i] = k; render(); updateCount(); };
      picks.appendChild(b);
    });
    card.append(img, meta, picks);
    frag.appendChild(card);
  });
  grid.replaceChildren(frag);
}
function updateCount(){
  document.getElementById('n').textContent = Object.keys(verdict).length;
}
document.getElementById('save').onclick = () => {
  const out = {};
  Object.entries(verdict).forEach(([i, v]) => {
    const it = ITEMS[Number(i)];
    out[`${it.match}-${it.player}-${it.phase}`] = { verdict: v, old: it.old, new: it.new };
  });
  const blob = new Blob([JSON.stringify(out, null, 1)], {type:'application/json'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'next-verdicts.json';
  a.click();
};
render(); updateCount();
</script></body></html>
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--old', required=True)
    parser.add_argument('--new', required=True)
    parser.add_argument('--video', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--max-items', type=int, default=400)
    args = parser.parse_args()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    old_root = Path(args.old).parent
    new_root = Path(args.new).parent
    old_manifest = load(Path(args.old))
    new_manifest = load(Path(args.new))
    video = Path(args.video)
    new_by_index = {m['index']: m for m in new_manifest['matches']}

    diffs = []
    for idx in sorted(new_by_index):
        match = new_by_index[idx]
        start_base = float((match.get('source_range') or {}).get('start_seconds') or 0.0)
        folder = f'match_{idx:03d}'
        old_auto = old_root / folder / 'autosave' / 'latest_editing_autosave.v2.json'
        new_auto = new_root / folder / 'autosave' / 'latest_editing_autosave.v2.json'
        if not (old_auto.is_file() and new_auto.is_file()):
            continue
        for player in ('1', '2'):
            o = rows_of(old_auto, f'p{player}')
            n = rows_of(new_auto, f'p{player}')
            for k in range(min(len(o), len(n))):
                if o[k]['next'] == n[k]['next']:
                    continue
                diffs.append({'match': idx, 'player': player, 'phase': k,
                              'seconds': round(start_base + (n[k]['start'] + n[k]['end']) / 2.0, 2),
                              'old': o[k]['next'], 'new': n[k]['next']})
    print('差分件数', len(diffs), flush=True)
    grouped = {}
    for d in diffs:
        grouped.setdefault((d['match'], d['player']), []).append(d)
    runs = []
    for (m, p), ds in sorted(grouped.items()):
        ds.sort(key=lambda x: x['phase'])
        cur = [ds[0]]
        for d in ds[1:]:
            if d['phase'] == cur[-1]['phase'] + 1:
                cur.append(d)
            else:
                runs.append((m, p, cur)); cur = [d]
        runs.append((m, p, cur))
    run_items = []
    for m, p, cur in runs:
        head = cur[0]
        run_items.append({**head, 'phase_end': cur[-1]['phase'], 'run_len': len(cur)})
    print('連続区間', len(run_items), flush=True)
    diffs = run_items
    items = []
    with tempfile.TemporaryDirectory() as tmp:
        frame_png = Path(tmp) / 'f.png'
        for d in diffs[:args.max_items]:
            if not grab(video, d['seconds'], frame_png):
                continue
            try:
                png = crop_png(frame_png, d['player'])
            except Exception:
                continue
            items.append({**d, 'image': base64.b64encode(png).decode('ascii')})
    html = (HTML.replace('__ITEMS__', json.dumps(items, ensure_ascii=False))
                .replace('__COUNT__', str(len(diffs))))
    target = out_dir / 'next-diff-review.html'
    target.write_text(html, encoding='utf-8')
    (out_dir / 'next-diffs.json').write_text(json.dumps(diffs, ensure_ascii=False, indent=1), encoding='utf-8')
    print('wrote', target, len(items), '件の画像', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
