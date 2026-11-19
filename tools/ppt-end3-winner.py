"""終了3クラスモデルで勝敗（引き分け / P1 / P2）を判定する。

これまでの detect-winners.py は旧WinnerCNN（左右の半画面）を使っていた。
この版は、結果画面だけを学習した3クラスCNN（artifacts-end3）を使う。

流れ:
  1. segments.matches.json の試合終了時刻から [end-3s, end+3s] を 5fps で切り出す
  2. 旧10クラス・シーンモデルで「結果画面らしいフレーム」だけに絞る
     （60_end/80_end_p1/90_end_p2 の合計がしきい値以上）
  3. 3クラスモデル（引き分け/P1/P2）で各フレームを分類し、平均確率で決める
  4. winner-detection.json を書き出す

引き分けの条件（2026-11-03〜）:
  平均確率が draw >= 0.60 **かつ** 採用フレーム >= 20 のときだけ引き分けと確定する。
  どちらかを満たさないときは p1 / p2 の平均確率で決め、同点なら undetermined。
  従来は「draw が 60% 以上なら引き分け」だけだったが、結果画面を DRAW と読み違える
  フレームが数秒続くとハードラベルが引き分けに寄るため、フレーム数も条件に入れて
  短い区間の DRAW では引き分けを確定しない。採用フレームが 20 未満のまま DRAW が
  優勢なときは、p1 / p2 の平均確率で暫定の勝者を出し、status は review のまま残す。

使い方:
  python ppt-end3-winner.py --batch-dir "<バッチ名>" [--video <動画>] [--workers 6]
                            [--output <json>] [--reference <旧winner-detection.json>]

  # 迷った試合だけを動画方式で見直し、既存の winner-detection.json の該当行を差し替える
  python ppt-end3-winner.py --batch-dir "<バッチ名>" --only-matches 17,38 --merge

  --only-matches を指定しないときは従来どおり全試合を処理する。--merge では、
  判定できた試合の行だけを差し替え、他の行・config・schema はそのまま残す。
  動画が無い・フレームが取れない試合は既存の行を変更せず、payload の skipped に
  理由を残す（例外で全体を止めない）。

終了コード:
  0 : 判定できた試合が十分（--only-matches / --merge の部分実行は常に 0）
  2 : 結果画面が取れない試合が多すぎる（呼び出し側は従来方式へフォールバック）
"""
from __future__ import annotations

import argparse
import json
import re
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
# 引き分けを確定するのに必要な「結果画面として採用したフレーム数」。
# 6秒窓を 5fps で切ると最大 30 フレーム。20 未満は 4 秒未満の証拠しかない。
MIN_DRAW_FRAMES = 20
# p1 と p2 の平均確率がこれ未満しか違わなければ同点とみなす。
TIE_EPSILON = 1e-6
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


def analysis_end(match: dict) -> float | None:
    """試合終了時刻（秒）。壊れた行なら None を返す。"""
    try:
        return float(match["analysis_range"]["end_seconds"])
    except (KeyError, TypeError, ValueError):
        return None


def skipped_record(match: dict, reason: str) -> dict:
    """判定を変えずに残す行（動画が無い・フレームが取れないなど）。"""
    end = analysis_end(match)
    record = {
        "match": int(match["index"]),
        "winner": None,
        "status": "undetermined",
        "confidence": 0.0,
        "votesP1": 0.0,
        "votesP2": 0.0,
        "votesDraw": 0.0,
        "frames": 0,
        "selectedFrames": 0,
        "skipped": True,
        "skipReason": reason,
        "source": "end3-model",
    }
    if end is not None:
        record["matchEnd"] = round(end, 3)
    return record


def decide_from_probabilities(draw: float, p1: float, p2: float,
                              selected_frames: int) -> tuple[str | None, str, float, str]:
    """平均確率から勝敗を決める。(winner, status, confidence, reason) を返す。

    reason:
      draw       : draw >= DRAW_MIN かつ 採用フレーム >= MIN_DRAW_FRAMES
      draw-frames: draw >= DRAW_MIN だが採用フレーム不足 → p1/p2 で暫定
      p1 / p2    : draw < DRAW_MIN → p1/p2 の平均確率で決定
      tie        : p1 と p2 が同点 → 判定不能

    confidence は「勝者クラスの確率 − 相手クラスの確率」。引き分けのときは
    draw − max(p1, p2)、P1/P2 のときは p1 − p2 を使う（負の値にしない）。
    status が ok になるのは、勝者クラス自体が CONF_OK 以上で、かつ 2 位に
    CONF_REVIEW 以上差をつけたときだけ。
    """
    if draw >= DRAW_MIN and selected_frames >= MIN_DRAW_FRAMES:
        winner = "draw"
        candidate = draw
        runner_up = max(p1, p2)
        reason = "draw"
    elif abs(p1 - p2) < TIE_EPSILON:
        return None, "undetermined", 0.0, "tie"
    else:
        winner = "p1" if p1 > p2 else "p2"
        candidate = p1 if winner == "p1" else p2
        runner_up = p2 if winner == "p1" else p1
        reason = winner
        if draw >= DRAW_MIN:
            # フレーム不足で引き分けを確定できなかったケース。
            reason = "draw-frames"
    confidence = candidate - runner_up
    if candidate >= CONF_OK and confidence >= CONF_REVIEW:
        status = "ok"
    else:
        status = "review"
    return winner, status, confidence, reason


def parse_only_matches(value: str | None) -> list[int]:
    """`--only-matches 17,38` を [17, 38] にする。未指定なら空。"""
    if value is None:
        return []
    tokens = [token for token in re.split(r"[,\s]+", value.strip()) if token]
    indexes: list[int] = []
    for token in tokens:
        try:
            index = int(token)
        except ValueError:
            raise SystemExit(f"--only-matches の書式が不正です: {token!r}（例: 17,38）")
        if index < 1:
            raise SystemExit(f"--only-matches は 1 以上の試合番号で指定してください: {index}")
        if index not in indexes:
            indexes.append(index)
    return indexes


def resolve_video(batch_dir: Path, explicit: str | None) -> Path | None:
    """判定に使う動画を返す（--video 優先、無ければ batch-results.json の source）。"""
    if explicit:
        return Path(explicit)
    manifest_path = batch_dir / "batch-results.json"
    if not manifest_path.is_file():
        return None
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None
    source = str(manifest.get("source") or "")
    return Path(source) if source else None


def analyse_match(scene_session, scene_input, end_session, end_input,
                  video: Path, match: dict, limit_seconds: float) -> dict:
    end = analysis_end(match)
    if end is None:
        return skipped_record(match, "invalid-analysis-range")
    start = max(0.0, end - WINDOW_BACK)
    length = WINDOW_FORWARD + WINDOW_BACK
    if limit_seconds:
        length = min(length, max(0.0, limit_seconds - start))
    frames = extract_window(video, start, length)
    if len(frames) == 0:
        return skipped_record(match, "no-frames")

    scene = softmax(scene_session.run(None, {scene_input: to_batch(frames)})[0])
    end_scores = scene[:, 6] + scene[:, 8] + scene[:, 9]
    selected = [
        index for index in range(len(frames))
        if float(end_scores[index]) >= SCENE_END_MIN and float(scene[index, 7]) < SCENE_BLACKOUT_MAX
    ]
    if not selected:
        return skipped_record(match, "no-result-frames")

    probabilities = softmax(end_session.run(
        None, {end_input: to_batch(frames[selected])})[0]).mean(axis=0)
    draw, p1, p2 = (float(probabilities[0]), float(probabilities[1]), float(probabilities[2]))
    winner, status, confidence, reason = decide_from_probabilities(
        draw, p1, p2, len(selected))
    return {
        "match": int(match["index"]),
        "matchEnd": round(end, 3),
        "frames": int(len(frames)),
        "winner": winner,
        "status": status,
        "confidence": round(confidence, 4),
        "votesP1": round(p1, 4),
        "votesP2": round(p2, 4),
        "votesDraw": round(draw, 4),
        "selectedFrames": len(selected),
        "decision": reason,
        "source": "end3-model",
    }


def analyse_match_safe(scene_session, scene_input, end_session, end_input,
                       video: Path, match: dict, limit_seconds: float) -> dict:
    """1試合の失敗で全体を止めない（skipped として理由を残す）。"""
    try:
        return analyse_match(scene_session, scene_input, end_session, end_input,
                             video, match, limit_seconds)
    except Exception as error:  # noqa: BLE001 - 試合単位で切り離す
        print(f"  試合 {match.get('index')} の判定に失敗: {error}", file=sys.stderr)
        return skipped_record(match, f"error: {error}")


def merge_into_payload(existing: dict | None, records: list[dict],
                       skipped: list[dict]) -> dict:
    """既存の winner-detection.json に、判定できた試合の行だけを差し替えて返す。"""
    payload = dict(existing) if existing else {
        "schema": "tetris-winner-detection/v1",
        "engine": "end3-model",
        "config": {},
    }
    config = dict(payload.get("config") or {})
    rows: dict[int, dict] = {}
    for row in payload.get("matches") or []:
        if not isinstance(row, dict):
            continue
        index = row.get("match")
        if isinstance(index, int) and index not in rows:
            rows[index] = row

    replaced: list[int] = []
    for record in records:
        if record.get("skipped"):
            continue
        index = int(record["match"])
        updated = dict(record)
        previous = rows.get(index)
        if previous is not None:
            updated.setdefault("rawWinner", previous.get("winner"))
            if previous.get("winner") != updated.get("winner"):
                updated["previousWinner"] = previous.get("winner")
                updated["previousStatus"] = previous.get("status")
        # 動画方式でも決まらなかった行は、要確認の印を残す。
        updated["needsSecondOpinion"] = (
            updated.get("winner") in (None, "draw") or updated.get("status") != "ok")
        updated["secondOpinion"] = True
        rows[index] = updated
        replaced.append(index)

    ordered = [rows[index] for index in sorted(rows)]
    payload["matches"] = ordered
    config["secondOpinion"] = True
    config["secondOpinionMatches"] = len(replaced)
    config["secondOpinionSkipped"] = len(skipped)
    if skipped:
        payload["skipped"] = skipped
    else:
        payload.pop("skipped", None)
    remaining = [row["match"] for row in ordered if row.get("needsSecondOpinion")]
    if "needsSecondOpinionMatches" in config or remaining:
        config["needsSecondOpinionMatches"] = remaining
    payload["config"] = config
    return payload


def merge_detection(target: Path, records: list[dict], skipped: list[dict]) -> dict:
    """target の winner-detection.json を読み、該当行だけ差し替えて書き戻す。"""
    existing = None
    if target.is_file():
        try:
            existing = json.loads(target.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError) as error:
            raise SystemExit(f"既存の winner-detection.json を読めません: {target} ({error})")
    payload = merge_into_payload(existing, records, skipped)
    target.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    return payload


def build_payload(batch_dir: Path, video: Path | None, records: list[dict],
                  skipped: list[dict], min_determined_ratio: float) -> dict:
    determined = sum(1 for row in records if row.get("winner") is not None)
    ratio = determined / max(1, len(records))
    payload = {
        "schema": "tetris-winner-detection/v1",
        "batch": batch_dir.name,
        "source": str(video) if video else "",
        "engine": "end3-model",
        "models": {"scene": str(SCENE_MODEL), "endState": str(END_MODEL)},
        "config": {"fps": FPS, "scale": [WIDTH, HEIGHT],
                   "windowBack": WINDOW_BACK, "windowForward": WINDOW_FORWARD,
                   "sceneEndMin": SCENE_END_MIN, "drawMin": DRAW_MIN,
                   "minDrawFrames": MIN_DRAW_FRAMES,
                   "confidenceOk": CONF_OK, "confidenceReview": CONF_REVIEW,
                   "minDeterminedRatio": min_determined_ratio,
                   "determinedRatio": round(ratio, 4),
                   "determinedMatches": determined, "totalMatches": len(records)},
        "matches": records,
    }
    if skipped:
        payload["skipped"] = skipped
    return payload


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--batch-dir", required=True)
    parser.add_argument("--video", default=None)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--output", default=None)
    parser.add_argument("--reference", default=None,
                        help="既存の winner-detection.json（精度比較用）")
    parser.add_argument("--only-matches", default=None,
                        help="処理する試合番号（例: 17,38）。未指定なら全試合")
    parser.add_argument("--merge", action="store_true",
                        help="既存の winner-detection.json の該当行だけを差し替える")
    parser.add_argument("--min-determined-ratio", type=float, default=MIN_DETERMINED_RATIO)
    args = parser.parse_args()

    only = parse_only_matches(args.only_matches)
    selective = bool(only or args.merge)

    batch_dir = Path(args.batch_dir)
    if not batch_dir.is_dir():
        batch_dir = BATCHES / args.batch_dir
    detection_path = batch_dir / "segments.matches.json"
    if not detection_path.is_file():
        raise SystemExit(f"segments.matches.json がありません: {detection_path}")
    detection = json.loads(detection_path.read_text(encoding="utf-8"))
    video = resolve_video(batch_dir, args.video)
    duration = float(detection.get("video", {}).get("duration_seconds") or 0.0)

    matches = detection.get("matches") or []
    if args.limit and not only:
        matches = matches[: args.limit]
    by_index: dict[int, dict] = {}
    for match in matches:
        index = match.get("index")
        if isinstance(index, int):
            by_index.setdefault(index, match)
    requested = only or sorted(by_index)
    targets = [by_index[index] for index in requested if index in by_index]
    missing = [index for index in requested if index not in by_index]

    video_ok = video is not None and video.is_file()
    if not video_ok and not selective:
        raise SystemExit(f"動画が見つかりません: {video}")

    started = time.time()
    print(f"{batch_dir.name}: {len(targets)} matches workers={args.workers} "
          f"video={video.name if video_ok else '(なし)'}", flush=True)
    if only:
        print(f"  対象試合: {','.join(str(index) for index in requested)}"
              f"（見つからない試合: {len(missing)}）", flush=True)
        if not args.merge:
            print("  注意: --merge 未指定のため、出力ファイルは対象試合だけになります",
                  flush=True)

    records: list[dict] = []
    if video_ok and targets:
        scene_session = ort.InferenceSession(
            str(SCENE_MODEL), providers=["CPUExecutionProvider"])
        end_session = ort.InferenceSession(
            str(END_MODEL), providers=["CPUExecutionProvider"])
        scene_input = scene_session.get_inputs()[0].name
        end_input = end_session.get_inputs()[0].name

        def job(match):
            return analyse_match_safe(scene_session, scene_input, end_session, end_input,
                                      video, match, duration)

        with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
            records = list(pool.map(job, targets))
    else:
        records = [skipped_record(match, "video-missing") for match in targets]
        if not video_ok:
            print(f"動画が無いため二重チェックを省略しました"
                  f"（{len(targets)}試合をスキップ）", flush=True)
    records.extend(skipped_record({"index": index}, "match-not-found") for index in missing)
    records.sort(key=lambda row: row["match"])
    skipped = [row for row in records if row.get("skipped")]

    target = Path(args.output) if args.output else batch_dir / "winner-detection.json"
    if args.merge:
        payload = merge_detection(target, records, skipped)
        config = payload.get("config") or {}
        saved_rows = {row["match"]: row for row in payload["matches"]}
        print(f"二重チェック（動画方式）: 差し替え {config.get('secondOpinionMatches', 0)}試合"
              f" / スキップ {config.get('secondOpinionSkipped', 0)}試合")
        for row in records:
            if row.get("skipped"):
                print(f"  スキップ match {row['match']}: {row.get('skipReason')}")
                continue
            saved = saved_rows.get(row["match"], {})
            previous = saved.get("previousWinner")
            note = f" ← 旧: {previous}" if previous is not None else ""
            print(f"  match {row['match']}: {row.get('winner')} "
                  f"({row.get('status')}, 信頼度 {row.get('confidence')}){note}")
        print(f"  saved {target}")
    else:
        payload = build_payload(batch_dir, video, records, skipped,
                                args.min_determined_ratio)
        target.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                          encoding="utf-8")
        p1 = sum(1 for row in records if row.get("winner") == "p1")
        p2 = sum(1 for row in records if row.get("winner") == "p2")
        draw = sum(1 for row in records if row.get("winner") == "draw")
        ok = sum(1 for row in records if row.get("status") == "ok")
        review = sum(1 for row in records if row.get("status") == "review")
        undetermined = sum(1 for row in records if row.get("status") == "undetermined")
        print(f"結果: 確定{ok} / 要確認{review} / 判定不能{undetermined}  "
              f"P1={p1} P2={p2} 引き分け={draw}")
        print(f"  決定率 {payload['config']['determinedMatches']}/{len(records)} = "
              f"{payload['config']['determinedRatio']:.1%}  {time.time() - started:.0f}s")
        for row in skipped:
            print(f"  スキップ match {row['match']}: {row.get('skipReason')}")
        print(f"  saved {target}")

    if args.reference:
        reference = json.loads(Path(args.reference).read_text(encoding="utf-8"))
        expected = {int(row["match"]): row.get("winner")
                    for row in reference.get("matches") or []}
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

    ratio = payload["config"].get("determinedRatio")
    if not selective and ratio is not None and ratio < args.min_determined_ratio:
        raise SystemExit(2)
    if selective and ratio is not None and ratio < args.min_determined_ratio:
        print("  （部分実行のため終了コード 2 は返しません）")


if __name__ == "__main__":
    main()
