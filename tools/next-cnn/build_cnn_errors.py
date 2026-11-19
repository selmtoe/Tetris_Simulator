"""現行NEXT CNNが間違えた画像を一覧化する。

  python build_cnn_errors.py --images <u8> --metadata <tsv> --model <onnx> --out <dir>
                             [--split test|all] [--limit 800]

- 正解ラベルと推論が食い違うサンプルだけを集める
- 正解ラベル別に絞り込める
"""
from __future__ import annotations

import argparse
import base64
import io
import json
from pathlib import Path

import numpy as np
from PIL import Image

NAMES = ['-', 'I', 'O', 'T', 'L', 'J', 'S', 'Z']

HTML = """<!doctype html><html lang="ja"><head><meta charset="utf-8"><title>NEXT CNN 誤判定一覧</title>
<style>
body{font-family:system-ui,'Meiryo',sans-serif;background:#14161c;color:#e6e8ee;margin:0;padding-bottom:40px}
header{position:sticky;top:0;background:#1c1f27;padding:10px 14px;border-bottom:1px solid #303542;z-index:9}
h1{font-size:15px;margin:0 0 6px}
button{background:#2b3040;color:#e6e8ee;border:1px solid #3c4356;border-radius:6px;padding:3px 9px;cursor:pointer;font-size:12px;margin-right:4px}
button.on{background:#3b5bdb;border-color:#4c6ef5}
#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:6px;padding:12px}
.card{background:#1b1e26;border:1px solid #2b3040;border-radius:8px;padding:5px}
img{width:100%;image-rendering:pixelated;border-radius:4px;background:#000}
.meta{font-size:11px;color:#9aa3b2;margin-top:3px;line-height:1.4}
.true{color:#69db7c;font-weight:700}.bad{color:#ff8787;font-weight:700}
</style></head><body>
<header>
  <h1>NEXT CNN 誤判定一覧（__COUNT__ 件 / 全 __TOTAL__ サンプル）</h1>
  <div id="filters"></div>
</header>
<div id="grid"></div>
<script>
const ITEMS = __ITEMS__;
let filter = 'all';
function renderFilters(){
  const box = document.getElementById('filters');
  box.innerHTML = '';
  ['all','I','O','T','L','J','S','Z'].forEach(l => {
    const b = document.createElement('button');
    b.textContent = l === 'all' ? 'すべて' : '正解 ' + l;
    if (filter === l) b.classList.add('on');
    b.onclick = () => { filter = l; renderFilters(); render(); };
    box.appendChild(b);
  });
  const s = document.createElement('span');
  s.style.cssText = 'font-size:12px;color:#9aa3b2';
  s.textContent = '  ' + filtered().length + ' 件';
  box.appendChild(s);
}
function filtered(){ return filter === 'all' ? ITEMS : ITEMS.filter(i => i.label === filter); }
function render(){
  const grid = document.getElementById('grid');
  const frag = document.createDocumentFragment();
  filtered().forEach(it => {
    const card = document.createElement('div');
    card.className = 'card';
    const img = document.createElement('img');
    img.src = 'data:image/png;base64,' + it.image;
    const meta = document.createElement('div');
    meta.className = 'meta';
    meta.innerHTML = `<span class="true">正解 ${it.label}</span> / <span class="bad">推論 ${it.pred}</span> ${(it.confidence*100).toFixed(0)}%` +
      `<br>#${it.index} ${it.match}試合 ${it.player}`;
    card.append(img, meta);
    frag.appendChild(card);
  });
  grid.replaceChildren(frag);
}
renderFilters(); render();
</script></body></html>
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--images', required=True)
    ap.add_argument('--metadata', required=True)
    ap.add_argument('--model', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--split', default='all')
    ap.add_argument('--limit', type=int, default=800)
    args = ap.parse_args()
    import csv
    import onnxruntime as ort

    import base64 as b64

    data = np.fromfile(args.images, dtype=np.uint8).reshape(-1, 3, 48, 48)
    rows = list(csv.DictReader(open(args.metadata, encoding='utf-8'), delimiter='\t'))
    if args.split != 'all':
        rows = [r for r in rows if r['split'] == args.split]
    rows = [r for r in rows if int(float(r['label'])) > 0 and str(r.get('unclear', '0')) == '0']
    index = np.array([int(r['image_index']) for r in rows])
    labels = np.array([int(float(r['label'])) for r in rows])
    x = data[index].astype(np.float32) / 127.5 - 1.0
    session = ort.InferenceSession(args.model, providers=['CPUExecutionProvider'])
    name = session.get_inputs()[0].name
    preds, confs = [], []
    for start in range(0, len(x), 512):
        out = session.run(None, {name: x[start:start + 512]})[0]
        z = out - out.max(axis=1, keepdims=True)
        p = np.exp(z)
        p /= p.sum(axis=1, keepdims=True)
        preds.append(out.argmax(axis=1))
        confs.append(p.max(axis=1))
    pred = np.concatenate(preds)
    conf = np.concatenate(confs)
    wrong = np.flatnonzero(pred != labels)
    items = []
    for k in wrong[:args.limit]:
        row = rows[int(k)]
        img = Image.fromarray(data[int(row['image_index'])].transpose(1, 2, 0)).resize((96, 96), Image.NEAREST)
        buf = io.BytesIO()
        img.save(buf, format='PNG')
        items.append({
            'index': int(row['image_index']),
            'label': NAMES[labels[int(k)]],
            'pred': NAMES[int(pred[int(k)])],
            'confidence': round(float(conf[int(k)]), 3),
            'match': row['match'],
            'player': 'P1' if row['player'] in ('1', 'p1') else 'P2',
            'image': b64.b64encode(buf.getvalue()).decode('ascii'),
        })
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    html = (HTML.replace('__ITEMS__', json.dumps(items, ensure_ascii=False))
                .replace('__COUNT__', str(len(items)))
                .replace('__TOTAL__', str(len(rows))))
    target = out_dir / 'cnn-errors.html'
    target.write_text(html, encoding='utf-8')
    print('wrote', target, len(items), '/', len(rows), '誤判定', flush=True)
    print('誤判定率 %.2f%%' % (100.0 * len(wrong) / max(1, len(rows))), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
