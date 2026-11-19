"""NEXT/HOLD CNN の設計比較・学習（形状＋色・低解像度耐性つき）。

  python loop_next2.py --images <images.u8> --metadata <metadata.tsv> --out <dir>
                       --arch spatial|dual|resnet|base --aug 2 --epochs 20

設計の狙い:
  - ピースは形状が固定なので、**形の情報を潰さない**構造にする（GAPだけに頼らない）
  - 白エフェクトで色が飛んでも、**彩度マスク（形）**から判別できるよう4ch入力にする（dual）
  - 低解像度・圧縮ノイズ・白かぶりを学習時に加えて耐性を作る（--aug 2）
  - 出力は piece_logits[8] と unclear_logit の2系統（エンジン仕様に合わせる）
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import numpy as np
import torch
from torch import nn

SIZE = 48
CLASSES = 8


def load_rows(path: Path):
    return list(csv.DictReader(path.open(encoding='utf-8'), delimiter='\t'))


class Block(nn.Module):
    def __init__(self, cin, cout, stride=1):
        super().__init__()
        self.conv1 = nn.Conv2d(cin, cout, 3, stride, 1, bias=False)
        self.bn1 = nn.BatchNorm2d(cout)
        self.conv2 = nn.Conv2d(cout, cout, 3, 1, 1, bias=False)
        self.bn2 = nn.BatchNorm2d(cout)
        self.short = None
        if stride != 1 or cin != cout:
            self.short = nn.Sequential(nn.Conv2d(cin, cout, 1, stride, bias=False), nn.BatchNorm2d(cout))

    def forward(self, x):
        out = nn.functional.silu(self.bn1(self.conv1(x)))
        out = self.bn2(self.conv2(out))
        skip = x if self.short is None else self.short(x)
        return nn.functional.silu(out + skip)


class Net(nn.Module):
    """arch:
         base    : 3ch入力 → GAP（従来）
         spatial : 3ch入力 → 6x6 空間マップを flatten（形を保つ）
         dual    : 4ch入力（RGB + 彩度マスク）→ 6x6 flatten
         resnet  : 4ch入力 + 残差ブロック → GAP+GMP
    """

    def __init__(self, chunks: int = 4, arch: str = 'spatial', width: int = 32):
        super().__init__()
        self.arch = arch
        cin = 4 if arch in ('dual', 'resnet') else 3
        w = width
        self.stem = nn.Sequential(
            nn.Conv2d(cin, w, 3, 1, 1, bias=False), nn.BatchNorm2d(w), nn.SiLU(),
            Block(w, w),
        )
        self.b2 = Block(w, w * 2, 2)
        self.b3 = Block(w * 2, w * 4, 2)
        self.b4 = Block(w * 4, w * 4, 2)
        feat = w * 4
        if arch in ('spatial', 'dual'):
            self.pool = nn.AdaptiveAvgPool2d(6)
            head_in = feat * 36
        elif arch == 'resnet':
            head_in = feat * 2
        else:
            self.pool = nn.AdaptiveAvgPool2d(1)
            head_in = feat
        self.drop = nn.Dropout(0.15)
        self.piece = nn.Linear(head_in, CLASSES)
        self.unclear = nn.Linear(head_in, 1)

    def forward(self, x):
        if self.arch in ('dual', 'resnet'):
            mx = x.max(dim=1, keepdim=True).values
            mn = x.min(dim=1, keepdim=True).values
            sat = ((mx - mn) / mx.clamp(min=1e-6)).clamp(0, 1)
            x = torch.cat([x, sat], dim=1)
        f = self.stem(x)
        f = self.b2(f)
        f = self.b3(f)
        f = self.b4(f)
        if self.arch == 'resnet':
            avg = nn.functional.adaptive_avg_pool2d(f, 1).flatten(1)
            mxp = nn.functional.adaptive_max_pool2d(f, 1).flatten(1)
            f = torch.cat([avg, mxp], dim=1)
        else:
            f = self.pool(f).flatten(1)
        f = self.drop(f)
        return self.piece(f), self.unclear(f).squeeze(1)


def augment(x: torch.Tensor, level: int, gen: torch.Generator) -> torch.Tensor:
    device = x.device
    n = x.shape[0]

    def rnd(shape):
        return torch.rand(shape, device=device, generator=gen)

    out = x.clone()
    # 明るさ・コントラスト
    out = out * (rnd((n, 1, 1, 1)) * 0.6 + 0.7) + (rnd((n, 1, 1, 1)) - 0.5) * 0.35
    # ぼかし
    if rnd((1,)).item() < 0.5:
        kernel = torch.ones(3, 3, device=device) / 9.0
        blurred = nn.functional.conv2d(out, kernel.expand(3, 1, 3, 3), padding=1, groups=3)
        mask = rnd((n, 1, 1, 1)) < 0.5
        out = torch.where(mask, blurred, out)
    if level >= 2:
        # 低解像度化 → 戻す（360p相当のぼけ）
        if rnd((1,)).item() < 0.6:
            factor = int(torch.randint(2, 4, (1,), device=device, generator=gen).item())
            small = nn.functional.interpolate(out, scale_factor=1.0 / factor, mode='area')
            out = nn.functional.interpolate(small, size=(SIZE, SIZE), mode='bilinear', align_corners=False)
        # 圧縮ノイズ
        if rnd((1,)).item() < 0.5:
            out = (out * 7).round() / 7
        # 白かぶり（エフェクト）
        if rnd((1,)).item() < 0.35:
            alpha = rnd((n, 1, 1, 1)) * 0.85
            out = out * (1 - alpha) + alpha
        # 部分的に白くする（帯状）
        if rnd((1,)).item() < 0.3:
            h = int(torch.randint(6, 30, (1,), device=device, generator=gen).item())
            top = int(torch.randint(0, SIZE - h, (1,), device=device, generator=gen).item())
            mask = rnd((n, 1, 1, 1)) < 0.5
            patch = out.clone()
            patch[:, :, top:top + h, :] = 1.0
            out = torch.where(mask, patch, out)
    # cutout
    for _ in range(2):
        h = int(torch.randint(4, 16, (1,), device=device, generator=gen).item())
        w = int(torch.randint(4, 16, (1,), device=device, generator=gen).item())
        top = int(torch.randint(0, SIZE - h, (1,), device=device, generator=gen).item())
        left = int(torch.randint(0, SIZE - w, (1,), device=device, generator=gen).item())
        mask = rnd((n, 1, 1, 1)) < 0.5
        fill = rnd((n, 1, 1, 1))
        patch = out.clone()
        patch[:, :, top:top + h, left:left + w] = fill
        out = torch.where(mask, patch, out)
    return out.clamp(0, 1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--images', required=True)
    parser.add_argument('--metadata', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--arch', default='dual', choices=('base', 'spatial', 'dual', 'resnet'))
    parser.add_argument('--width', type=int, default=32)
    parser.add_argument('--aug', type=int, default=2)
    parser.add_argument('--epochs', type=int, default=20)
    parser.add_argument('--batch-size', type=int, default=256)
    parser.add_argument('--lr', type=float, default=3e-3)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--test-split', default='test')
    args = parser.parse_args()
    torch.manual_seed(args.seed)
    np.random.seed(args.seed)
    device = 'cuda' if torch.cuda.is_available() else 'cpu'
    gen = torch.Generator(device=device).manual_seed(args.seed)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    rows = load_rows(Path(args.metadata))
    raw = np.fromfile(args.images, dtype=np.uint8)
    data = raw.reshape(-1, 3, SIZE, SIZE)
    index = np.array([int(r['image_index']) for r in rows])
    labels = np.array([max(0, int(float(r['label']))) for r in rows])
    unclear = np.array([1.0 if (int(float(r['label'])) <= 0 or str(r.get('unclear', '0')) == '1') else 0.0
                        for r in rows], dtype=np.float32)
    splits = np.array([r['split'] for r in rows])

    def take(mask):
        idx = index[mask]
        x = data[idx].astype(np.float32) / 255.0
        return torch.from_numpy(x), torch.from_numpy(labels[mask]), torch.from_numpy(unclear[mask])

    xtr, ytr, utr = take(splits == 'train')
    xva, yva, uva = take(splits == 'val')
    xte, yte, ute = take(splits == args.test_split)
    print(f'arch={args.arch} aug={args.aug} width={args.width} train={len(xtr)} val={len(xva)} test={len(xte)}', flush=True)

    model = Net(arch=args.arch, width=args.width).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.epochs)
    counts = torch.bincount(ytr[utr == 0], minlength=CLASSES).float().clamp(min=1)
    weights = (counts.sum() / (CLASSES * counts)).to(device)
    best, best_state = 0.0, None
    for epoch in range(args.epochs):
        model.train()
        order = torch.randperm(len(xtr))
        for start in range(0, len(order), args.batch_size):
            sel = order[start:start + args.batch_size]
            xb, yb, ub = xtr[sel].to(device), ytr[sel].to(device), utr[sel].to(device)
            if args.aug:
                xb = augment(xb, args.aug, gen)
            opt.zero_grad()
            logits, unclear_logit = model(xb)
            clear = ub == 0
            piece_loss = (nn.functional.cross_entropy(logits[clear], yb[clear], weight=weights,
                                                      label_smoothing=0.05)
                          if clear.any() else torch.zeros((), device=device))
            unclear_loss = nn.functional.binary_cross_entropy_with_logits(unclear_logit, ub)
            loss = piece_loss + 0.4 * unclear_loss
            loss.backward()
            opt.step()
            if epoch == 0 and start < 600:
                print(f'  step {start//args.batch_size} loss={float(loss):.4f} '
                      f'piece={float(piece_loss):.4f} unclear={float(unclear_loss):.4f} '
                      f'logits_mean={float(logits.mean()):.4f}', flush=True)
        sched.step()
        model.eval()
        with torch.no_grad():
            pred = model(xva.to(device))[0].argmax(1).cpu()
            clear = uva == 0
            acc_va = (pred[clear] == yva[clear]).float().mean().item() if clear.any() else 0.0
            tr_pred = model(xtr[:4000].to(device))[0].argmax(1).cpu()
            tr_clear = utr[:4000] == 0
            acc_tr = (tr_pred[tr_clear] == ytr[:4000][tr_clear]).float().mean().item() if tr_clear.any() else 0.0
        if epoch == 0:
            print(f'DEBUG clear_train={int(tr_clear.sum())}/{len(tr_clear)} clear_val={int(clear.sum())}/{len(clear)} '
                  f'pred_classes={sorted(set(tr_pred.tolist()))} label_classes={sorted(set(ytr[:4000][tr_clear].tolist()))}',
                  flush=True)
        if acc_va >= best:
            best = acc_va
            best_state = {k: v.detach().clone() for k, v in model.state_dict().items()}
        print(f'epoch {epoch+1}: train {acc_tr*100:.2f}% val {acc_va*100:.2f}%', flush=True)
    if best_state:
        model.load_state_dict(best_state)
    model.eval()
    with torch.no_grad():
        logits, _ = model(xte.to(device))
        pred = logits.argmax(1).cpu()
        clear = ute == 0
        acc_te = (pred[clear] == yte[clear]).float().mean().item() if clear.any() else 0.0
    print(f'TEST {acc_te*100:.2f}% (best val {best*100:.2f}%)', flush=True)
    per_class = {}
    for c in range(1, CLASSES):
        mask = (yte == c) & clear
        if mask.sum():
            per_class[c] = round(float((pred[mask] == c).float().mean()), 4)
    (out / 'result.json').write_text(json.dumps({
        'arch': args.arch, 'aug': args.aug, 'width': args.width, 'epochs': args.epochs,
        'lr': args.lr, 'seed': args.seed, 'train': len(xtr), 'val': len(xva), 'test': len(xte),
        'testAccuracy': round(float(acc_te), 4), 'bestVal': round(float(best), 4), 'perClass': per_class,
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
