"""終了3クラスモデルで勝敗（引き分け / P1 / P2）を判定する。

これまでの detect-winners.py は旧WinnerCNN（左右の半画面）を使っていた。
この版は、結果画面だけを学習した3クラスCNN（artifacts-end3）を使う。

流れ:
  1. segments.matches.json の試合終了時刻から [end-3s, end+3s] を 5fps で切り出す
  2. 旧10クラス・シーンモデルで「結果画面らしいフレーム」だけに絞る
     （60_end/80_end_p1/90_end_p2 の合計がしきい値以上）
  3. 3クラスモデル（引き分け/P1/P2）で各フレームを分類し、平均確率で決める
  4. winner-detection.json を書き出す

使い方:
  python ppt-end3-winner.py --batch-dir "<バッチ名>" [--video <動画>] [--workers 6]
                            [--output <json>] [--reference <旧winner-detection.json>]

終了コード:
  0 : 判定できた試合が十分
  2 : 結果画面が取れない試合が多すぎる（呼び出し側は従来方式へフォールバック）
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort

ROOT = Path(r"C:\Users\hirom\Documents\tetris ai")
RUNTIME = ROOT / "tools" / "video-recovery-runtime"
BATCHES = RUNTIME / "data" / "batches"
FFMPEG = RUNTIME / "bin" / "ffmpeg.exe"
SCENE_MODEL = ROOT / "tools" / "ppt-lab" / "models" / "fast320" / "scene-classifier.onnx"
END_MODEL = ROOT / "tools" / "ppt-lab" / "models" / "end3" / "scene-classifier.onnx"

WIDTH, HEIGHT = 640, 360
FPS = 5.0
WINDOW_BACK = 3.0
WINDOW_FORWARD = 3.0
SCENE_END_MIN = 0.45
SCENE_BLACKOUT_MAX = 0.50
DRAW_MIN = 0.60
CONF_OK = 0.60
CONF_REVIEW = 0.20
MIN_DETERMINED_RATIO = 0.30


def extract_window(video: Path, start: float, length: float) -> np.ndarray:
    command = [
        str(FFMPEG), "-hide_banner", "-loglevel", "error",
        "-ss", f"{max(0.0, start):.3f}", "-t", f"{length:.3f}", "-i", str(video),
        "-vf", f"fps={FPS},scale={WIDTH}:{HEIGHT}",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-",
    ]
    data = subprocess.run(command, capture_output=True).stdout
    size = WIDTH * HEIGHT * 3
    count = len(data) // size
    if count == 0:
        return np.empty((0, HEIGHT, WIDTH, 3), dtype=np.uint8)
    return np.frombuffer(data[: count * size], dtype=np.uint8).reshape(
        count, HEIGHT, WIDTH, 3)


def to_batch(frames: np.ndarray) -> np.ndarray:
    grays = np.stack([
        cv2.resize(cv2.cvtColor(frame, cv2.COLOR_RGB2GRAY), (160, 90),
                   interpolation=cv2.INTER_AREA)
        for frame in frames]).astype(np.float32)
    return grays[:, None, :, :] / 127.5 - 1.0


def softmax(logits: np.ndarray) -> np.ndarray:
    exp = np.exp(logits - logits.max(axis=1, keepdims=True))
    return exp / exp.sum(axis=1, keepdims=True)


def analyse_match(scene_session, scene_input, end_session, end_input,
                  video: Path, match: dict, limit_seconds: float) -> dict:
    end = float(match["analysis_range"]["end_seconds"])
    start = max(0.0, end - WINDOW_BACK)
    length = WINDOW_FORWARD + WINDOW_BACK
    if limit_seconds:
        length = min(length, max(0.0, limit_seconds - start))
    record = {"match": int(match["index"]), "matchEnd": round(end, 3)}
    frames = extract_window(video, start, length)
    record["frames"] = int(len(frames))
    if len(frames) == 0:
        record.update({"winner": None, "status": "undetermined", "confidence": 0.0,
                       "votesP1": 0, "votesP2": 0, "votesDraw": 0, "selectedFrames": 0,
                       "source": "end3-model"})
        return record

    scene = softmax(scene_session.run(None, {scene_input: to_batch(frames)})[0])
    end_scores = scene[:, 6] + scene[:, 8] + scene[:, 9]
    selected = [
        index for index in range(len(frames))
        if float(end_scores[index]) >= SCENE_END_MIN and float(scene[index, 7]) < SCENE_BLACKOUT_MAX
    ]
    if not selected:
        record.update({"winner": None, "status": "undetermined", "confidence": 0.0,
                       "votesP1": 0, "votesP2": 0, "votesDraw": 0, "selectedFrames": 0,
                       "source": "end3-model"})
        return record

    probabilities = softmax(end_session.run(
        None, {end_input: to_batch(frames[selected])})[0]).mean(axis=0)
    draw, p1, p2 = (float(probabilities[0]), float(probabilities[1]), float(probabilities[2]))
    top = max(draw, p1, p2)
    if draw >= DRAW_MIN and draw > max(p1, p2):
        winner = "draw"
    else:
        winner = "p1" if p1 >= p2 else "p2"
    ordered = sorted([draw, p1, p2], reverse=True)
    confidence = ordered[0] - ordered[1]
    if top >= CONF_OK and confidence >= CONF_REVIEW:
        status = "ok"
    else:
        status = "review"
    record.update({
        "winner": winner,
        "status": status,
        "confidence": round(confidence, 4),
        "votesP1": round(p1, 4),
        "votesP2": round(p2, 4),
        "votesDraw": round(draw, 4),
        "selectedFrames": len(selected),
        "source": "end3-model",
    })
    return record


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--batch-dir", required=True)
    parser.add_argument("--video", default=None)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--output", default=None)
    parser.add_argument("--reference", default=None,
                        help="既存の winner-detection.json（精度比較用）")
    parser.add_argument("--min-determined-ratio", type=float, default=MIN_DETERMINED_RATIO)
    args = parser.parse_args()

    batch_dir = Path(args.batch_dir)
    if not batch_dir.is_dir():
        batch_dir = BATCHES / args.batch_dir
    detection_path = batch_dir / "segments.matches.json"
    if not detection_path.is_file():
        raise SystemExit(f"segments.matches.json がありません: {detection_path}")
    detection = json.loads(detection_path.read_text(encoding="utf-8"))
    video = Path(args.video) if args.video else None
    if video is None:
        manifest_path = batch_dir / "batch-results.json"
        if manifest_path.is_file():
            manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
            video = Path(str(manifest.get("source") or ""))
    if video is None or not video.is_file():
        raise SystemExit(f"動画が見つかりません: {video}")
    duration = float(detection.get("video", {}).get("duration_seconds") or 0.0)

    scene_session = ort.InferenceSession(str(SCENE_MODEL), providers=["CPUExecutionProvider"])
    end_session = ort.InferenceSession(str(END_MODEL), providers=["CPUExecutionProvider"])
    scene_input = scene_session.get_inputs()[0].name
    end_input = end_session.get_inputs()[0].name

    matches = detection["matches"]
    if args.limit:
        matches = matches[: args.limit]
    started = time.time()
    print(f"{batch_dir.name}: {len(matches)} matches "
          f"workers={args.workers} video={video.name}", flush=True)

    def job(match):
        return analyse_match(scene_session, scene_input, end_session, end_input,
                             video, match, duration)

    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        records = list(pool.map(job, matches))
    records.sort(key=lambda row: row["match"])

    determined = sum(1 for row in records if row.get("winner") is not None)
    ratio = determined / max(1, len(records))
    payload = {
        "schema": "tetris-winner-detection/v1",
        "batch": batch_dir.name,
        "source": str(video),
        "engine": "end3-model",
        "models": {"scene": str(SCENE_MODEL), "endState": str(END_MODEL)},
        "config": {"fps": FPS, "scale": [WIDTH, HEIGHT],
                   "windowBack": WINDOW_BACK, "windowForward": WINDOW_FORWARD,
                   "sceneEndMin": SCENE_END_MIN, "drawMin": DRAW_MIN,
                   "confidenceOk": CONF_OK, "confidenceReview": CONF_REVIEW,
                   "minDeterminedRatio": args.min_determined_ratio,
                   "determinedRatio": round(ratio, 4),
                   "determinedMatches": determined, "totalMatches": len(records)},
        "matches": records,
    }
    out = Path(args.output) if args.output else batch_dir / "winner-detection.json"
    out.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")

    p1 = sum(1 for row in records if row.get("winner") == "p1")
    p2 = sum(1 for row in records if row.get("winner") == "p2")
    draw = sum(1 for row in records if row.get("winner") == "draw")
    ok = sum(1 for row in records if row.get("status") == "ok")
    review = sum(1 for row in records if row.get("status") == "review")
    undetermined = sum(1 for row in records if row.get("status") == "undetermined")
    print(f"結果: 確定{ok} / 要確認{review} / 判定不能{undetermined}  "
          f"P1={p1} P2={p2} 引き分け={draw}")
    print(f"  決定率 {determined}/{len(records)} = {ratio:.1%}  {time.time() - started:.0f}s")
    print(f"  saved {out}")

    if args.reference:
        reference = json.loads(Path(args.reference).read_text(encoding="utf-8"))
        expected = {int(row["match"]): row.get("winner") for row in reference.get("matches") or []}
        agree = total = 0
        for row in records:
            want = expected.get(int(row["match"]))
            if want not in ("p1", "p2"):
                continue
            total += 1
            if row.get("winner") == want:
                agree += 1
        if total:
            print(f"  参照との一致: {agree}/{total} = {agree / total:.1%}")

    if ratio < args.min_determined_ratio:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
