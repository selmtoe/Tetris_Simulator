"""NEXT CNN 学習（シンプル版・構成比較用）。

  python train_simple.py --images <u8> --metadata <tsv> --out <dir>
                         --arch dual|spatial|base|resnet --aug 0|1|2 --epochs 20

- 損失は 交差エントロピー（重みなし）＋ unclear の BCE
- 大きめのバッチで安定して学習
- 出力: next_hold.candidate.onnx（image[1,3,48,48] / piece_logits[1,8] / unclear_logit[1]）
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

import numpy as np
import torch
from torch import nn

sys.path.insert(0, str(Path(__file__).resolve().parent))
from loop_next2 import Net, augment  # noqa: E402

SIZE = 48


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--images', required=True)
    parser.add_argument('--metadata', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--arch', default='dual', choices=('base', 'spatial', 'dual', 'resnet'))
    parser.add_argument('--width', type=int, default=24)
    parser.add_argument('--aug', type=int, default=2)
    parser.add_argument('--epochs', type=int, default=20)
    parser.add_argument('--batch-size', type=int, default=512)
    parser.add_argument('--lr', type=float, default=2e-3)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--unclear-weight', type=float, default=0.3,
                        help='unclearヘッドの損失重み（0で常にクリア扱い）')
    args = parser.parse_args()
    torch.manual_seed(args.seed)
    np.random.seed(args.seed)
    device = 'cuda' if torch.cuda.is_available() else 'cpu'
    gen = torch.Generator(device=device).manual_seed(args.seed)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    rows = list(csv.DictReader(Path(args.metadata).open(encoding='utf-8'), delimiter='\t'))
    data = np.fromfile(args.images, dtype=np.uint8).reshape(-1, 3, SIZE, SIZE)
    index = np.array([int(r['image_index']) for r in rows])
    labels = np.array([max(0, int(float(r['label']))) for r in rows], dtype=np.int64)
    unclear = np.array([1.0 if int(float(r['label'])) <= 0 else 0.0 for r in rows], dtype=np.float32)
    splits = np.array([r['split'] for r in rows])

    def take(mask):
        idx = index[mask]
        x = data[idx].astype(np.float32) / 127.5 - 1.0
        return torch.from_numpy(x), torch.from_numpy(labels[mask]), torch.from_numpy(unclear[mask])

    xtr, ytr, utr = take(splits == 'train')
    xva, yva, uva = take(splits == 'val')
    xte, yte, ute = take(splits == 'test')
    print(f'arch={args.arch} aug={args.aug} width={args.width} train={len(xtr)} val={len(xva)} test={len(xte)} device={device}', flush=True)

    model = Net(arch=args.arch, width=args.width).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.epochs)
    best, best_state = 0.0, None
    for epoch in range(args.epochs):
        model.train()
        order = torch.randperm(len(xtr))
        total = 0.0
        for start in range(0, len(order), args.batch_size):
            sel = order[start:start + args.batch_size]
            xb, yb, ub = xtr[sel].to(device), ytr[sel].to(device), utr[sel].to(device)
            if args.aug:
                xb = augment(xb, args.aug, gen)
            opt.zero_grad()
            logits, unclear_logit = model(xb)
            clear = ub == 0
            loss = (nn.functional.cross_entropy(logits[clear], yb[clear]) if clear.any()
                    else torch.zeros((), device=device))
            if args.unclear_weight > 0:
                loss = loss + args.unclear_weight * nn.functional.binary_cross_entropy_with_logits(
                    unclear_logit, ub)
            else:
                # unclear は常に「クリア」に固定（エンジン側の判定に任せる）
                loss = loss + 0.0 * unclear_logit.sum()
            loss.backward()
            opt.step()
            total += float(loss)
        sched.step()
        model.eval()
        with torch.no_grad():
            pred = model(xva.to(device))[0].argmax(1).cpu()
            clear = uva == 0
            acc_va = (pred[clear] == yva[clear]).float().mean().item() if clear.any() else 0.0
            tr_pred = model(xtr[:4000].to(device))[0].argmax(1).cpu()
            tr_clear = utr[:4000] == 0
            acc_tr = (tr_pred[tr_clear] == ytr[:4000][tr_clear]).float().mean().item() if tr_clear.any() else 0.0
        if acc_va >= best:
            best, best_state = acc_va, {k: v.detach().clone() for k, v in model.state_dict().items()}
        print(f'epoch {epoch+1}: loss {total:.3f} train {acc_tr*100:.2f}% val {acc_va*100:.2f}%', flush=True)
    if best_state:
        model.load_state_dict(best_state)
    model.eval()
    with torch.no_grad():
        logits, _ = model(xte.to(device))
        pred = logits.argmax(1).cpu()
        clear = ute == 0
        acc_te = (pred[clear] == yte[clear]).float().mean().item() if clear.any() else 0.0
    print(f'TEST {acc_te*100:.2f}% (best val {best*100:.2f}%)', flush=True)
    per_class = {c: round(float((pred[(yte == c) & clear] == c).float().mean()), 4)
                 for c in range(1, 8) if ((yte == c) & clear).sum()}
    (out / 'result.json').write_text(json.dumps({
        'arch': args.arch, 'aug': args.aug, 'width': args.width, 'epochs': args.epochs,
        'lr': args.lr, 'testAccuracy': round(float(acc_te), 4), 'bestVal': round(float(best), 4),
        'perClass': per_class, 'train': len(xtr), 'val': len(xva), 'test': len(xte),
    }, ensure_ascii=False, indent=1), encoding='utf-8')
    onnx_path = out / 'next_hold.candidate.onnx'
    dummy = torch.zeros(1, 3, SIZE, SIZE, device=device)
    torch.onnx.export(model, dummy, str(onnx_path), input_names=['image'],
                      output_names=['piece_logits', 'unclear_logit'], opset_version=17,
                      dynamic_axes={'image': {0: 'batch'}, 'piece_logits': {0: 'batch'},
                                    'unclear_logit': {0: 'batch'}}, dynamo=False)
    print(f'wrote {onnx_path}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
