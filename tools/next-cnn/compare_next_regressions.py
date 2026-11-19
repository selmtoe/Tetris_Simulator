"""旧解析（警告なし）→新解析（警告あり）になった試合の NEXT 差分を画像つきでまとめる。

  python compare_next_regressions.py --old <batch-results.json> --new <batch-results.json>
                                      --video <mkv> --out <dir>

- 旧解析で警告なし・新解析で警告ありの試合を対象
- 両解析の autosave ラベル（labels.pX[phase][5] = NEXT文字列）を比較し、
  最初に食い違ったフェーズの画像を動画から切り出して並べる
"""
from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

def jp_font(size: int):
    for candidate in (r'C:\Windows\Fonts\meiryo.ttc', r'C:\Windows\Fonts\YuGothM.ttc',
                      r'C:\Windows\Fonts\msgothic.ttc'):
        try:
            return ImageFont.truetype(candidate, size)
        except Exception:
            continue
    return ImageFont.load_default()

FONT_S = None
FONT_M = None

REF_W, REF_H = 850.0, 478.0
REGION = {
    '1': (250, 58, 380, 120),   # p1: HOLD 右の NEXT 5枠（850x478 基準）
    '2': (670, 58, 800, 120),   # p2: NEXT 5枠
}
COLORS = {
    '_': (40, 40, 48), 'I': (80, 200, 230), 'O': (240, 205, 50), 'T': (200, 90, 200),
    'L': (240, 150, 50), 'J': (70, 110, 230), 'S': (90, 210, 80), 'Z': (230, 70, 70),
}


def load_json(path: Path):
    return json.loads(path.read_text(encoding='utf-8'))


def frame_mapping(width: int, height: int):
    aspect = width / height
    if aspect > 16 / 9:
        crop_w = height * 16 / 9
        x, y = (width - crop_w) / 2, 0.0
    else:
        crop_w = width
        x, y = 0.0, (height - crop_w * 9 / 16) / 2
    scale = crop_w / 1920.0
    return x, y, scale


def crop_rect(region, width, height):
    x, y, scale = frame_mapping(width, height)
    content_w = scale * 1920.0
    content_h = content_w * 9 / 16
    rx, ry, rx1, ry1 = region
    x0 = round(x + rx * content_w / REF_W)
    y0 = round(y + ry * content_h / REF_H)
    x1 = round(x + rx1 * content_w / REF_W)
    y1 = round(y + ry1 * content_h / REF_H)
    return x0, y0, x1, y1


def grab_frame(video: Path, seconds: float, out_png: Path) -> bool:
    ffmpeg = str(Path(__file__).resolve().parents[1] / 'video-recovery-runtime' / 'bin' / 'ffmpeg.exe')
    if not Path(ffmpeg).is_file():
        ffmpeg = 'ffmpeg'
    cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-ss', f'{seconds:.3f}',
           '-i', str(video), '-frames:v', '1', '-y', str(out_png)]
    try:
        subprocess.run(cmd, check=True, timeout=120)
    except Exception:
        return False
    return out_png.is_file()


def next_rows(autosave: Path, player: str):
    d = load_json(autosave)
    rows = (d.get('labels') or {}).get(player) or []
    out = []
    for r in rows:
        try:
            out.append({'start': float(r[0]) / 1000.0, 'end': float(r[1]) / 1000.0,
                        'piece': r[2], 'next': str(r[5] or '')})
        except Exception:
            continue
    return out


def draw_text_block(draw, xy, text, fill=(235, 235, 240), size=16):
    draw.text(xy, text, fill=fill)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--old', required=True)
    parser.add_argument('--new', required=True)
    parser.add_argument('--video', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    old_root = Path(args.old).parent
    new_root = Path(args.new).parent
    old_manifest = load_json(Path(args.old))
    new_manifest = load_json(Path(args.new))
    video = Path(args.video)

    old_warn = {m['index']: bool((m.get('quality_warning') or {}).get('suspicious')) for m in old_manifest['matches']}
    new_warn = {m['index']: bool((m.get('quality_warning') or {}).get('suspicious')) for m in new_manifest['matches']}
    new_by_index = {m['index']: m for m in new_manifest['matches']}

    targets = [i for i in sorted(new_warn) if new_warn[i] and not old_warn.get(i, False)]
    print('旧=警告なし / 新=警告あり:', targets, flush=True)

    summary = []
    for idx in targets:
        match = new_by_index[idx]
        start_base = float((match.get('source_range') or {}).get('start_seconds') or 0.0)
        issues = (match.get('quality_warning') or {}).get('issues') or []
        player = str(issues[0].get('player')) if issues else '1'
        best = None
        folder = f'match_{idx:03d}'
        old_auto = old_root / folder / 'autosave' / 'latest_editing_autosave.v2.json'
        new_auto = new_root / folder / 'autosave' / 'latest_editing_autosave.v2.json'
        if not (old_auto.is_file() and new_auto.is_file()):
            print(f'試合{idx}: autosave なし', flush=True)
            continue
        diff = None
        for cand in ('1', '2'):
            o_rows = next_rows(old_auto, f'p{cand}')
            n_rows = next_rows(new_auto, f'p{cand}')
            lim = min(len(o_rows), len(n_rows))
            k0 = next((k for k in range(lim) if o_rows[k]['next'] != n_rows[k]['next']), None)
            if k0 is not None and (best is None or k0 < best[0]):
                best = (k0, cand, o_rows, n_rows)
        if best is not None:
            diff, player, old_rows, new_rows = best
        if diff is None:
            print(f'試合{idx}: NEXT文字列に差なし（盤面側の要因）', flush=True)
            summary.append({'match': idx, 'player': player, 'diff': None, 'issues': len(issues)})
            continue
        old_next = old_rows[diff]['next']
        new_next = new_rows[diff]['next']
        slot = next((s for s in range(min(len(old_next), len(new_next))) if old_next[s] != new_next[s]), 0)
        mid = (new_rows[diff]['start'] + new_rows[diff]['end']) / 2.0
        seconds = start_base + mid
        with tempfile.TemporaryDirectory() as tmp:
            png = Path(tmp) / 'frame.png'
            if not grab_frame(video, seconds, png):
                print(f'試合{idx}: フレーム取得失敗', flush=True)
                continue
            frame = Image.open(png).convert('RGB')
        box = crop_rect(REGION[player], frame.width, frame.height)
        crop = frame.crop(box)
        scale = 5
        crop = crop.resize((crop.width * scale, crop.height * scale), Image.LANCZOS)
        pad = 16
        height = crop.height + pad * 2 + 150
        width = max(crop.width + pad * 2, 900)
        sheet = Image.new('RGB', (width, height), (18, 19, 24))
        sheet.paste(crop, (pad, pad + 26))
        draw = ImageDraw.Draw(sheet)
        f_body = jp_font(17)
        f_title = jp_font(18)
        draw.text((pad, 6), f'試合 {idx}  P{player}  フェーズ {diff}  t={seconds:.2f}s',
                  fill=(255, 255, 255), font=f_title)
        y = crop.height + pad + 34
        draw.text((pad, y), f'旧NEXT: {old_next}', fill=(120, 220, 120), font=f_body)
        draw.text((pad, y + 26), f'新NEXT: {new_next}', fill=(240, 150, 150), font=f_body)
        if slot < 5:
            draw.text((pad, y + 52), f'違い: {slot+1}番目  旧={old_next[slot]} 新={new_next[slot]}',
                      fill=(255, 220, 120), font=f_body)
        info = ' / '.join(
            f"{i.get('player')}P {i.get('first_phase')}-{i.get('last_phase')} "
            f"一致率{i.get('minimum_match_ratio')}" for i in issues[:3])
        draw.text((pad, y + 80), f'新解析の不一致区間: {info}', fill=(180, 180, 195), font=f_body)
        target = out_dir / f'next-diff-match-{idx:03d}.png'
        sheet.save(target)
        print('wrote', target, flush=True)
        summary.append({'match': idx, 'player': player, 'phase': diff, 'seconds': round(seconds, 2),
                        'old': old_next, 'new': new_next, 'slot': slot + 1,
                        'old_piece': old_next[slot], 'new_piece': new_next[slot],
                        'issues': len(issues)})
    (out_dir / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding='utf-8')
    print(json.dumps(summary, ensure_ascii=False, indent=1))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
