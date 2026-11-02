"""検出JSONの classification_runs から winner-detection.json を作る（フォールバック）。

2026-11-02〜の本線は ppt-end3-winner.py（終了3クラスモデル）。
このスクリプトは、動画が無い・再デコードが遅い場合のフォールバックとして、
従来の10クラス・シーンモデルの票（8=P1勝ち / 9=P2勝ち / 6=終了）を数える。
6 は「終了だが勝敗不明」であり、引き分けではない。

従来の detect-winners.py は試合ごとに動画を切り直して再デコードしていたが、
検出パスがすでに end / end_p1 / end_p2 を毎フレーム出しているので、
その票を数えるだけで同じ結果が得られる（145試合で完全一致を確認済み）。

終了コード:
  0 : 決められた試合が十分（determinedRatio >= --min-determined-ratio）
  2 : 決められた試合が少なすぎる（8クラスモデル等）→ 従来方式へフォールバック
  1 : 入力不備などのハードエラー
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

WINDOW_SECONDS = 3.0
# 結果画面の「やった！」バナーは end 判定より後に出ることがある。
# 実測（MT6d-MyE6MY, 360p/30fps）で +1s では 19/29、+3s で 22/29 まで回復した。
FORWARD_SECONDS = 3.0
CONF_OK = 0.60
CONF_REVIEW = 0.20
MIN_DETERMINED_RATIO = 0.30
EXIT_UNDETERMINED = 2
# 結果画面の票がこの秒数に満たない試合は判定不能にする。
# 境界が結果画面より後ろへずれた試合では、3秒窓に結果画面が入らず
# 数フレームの誤票（「他プレイヤーの入力を待っています」画面は引き分けに
# 見えやすい）だけで勝敗を書いてしまうため、票の総量で足切りする。
# 引き分けは誤検出しやすい（「他プレイヤーの入力を待っています」画面が
# 引き分けに見える）ので、引き分けと確定するには票がこの秒数以上必要。
MIN_DRAW_EVIDENCE_SECONDS = 1.0
# P1/P2 は短い票でも、はっきり偏っていれば決めてよい（結果画面が短い試合がある）。
MIN_SIDE_EVIDENCE_SECONDS = 0.25
MIN_SIDE_MARGIN = 0.60


def decide_end_state(p1: int, p2: int, draw: int, fps: float):
    """終了3クラスの票から勝敗を決める。(winner, status, confidence) を返す。"""
    total = p1 + p2 + draw
    if total <= 0:
        return None, 'undetermined', 0.0
    if draw > max(p1, p2) and draw >= total * 0.5:
        if total < MIN_DRAW_EVIDENCE_SECONDS * fps:
            # 数フレームの「引き分け」は待機画面などの誤検出。決めない。
            return None, 'undetermined', 0.0
        confidence = round(draw / total, 4)
        return 'draw', ('ok' if confidence >= CONF_OK else 'review'), confidence
    winner = 'p1' if p1 >= p2 else 'p2'
    confidence = round(abs(p1 - p2) / total, 4)
    if total < MIN_SIDE_EVIDENCE_SECONDS * fps or confidence < MIN_SIDE_MARGIN:
        return None, 'undetermined', 0.0
    return winner, ('ok' if confidence >= CONF_OK else 'review'), confidence
# 3秒窓に結果画面が入らない試合だけ、end の手前までこの秒数だけ遡って
# 最後の結果画面ブロックを探す（境界が結果画面より後ろへずれた試合の救済）。
BACKTRACK_SECONDS = 40.0
# 遡って拾うブロックの足切り。ゲーム中に 8/9 が一瞬出る誤検出を除くため、
# 長さと平均確信度の両方を要求する。
BACKTRACK_MIN_SECONDS = 2.0
BACKTRACK_MIN_CONFIDENCE = 0.90
BACKTRACK_GAP_SECONDS = 0.3


def last_result_block(items, fps: float, low: int, high: int):
    """[low, high) のうち最後に現れた「結果画面らしい」連続ブロックの票。

    戻り値は {ラベル: フレーム数}。足切りを満たすブロックが無ければ None。
    """
    picked = []
    for start, stop, label, confidence, *_rest in items:
        if label not in (6, 8, 9):
            continue
        if stop <= low or start >= high:
            continue
        picked.append((max(start, low), min(stop, high), label, confidence))
    picked.sort()
    merged = []
    for start, stop, label, confidence in picked:
        span = stop - start
        if merged and start - merged[-1]['end'] <= BACKTRACK_GAP_SECONDS * fps:
            block = merged[-1]
            block['end'] = max(block['end'], stop)
            block['labels'][label] = block['labels'].get(label, 0) + span
            block['confidenceSum'] += confidence * span
            block['frames'] += span
        else:
            merged.append({'end': stop, 'labels': {label: span},
                           'confidenceSum': confidence * span, 'frames': span})
    best = None
    for block in merged:
        seconds = block['frames'] / fps if fps else 0.0
        if seconds < BACKTRACK_MIN_SECONDS:
            continue
        if block['confidenceSum'] / block['frames'] < BACKTRACK_MIN_CONFIDENCE:
            continue
        best = block
    return best['labels'] if best else None


def build(detection: Path, min_determined_ratio: float) -> dict:
    data = json.loads(detection.read_text(encoding='utf-8'))
    fps = float(data['video']['fps'])
    runs = data.get('classification_runs') or {}
    items = runs.get('items') or []
    if not items:
        raise SystemExit(f'classification_runs が空です（部分解析の可能性）: {detection}')
    # エンジン内で終了3クラスモデルを適用した検出は settings.end_state_model を持つ。
    # その場合ラベル6は「引き分け」、8/9はP1/P2勝ち。
    end_state = bool((data.get('settings') or {}).get('end_state_model'))
    rows = []
    for match in data['matches']:
        end_frame = int(match['end']['frame'])
        low = max(0, int(end_frame - WINDOW_SECONDS * fps))
        high = int(end_frame + FORWARD_SECONDS * fps)
        p1 = p2 = draw = end_any = 0
        for start, stop, label, *_ in items:
            if stop <= low or start >= high:
                continue
            overlap = min(stop, high) - max(start, low)
            if label == 8:
                p1 += overlap
            elif label == 9:
                p2 += overlap
            elif label == 6:
                if end_state:
                    draw += overlap
                else:
                    end_any += overlap
        backtrack = ''
        total = p1 + p2 + draw
        winner, status, confidence = decide_end_state(p1, p2, draw, fps)
        if end_state and winner is None:
            # 境界が結果画面より後ろへずれると 3秒窓に結果画面が入らない。
            # そのときは end の手前まで遡って最後の結果画面ブロックを探す。
            block = last_result_block(
                items, fps,
                max(0, int(end_frame - BACKTRACK_SECONDS * fps)), end_frame)
            if block:
                p1 = block.get(8, 0)
                p2 = block.get(9, 0)
                draw = block.get(6, 0)
                total = p1 + p2 + draw
                winner, status, confidence = decide_end_state(p1, p2, draw, fps)
                backtrack = 'result-block'
        evidence = total / fps if fps else 0.0
        if end_state:
            end_votes = total
        else:
            total = p1 + p2
            evidence = end_any / fps if fps else 0.0
            if total == 0:
                winner, status, confidence = None, 'undetermined', 0.0
            else:
                winner = 'p1' if p1 >= p2 else 'p2'
                confidence = round(abs(p1 - p2) / total, 4)
                status = 'ok' if confidence >= CONF_OK else 'review'
            end_votes = end_any
        rows.append({
            'match': int(match['index']),
            'matchEnd': round(float(match['analysis_range']['end_seconds']), 3),
            'frameSeconds': round(float(match['analysis_range']['end_seconds']), 3),
            'votesP1': p1,
            'votesP2': p2,
            'votesDraw': draw,
            'endVotes': end_votes,
            'evidenceSeconds': round(evidence, 3),
            'backtrack': backtrack,
            'confidence': confidence,
            'winner': winner,
            'status': status,
            'rawWinner': winner,
            'source': 'scene-runs-inline' if end_state else 'scene-runs',
        })
    determined = sum(1 for row in rows if row['winner'] is not None)
    ratio = round(determined / len(rows), 4) if rows else 0.0
    return {
        'schema': 'tetris-winner-detection/v1',
        'engine': 'scene-runs-inline' if end_state else 'scene-runs',
        'source': str(detection),
        'config': {'windowSeconds': WINDOW_SECONDS, 'forwardSeconds': FORWARD_SECONDS,
                   'confidenceOk': CONF_OK, 'confidenceReview': CONF_REVIEW,
                   'endState': end_state,
                   'backtrackSeconds': BACKTRACK_SECONDS if end_state else 0.0,
                   'minDrawEvidenceSeconds': MIN_DRAW_EVIDENCE_SECONDS if end_state else 0.0,
                   'minSideEvidenceSeconds': MIN_SIDE_EVIDENCE_SECONDS if end_state else 0.0,
                   'minSideMargin': MIN_SIDE_MARGIN if end_state else 0.0,
                   'minDeterminedRatio': min_determined_ratio,
                   'determinedRatio': ratio,
                   'determinedMatches': determined,
                   'totalMatches': len(rows)},
        'matches': rows,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--batch-dir', required=True)
    parser.add_argument('--detection', default='')
    parser.add_argument('--output', default='')
    parser.add_argument('--min-determined-ratio', type=float, default=MIN_DETERMINED_RATIO,
                        help='決められた試合の割合がこれ未満なら終了コード 2（既定 0.3）')
    args = parser.parse_args()

    if not 0.0 <= args.min_determined_ratio <= 1.0:
        raise SystemExit('--min-determined-ratio は 0.0〜1.0 で指定してください')

    batch = Path(args.batch_dir)
    detection = Path(args.detection) if args.detection else batch / 'segments.matches.json'
    if not detection.is_file():
        raise SystemExit(f'検出JSONがありません: {detection}')
    payload = build(detection, args.min_determined_ratio)
    out = Path(args.output) if args.output else batch / 'winner-detection.json'
    out.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding='utf-8')

    rows = payload['matches']
    ok = sum(1 for row in rows if row['status'] == 'ok')
    review = sum(1 for row in rows if row['status'] == 'review')
    p1 = sum(1 for row in rows if row['winner'] == 'p1')
    p2 = sum(1 for row in rows if row['winner'] == 'p2')
    draw = sum(1 for row in rows if row['winner'] == 'draw')
    determined = payload['config']['determinedMatches']
    ratio = payload['config']['determinedRatio']
    print(f'勝敗（分類ログから）: 確定{ok} / 要確認{review} / 計{len(rows)} '
          f'（P1勝ち={p1} P2勝ち={p2} 引き分け={draw}）')
    print(f'  決定率 {determined}/{len(rows)} = {ratio:.1%}'
          f'（しきい値 {args.min_determined_ratio:.0%}）')
    print(f'  saved {out}')
    if ratio < args.min_determined_ratio:
        print('勝敗を決められた試合が少なすぎます（結果画面が無い／8クラスモデルなど）。'
              '従来の再デコード方式へフォールバックします。')
        raise SystemExit(EXIT_UNDETERMINED)


if __name__ == '__main__':
    main()
