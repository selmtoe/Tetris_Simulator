"""解析済みバッチから「名前確認」用のデータを作る。

やること:
  1. batch-results.json の各試合について、autosave の先頭から playerNames と crop を読む
  2. ffmpeg で試合開始直後の1フレームを切り抜き、P1/P2の名前画像を作る
  3. 画像を 142x11 の輝度グリッドに落として指紋にし、相関 0.985 以上を同じ組にする
  4. 結果を JSON と PNG キャッシュとして保存する（2回目以降は再計算しない）
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import subprocess
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

THUMB_WIDTH = 142
THUMB_HEIGHT = 11
SAME_IMAGE_THRESHOLD = 0.985
STRIP_WIDTH = 700
PREFIX_BYTES = 64 * 1024


def _read_prefix(path: Path) -> str:
    with path.open('rb') as handle:
        return handle.read(PREFIX_BYTES).decode('utf-8', errors='replace')


def read_player_names(autosave: Path) -> dict:
    """autosave の先頭だけを読み、playerNames を取り出す（C++版と同じ手順）。"""
    empty = {'p1': '', 'p2': '', 'p1crop': None, 'p2crop': None}
    try:
        prefix = _read_prefix(autosave)
    except OSError:
        return empty
    key = prefix.find('"playerNames"')
    if key < 0:
        return empty
    colon = prefix.find(':', key + len('"playerNames"'))
    if colon < 0:
        return empty
    begin = prefix.find('{', colon + 1)
    if begin < 0:
        return empty
    depth = 0
    in_string = False
    escaped = False
    end = -1
    for index in range(begin, len(prefix)):
        char = prefix[index]
        if in_string:
            if escaped:
                escaped = False
            elif char == '\\':
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
        elif char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0:
                end = index + 1
                break
    if end < 0:
        return empty
    try:
        names = json.loads(prefix[begin:end])
    except json.JSONDecodeError:
        return empty
    crop = lambda side: (names.get(side) or {}).get('crop')
    return {
        'p1': str((names.get('p1') or {}).get('text', '')),
        'p2': str((names.get('p2') or {}).get('text', '')),
        'p1crop': crop('p1'), 'p2crop': crop('p2')
    }


def _valid_crop(value) -> bool:
    return (isinstance(value, list) and len(value) == 4 and
            all(isinstance(v, (int, float)) and math.isfinite(v) for v in value) and
            value[0] >= 0 and value[1] >= 0 and value[2] >= 8 and value[3] >= 6)


def plate_box(p1, p2):
    """P1とP2の切り抜きをまとめて取るための外接矩形。"""
    if not _valid_crop(p1) or not _valid_crop(p2):
        return None
    left = int(round(min(p1[0], p2[0])))
    top = int(round(min(p1[1], p2[1])))
    right = int(round(max(p1[0] + p1[2], p2[0] + p2[2])))
    bottom = int(round(max(p1[1] + p1[3], p2[1] + p2[3])))
    if right - left < 32 or bottom - top < 8 or right - left > 3840:
        return None
    return (left, top, right, bottom)


def frame_seconds(start: float, end: float) -> float:
    duration = max(0.0, end - start)
    seconds = max(0.0, start + min(max(duration * 0.2, 0.5), 4.0))
    if end > start:
        seconds = min(seconds, max(start, end - 0.05))
    return seconds


def extract_strip(ffmpeg: Path, video: Path, seconds: float, box, output: Path) -> bool:
    output.parent.mkdir(parents=True, exist_ok=True)
    left, top, right, bottom = box
    width, height = right - left, bottom - top
    scale = STRIP_WIDTH
    vf = f'crop={width}:{height}:{left}:{top},scale={scale}:-1:flags=lanczos'
    command = [str(ffmpeg), '-hide_banner', '-loglevel', 'error', '-nostdin',
               '-ss', f'{seconds:.3f}', '-i', str(video),
               '-map', '0:v:0', '-an', '-sn', '-dn', '-frames:v', '1',
               '-vf', vf, '-y', str(output)]
    try:
        result = subprocess.run(command, capture_output=True, timeout=120,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except (OSError, subprocess.SubprocessError):
        return False
    return result.returncode == 0 and output.is_file() and output.stat().st_size > 0


def _normalize(values):
    mean = sum(values) / len(values)
    variance = sum((v - mean) ** 2 for v in values) / len(values)
    deviation = math.sqrt(variance)
    if deviation < 1.0:
        return None
    return [(v - mean) / deviation for v in values]


def _correlation(left, right) -> float:
    if not left or not right or len(left) != len(right):
        return 0.0
    return sum(a * b for a, b in zip(left, right)) / len(left)


def _slice(image, region, output: Path):
    """strip から1人分を切り出し、表示用PNGと（正規化ベクトル, 指紋ハッシュ）を返す。"""
    from PIL import Image
    strip = image.crop(region).resize((THUMB_WIDTH, THUMB_HEIGHT), Image.LANCZOS).convert('L')
    strip.save(output, 'PNG')
    pixels = [int(value) for value in strip.getdata()]
    digest = hashlib.sha256(bytes(pixels)).hexdigest()[:32]
    return _normalize([float(value) for value in pixels]), digest


class NameReview:
    """1本の動画に対する名前確認データ（試合ごとの画像・組・名前）。"""

    def __init__(self, cache_root: Path, ffmpeg: Path | None, recall=None):
        self.cache_root = Path(cache_root)
        self.ffmpeg = ffmpeg
        self.recall = recall
        self._lock = threading.Lock()
        self._built: dict[str, dict] = {}
        self._light: dict[str, dict] = {}

    def cache_dir(self, video_id: str) -> Path:
        folder = self.cache_root / video_id
        folder.mkdir(parents=True, exist_ok=True)
        return folder

    def image_path(self, video_id: str, index: int, side: str) -> Path:
        return self.cache_dir(video_id) / f'match_{index:03d}_{side}.png'

    # ---- 構築 -------------------------------------------------------------
    def forget(self, video_id: str) -> None:
        """名前を保存した後、次回に作り直せるようキャッシュを捨てる。"""
        with self._lock:
            self._built.pop(video_id, None)
            self._light.pop(video_id, None)

    def load(self, video_id: str, manifest: Path) -> dict:
        with self._lock:
            cached = self._built.get(video_id)
        if cached:
            return cached
        data = self._build(video_id, manifest)
        with self._lock:
            self._built[video_id] = data
        return data

    def light(self, video_id: str, manifest: Path) -> dict:
        """画像を作らない軽量版（試合一覧・フィルタ用）。"""
        with self._lock:
            cached = self._light.get(video_id)
        if cached:
            return cached
        try:
            root = json.loads(manifest.read_text(encoding='utf-8-sig'))
        except (OSError, json.JSONDecodeError) as error:
            raise ValueError(f'解析結果を読めません: {error}') from error
        base = manifest.parent
        winners: dict[int, dict] = {}
        for candidate in (base / 'winner-detection.json', base / 'winners.json'):
            if not candidate.is_file():
                continue
            try:
                payload = json.loads(candidate.read_text(encoding='utf-8-sig'))
            except (OSError, json.JSONDecodeError):
                continue
            for entry in payload.get('matches') or []:
                try:
                    number = int(entry.get('match', 0))
                except (TypeError, ValueError):
                    continue
                winners[number] = {'winner': str(entry.get('winner') or ''),
                                   'winnerStatus': str(entry.get('status') or '')}
            break
        rows = []
        for item in root.get('matches') or []:
            index = int(item.get('index', 0))
            autosave = Path(str(item.get('autosave', '')))
            if not autosave.is_absolute():
                autosave = base / autosave
            names = read_player_names(autosave) if autosave.is_file() else \
                {'p1': '', 'p2': '', 'p1crop': None, 'p2crop': None}
            if not names.get('p1') and not names.get('p2'):
                # autosave が無い（取り込んだ動画など）は manifest の名前を使う
                for side in ('p1', 'p2'):
                    value = (item.get('player_names') or {}).get(side)
                    text_value = value.get('text') if isinstance(value, dict) else value
                    if text_value:
                        names[side] = str(text_value)
            extra = winners.get(index, {})
            rows.append({
                'index': index,
                'start': float(item.get('source_range', {}).get('start_seconds', 0) or 0),
                'end': float(item.get('source_range', {}).get('end_seconds', 0) or 0),
                'p1': names['p1'], 'p2': names['p2'],
                'winner': extra.get('winner', ''),
                'winnerStatus': extra.get('winnerStatus', '')
            })
        data = {'matches': rows, 'source': str(root.get('source', '')), 'warning': ''}
        with self._lock:
            self._light[video_id] = data
        return data
    def _build(self, video_id: str, manifest: Path) -> dict:
        try:
            root = json.loads(manifest.read_text(encoding='utf-8-sig'))
        except (OSError, json.JSONDecodeError) as error:
            raise ValueError(f'解析結果を読めません: {error}') from error
        source = Path(str(root.get('source', '')))
        base = manifest.parent
        rows = []
        for item in root.get('matches') or []:
            index = int(item.get('index', 0))
            autosave = Path(str(item.get('autosave', '')))
            if not autosave.is_absolute():
                autosave = base / autosave
            names = read_player_names(autosave) if autosave.is_file() else \
                {'p1': '', 'p2': '', 'p1crop': None, 'p2crop': None}
            if not names.get('p1') and not names.get('p2'):
                # autosave が無い（取り込んだ動画など）は manifest の名前を使う
                for side in ('p1', 'p2'):
                    value = (item.get('player_names') or {}).get(side)
                    text_value = value.get('text') if isinstance(value, dict) else value
                    if text_value:
                        names[side] = str(text_value)
            combined = str((item.get('simulator') or {}).get('combined', ''))
            rows.append({
                'index': index,
                'start': float(item.get('source_range', {}).get('start_seconds', 0) or 0),
                'end': float(item.get('source_range', {}).get('end_seconds', 0) or 0),
                'p1': names['p1'], 'p2': names['p2'],
                'hash': combined.split('#', 1)[1] if '#' in combined else '',
                'p1crop': names['p1crop'], 'p2crop': names['p2crop']
            })
        # 勝敗（winner-detection.json があれば試合ごとに付ける）
        winners: dict[int, dict] = {}
        for candidate in (base / 'winner-detection.json', base / 'winners.json'):
            if not candidate.is_file():
                continue
            try:
                payload = json.loads(candidate.read_text(encoding='utf-8-sig'))
            except (OSError, json.JSONDecodeError):
                continue
            for entry in payload.get('matches') or []:
                try:
                    number = int(entry.get('match', 0))
                except (TypeError, ValueError):
                    continue
                winners[number] = {'winner': str(entry.get('winner') or ''),
                                   'winnerStatus': str(entry.get('status') or '')}
            break
        for row in rows:
            extra = winners.get(int(row['index']), {})
            row['winner'] = extra.get('winner', '')
            row['winnerStatus'] = extra.get('winnerStatus', '')
        if not rows:
            return {'matches': [], 'source': str(source), 'warning': '試合がありません'}

        if not self.ffmpeg or not self.ffmpeg.is_file() or not source.is_file():
            for row in rows:
                row['p1Group'] = row['p2Group'] = 0
                row['p1Image'] = row['p2Image'] = ''
            return {'matches': rows, 'source': str(source),
                    'warning': 'ffmpeg か元動画が見つからないため、名前画像は表示できません'}

        folder = self.cache_dir(video_id)
        for row in rows:
            row['p1Image'] = ''
            row['p2Image'] = ''
            row['p1Group'] = 0
            row['p2Group'] = 0

        def work(row):
            box = plate_box(row['p1crop'], row['p2crop'])
            if not box:
                return row, None, None
            strip_path = folder / f"match_{row['index']:03d}_strip.png"
            if not strip_path.is_file():
                if not extract_strip(self.ffmpeg, source,
                                     frame_seconds(row['start'], row['end']), box, strip_path):
                    return row, None, None
            from PIL import Image
            try:
                with Image.open(strip_path) as image:
                    image.load()
                    width, height = image.size
                    box_width = box[2] - box[0]
                    box_height = box[3] - box[1]
                    scale_x = width / box_width
                    scale_y = height / box_height

                    def region(crop):
                        left = int(round((crop[0] - box[0]) * scale_x))
                        top = int(round((crop[1] - box[1]) * scale_y))
                        right = left + int(round(crop[2] * scale_x))
                        bottom = top + int(round(crop[3] * scale_y))
                        return (max(0, left), max(0, top),
                                min(width, max(left + 2, right)),
                                min(height, max(top + 2, bottom)))

                    p1 = _slice(image, region(row['p1crop']),
                                self.image_path(video_id, row['index'], 'p1'))
                    p2 = _slice(image, region(row['p2crop']),
                                self.image_path(video_id, row['index'], 'p2'))
                    return row, p1, p2
            except OSError:
                return row, None, None

        workers = min(4, max(1, (os.cpu_count() or 2) // 2))
        with ThreadPoolExecutor(max_workers=workers) as pool:
            for row, p1, p2 in pool.map(work, rows):
                if p1:
                    row['p1Image'] = f"/api/videos/{video_id}/name-image/{row['index']}/p1"
                    row['_p1'], row['p1Fingerprint'] = p1
                if p2:
                    row['p2Image'] = f"/api/videos/{video_id}/name-image/{row['index']}/p2"
                    row['_p2'], row['p2Fingerprint'] = p2

        self._assign(rows, '_p1', 'p1Group')
        self._assign(rows, '_p2', 'p2Group')
        for row in rows:
            row.pop('_p1', None)
            row.pop('_p2', None)
            row.pop('p1crop', None)
            row.pop('p2crop', None)
            # 以前に確定した名前があれば、OCRより優先して候補にする
            for side in ('p1', 'p2'):
                guess = ''
                fingerprint = row.get(side + 'Fingerprint')
                if fingerprint and self.recall:
                    try:
                        guess = self.recall(fingerprint) or ''
                    except Exception:
                        guess = ''
                row[side + 'Guess'] = guess
        # 同じ組なら同じ相手なので、1件でも記憶があれば組全体に広げる
        for side, group_key in (('p1', 'p1Group'), ('p2', 'p2Group')):
            remembered = {}
            for row in rows:
                group = row.get(group_key) or 0
                if group and row.get(side + 'Guess'):
                    remembered.setdefault(group, row[side + 'Guess'])
            for row in rows:
                group = row.get(group_key) or 0
                if group and not row.get(side + 'Guess') and group in remembered:
                    row[side + 'Guess'] = remembered[group]
        return {'matches': rows, 'source': str(source), 'warning': ''}

    @staticmethod
    def _assign(rows, key, target):
        representatives = []
        for row in rows:
            fingerprint = row.get(key)
            if not fingerprint:
                continue
            assigned = 0
            best = SAME_IMAGE_THRESHOLD
            for group, reference in enumerate(representatives, start=1):
                score = _correlation(fingerprint, reference)
                if score >= best:
                    best = score
                    assigned = group
            if not assigned:
                representatives.append(fingerprint)
                assigned = len(representatives)
            row[target] = assigned

# ---------------------------------------------------------------------------
# 名前の書き戻し
# ---------------------------------------------------------------------------
def _find_player_names(text: str):
    key = text.find('"playerNames"')
    if key < 0:
        return -1, -1
    colon = text.find(':', key + len('"playerNames"'))
    if colon < 0:
        return -1, -1
    begin = text.find('{', colon + 1)
    if begin < 0:
        return -1, -1
    depth = 0
    in_string = False
    escaped = False
    for index in range(begin, len(text)):
        char = text[index]
        if in_string:
            if escaped:
                escaped = False
            elif char == '\\':
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
        elif char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0:
                return begin, index + 1
    return -1, -1


def write_player_names(autosave: Path, p1: str | None, p2: str | None) -> bool:
    """autosave の playerNames だけを書き換える（他のバイト列は触らない）。"""
    try:
        raw = autosave.read_bytes()
    except OSError:
        return False
    bom = raw.startswith(b'\xef\xbb\xbf')
    try:
        text = raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        return False
    begin, end = _find_player_names(text)
    if begin < 0:
        return False
    try:
        names = json.loads(text[begin:end])
    except json.JSONDecodeError:
        return False
    if p1 is not None:
        names.setdefault('p1', {})['text'] = p1
    if p2 is not None:
        names.setdefault('p2', {})['text'] = p2
    updated = text[:begin] + json.dumps(names, ensure_ascii=False, separators=(',', ':')) + text[end:]
    payload = updated.encode('utf-8')
    if bom:
        payload = b'\xef\xbb\xbf' + payload
    pending = autosave.with_suffix(autosave.suffix + '.writing')
    try:
        pending.write_bytes(payload)
        pending.replace(autosave)
    except OSError:
        pending.unlink(missing_ok=True)
        return False
    return True


def apply_names(manifest: Path, updates: list[dict]) -> dict:
    """{index, p1, p2} の一覧を autosave に書き戻す。"""
    try:
        root = json.loads(manifest.read_text(encoding='utf-8-sig'))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f'解析結果を読めません: {error}') from error
    base = manifest.parent
    by_index = {}
    for item in root.get('matches') or []:
        by_index[int(item.get('index', 0))] = item
    changed = 0
    manifest_touched = False
    for update in updates:
        index = int(update.get('index', 0))
        item = by_index.get(index)
        if not item:
            continue
        p1 = (update.get('p1') or '').strip() or None
        p2 = (update.get('p2') or '').strip() or None
        if p1 is None and p2 is None:
            continue
        autosave = Path(str(item.get('autosave', '')))
        if not autosave.is_absolute():
            autosave = base / autosave
        if autosave.is_file():
            if write_player_names(autosave, p1, p2):
                changed += 1
            continue
        # autosave が無い（取り込んだ動画）は manifest の player_names を更新する
        names = item.setdefault('player_names', {})
        for side, value in (('p1', p1), ('p2', p2)):
            if value is None:
                continue
            current = names.get(side)
            if isinstance(current, dict):
                current['text'] = value
            else:
                names[side] = {'text': value}
        manifest_touched = True
        changed += 1
    if manifest_touched:
        try:
            manifest.write_text(json.dumps(root, ensure_ascii=False, indent=2), encoding='utf-8')
        except OSError as error:
            raise ValueError(f'解析結果に書き込めません: {error}') from error
    return {'changed': changed}