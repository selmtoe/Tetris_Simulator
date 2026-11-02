"""解析済みバッチから「名前確認」用のデータを作る。

やること:
  1. batch-results.json の各試合について、autosave の先頭から playerNames と crop を読む
  2. ffmpeg で試合が始まってすぐの1フレームを切り抜き、P1/P2の名前画像を作る
  3. 画像を 142x11 の輝度グリッドに落として指紋にし、相関 0.985 以上を同じ組にする
  4. 結果を JSON と PNG キャッシュとして保存する（2回目以降は再計算しない）

解析直後（jobs.py）にも save_name_crops を呼び、動画を消す前に名前画像を
name-crops/<指紋>.png へ残す。指紋はここでの計算と同じなので、動画が消えた後も
「同じ画像＝同じ人」でプレイヤー名をまとめられる。
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import re
import subprocess
import tempfile
import threading
import time
import unicodedata
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

THUMB_WIDTH = 142
THUMB_HEIGHT = 11
SAME_IMAGE_THRESHOLD = 0.985
STRIP_WIDTH = 700
PREFIX_BYTES = 64 * 1024
# 試合ごとの画像指紋を残すファイル（解析結果と同じフォルダに置く）。
FINGERPRINT_STATE_FILE = 'name-fingerprints.json'
FINGERPRINT_SCHEMA = 'ppt-name-fingerprints/v1'


def name_crop_dir() -> Path:
    """確定した名前の画像を置く場所（server.py の CROP_DIR と同じ）。

    %LOCALAPPDATA%\\PPTLab\\name-crops/<指紋>.png。名前の記憶（name_memory）と
    同じ指紋を鍵にするので、保存形式はこれまでと変わらない。
    """
    return Path(os.environ.get('LOCALAPPDATA') or Path.home()) / 'PPTLab' / 'name-crops'


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


def load_manifest(manifest: Path) -> dict:
    """解析結果（manifest）を読む。読めなければ ValueError。"""
    try:
        return json.loads(Path(manifest).read_text(encoding='utf-8-sig'))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f'解析結果を読めません: {error}') from error


def read_match_names(manifest: Path, root: dict | None = None) -> list[dict]:
    """manifest の各試合から、名前テキストと crop（切り抜き座標）を取り出す。

    autosave があればそこから読み、無ければ manifest の player_names を使う。
    crop は [x, y, w, h]。名前板の位置は試合が変わっても同じことが多いが、
    画像そのものは試合ごとに違うので、同一性の判定には動画から切り出す必要がある。
    """
    manifest = Path(manifest)
    base = manifest.parent
    if root is None:
        try:
            root = json.loads(manifest.read_text(encoding='utf-8-sig'))
        except (OSError, json.JSONDecodeError):
            return []
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
                crop_value = value.get('crop') if isinstance(value, dict) else None
                if crop_value and not names.get(side + 'crop'):
                    names[side + 'crop'] = crop_value
        combined = str((item.get('simulator') or {}).get('combined', ''))
        rows.append({
            'index': index,
            'start': float(item.get('source_range', {}).get('start_seconds', 0) or 0),
            'end': float(item.get('source_range', {}).get('end_seconds', 0) or 0),
            'p1': names['p1'], 'p2': names['p2'],
            'hash': combined.split('#', 1)[1] if '#' in combined else '',
            'p1crop': names['p1crop'], 'p2crop': names['p2crop']
        })
    return rows


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
    """名前板を切り抜く時刻（試合開始から数秒）。

    名前板は試合が始まると表示される。実測（50分・720pの配信35試合）では、
    開始+2秒なら35試合すべてで同じ名前板が写ったが、+4秒だと演出（カットイン）
    が重なって別の絵になる試合があった（同じ人の画像が別グループに分かれた）。
    動画から切り出す位置は名前板の座標に合わせて extract_strip が拡大する。
    """
    duration = max(0.0, end - start)
    seconds = max(0.0, start + min(max(duration * 0.2, 0.5), 2.0))
    if end > start:
        seconds = min(seconds, max(start, end - 0.05))
    return seconds


def extract_strip(ffmpeg: Path, video: Path, seconds: float, box, output: Path) -> bool:
    output.parent.mkdir(parents=True, exist_ok=True)
    left, top, right, bottom = box
    width, height = right - left, bottom - top
    scale = STRIP_WIDTH
    # crop の座標は 1920x1080 を基準に記録されている（720p の配信でも同じ値が
    # 入る）。実フレームの大きさに合わせて引き伸ばす。1080p では従来と同じ
    # 切り抜き（iw=1920, ih=1080 なので式の値がそのまま座標になる）。
    vf = (f'crop=iw*{width}/1920:ih*{height}/1080:'
          f'iw*{left}/1920:ih*{top}/1080,'
          f'scale={scale}:-1:flags=lanczos')
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


def player_thumb(image, region):
    """strip の1人分を 142x11 の輝度画像にする（指紋と表示用PNGの元）。"""
    from PIL import Image
    return image.crop(region).resize((THUMB_WIDTH, THUMB_HEIGHT),
                                     Image.LANCZOS).convert('L')


def thumb_fingerprint(thumb):
    """142x11 の画素から（正規化ベクトル, 指紋ハッシュ）を作る。

    指紋は画素そのものの SHA-256 先頭32桁。名前の記憶（name_memory）と
    name-crops/<指紋>.png がこの値で結び付いているので、変えてはいけない。
    """
    pixels = [int(value) for value in thumb.getdata()]
    digest = hashlib.sha256(bytes(pixels)).hexdigest()[:32]
    return _normalize([float(value) for value in pixels]), digest


def image_vector(image):
    """保存済みの名前画像から、比較用の正規化ベクトルを作る（無ければ None）。"""
    from PIL import Image
    thumb = image.convert('L').resize((THUMB_WIDTH, THUMB_HEIGHT), Image.LANCZOS)
    return _normalize([float(value) for value in thumb.getdata()])


def crop_vector(path: Path):
    """name-crops の PNG（142x11）を、比較用の正規化ベクトルにする。"""
    try:
        from PIL import Image
        with Image.open(Path(path)) as image:
            image.load()
            return image_vector(image)
    except (OSError, ValueError, ImportError):
        return None


def name_region(box, crop, width: int, height: int):
    """strip 画像（box を STRIP_WIDTH に拡大したもの）上の、1人分の矩形。"""
    box_width = box[2] - box[0]
    box_height = box[3] - box[1]
    scale_x = width / box_width
    scale_y = height / box_height
    left = int(round((crop[0] - box[0]) * scale_x))
    top = int(round((crop[1] - box[1]) * scale_y))
    right = left + int(round(crop[2] * scale_x))
    bottom = top + int(round(crop[3] * scale_y))
    return (max(0, left), max(0, top),
            min(width, max(left + 2, right)),
            min(height, max(top + 2, bottom)))


def cut_match_names(ffmpeg: Path, source: Path, strip_path: Path, box, seconds: float,
                    p1crop, p2crop):
    """1試合ぶんの P1/P2 の名前画像を切り出す。

    戻り値は ((画像, 正規化ベクトル, 指紋) または None, 同 P2)。strip は
    strip_path にキャッシュする（無ければ ffmpeg で切り出す）。
    """
    if not strip_path.is_file():
        if not extract_strip(ffmpeg, source, seconds, box, strip_path):
            return None, None
    from PIL import Image
    try:
        with Image.open(strip_path) as image:
            image.load()
            width, height = image.size
            thumbs = []
            for crop in (p1crop, p2crop):
                thumb = player_thumb(image, name_region(box, crop, width, height))
                thumbs.append((thumb, *thumb_fingerprint(thumb)))
            return thumbs[0], thumbs[1]
    except OSError:
        return None, None


# ---------------------------------------------------------------------------
# 同じ人をまとめる（動画一覧・名前確認で同じ規則を使う）
# ---------------------------------------------------------------------------
def fold_name(text: str) -> str:
    """表記ゆれを畳むためのキー（NFKC・記号除去・小文字化）。"""
    normalized = unicodedata.normalize('NFKC', str(text or ''))
    return re.sub(r'[\s_\-・.,:;!?"\'()（）\[\]]+', '', normalized).lower()


def most_common(texts) -> str:
    """いちばん多く出た表記（空文字は数えない。同数なら先に出たもの）。"""
    counts: dict[str, int] = {}
    first: dict[str, int] = {}
    for order, text in enumerate(texts):
        text = str(text or '').strip()
        if not text:
            continue
        counts[text] = counts.get(text, 0) + 1
        first.setdefault(text, order)
    if not counts:
        return ''
    return max(counts, key=lambda text: (counts[text], -first[text]))


def representative_name(texts) -> str:
    """組の代表名。OCR が空だった試合も1票として数える。

    空が最多なら '' を返す（画面は「（OCR空）」と出す）。空と表記が同数なら、
    名前として使える表記のほうを選ぶ。
    """
    counts: dict[str, int] = {}
    first: dict[str, int] = {}
    for order, raw in enumerate(texts):
        text = str(raw or '').strip()
        counts[text] = counts.get(text, 0) + 1
        first.setdefault(text, order)
    if not counts:
        return ''
    return max(counts, key=lambda text: (counts[text], bool(text), -first[text]))


def group_name_images(rows, vector_of, threshold: int | float = SAME_IMAGE_THRESHOLD):
    """名前画像の指紋を相関でまとめ、同じ人どうしを1組にする。

    rows は read_match_names() / save_name_crops() が返す辞書の列
    （'index'・'p1'・'p2'・'p1fingerprint'・'p2fingerprint' を見る）。
    vector_of は「指紋 → 比較用ベクトル（無ければ None）」を返す関数。

    戻り値は (groups, text_only)。
      groups:    [{'vector', 'texts': [...], 'members': [...]}]
                 texts は OCR が空でない表記だけ（動画一覧の名前はこちらを使う）。
                 members は空も含む（画面は「（OCR空）」として1組に数える）。
      text_only: 画像が取れない試合（古い解析・動画が無い等）。従来どおり文字で扱う。

    人数の上限は設けない（組数を理由に画像のまとまりを捨てない）。
    """
    groups: list[dict] = []
    text_only: list[dict] = []
    for row in rows:
        index = int(row.get('index') or 0)
        for side in ('p1', 'p2'):
            text = str(row.get(side) or '').strip()
            fingerprint = str(row.get(side + 'fingerprint') or '')
            vector = vector_of(fingerprint) if fingerprint else None
            member = {'index': index, 'side': side, 'text': text, 'fingerprint': fingerprint}
            if vector is None:
                # 画像が取れない試合は、従来どおり文字で扱う
                text_only.append(member)
                continue
            assigned = None
            best = threshold
            for group in groups:
                score = _correlation(vector, group['vector'])
                if score >= best:
                    best = score
                    assigned = group
            if assigned is None:
                assigned = {'vector': vector, 'texts': [], 'members': []}
                groups.append(assigned)
            if text:
                assigned['texts'].append(text)
            assigned['members'].append(member)
    return groups, text_only


def name_groups(rows, vector_of, threshold: int | float = SAME_IMAGE_THRESHOLD) -> list[dict]:
    """名前確認画面の1エントリずつ（＝同じ人のまとまり）を作る。

    画像の指紋でまとめた組に、画像が取れなかった試合を文字で足す。戻り値の各要素:
      order          … 見つかった順
      kind           … 'image'（画像でまとめた組） / 'text'（文字だけ）
      group          … 1始まりの組番号（'text' は 0）
      texts          … 空でない OCR 表記（出た順・重複あり）
      members        … [{'index','side','text','fingerprint'}]
      representative … 代表名（空なら「（OCR空）」）
    """
    groups, text_only = group_name_images(rows, vector_of, threshold)
    entries = [{'order': order, 'kind': 'image', 'group': order + 1,
                'texts': list(group['texts']), 'members': list(group['members'])}
               for order, group in enumerate(groups)]
    # 画像が無かった試合は、文字が一致すればその組に足し、合わなければ別名にする
    folded: dict[str, dict] = {}
    for entry in entries:
        for text in entry['texts']:
            folded.setdefault(fold_name(text), entry)
    extras: list[dict] = []
    for member in text_only:
        text = member['text']
        # 名前が空の試合は、従来どおり P1/P2 ごとに1つへまとめる
        key = fold_name(text) if text else f'{member["side"]}:unknown'
        target = folded.get(key)
        if target is not None:
            target['members'].append(member)
            if text:
                target['texts'].append(text)
            continue
        entry = {'order': len(groups) + len(extras), 'kind': 'text', 'group': 0,
                 'texts': [text] if text else [], 'members': [member]}
        extras.append(entry)
        folded[key] = entry
    entries += extras
    entries.sort(key=lambda item: (-len(item['texts']), item['order']))
    for entry in entries:
        entry['representative'] = representative_name([m['text'] for m in entry['members']])
    return entries


def group_payload(entries, by_index) -> list[dict]:
    """名前確認画面に返す「1組 = 1エントリ」の一覧（試合数の多い順）。

    entries は name_groups() の戻り値、by_index は {試合番号: row}。
    1組につき1件だけ返すので、OCR の表記ゆれが別カードに分かれることはない。
    """
    payload: list[dict] = []
    for entry in entries:
        indexes: list[int] = []
        counts: dict[str, int] = {}
        fingerprints: list[str] = []
        image = ''
        guess = ''
        remembered = False
        override = False
        for member in entry['members']:
            index = int(member['index'])
            if index not in indexes:
                indexes.append(index)
            text = member['text']
            if text:
                counts[text] = counts.get(text, 0) + 1
            if member['fingerprint'] and member['fingerprint'] not in fingerprints:
                fingerprints.append(member['fingerprint'])
            row = by_index.get(index) or {}
            side = member['side']
            member_guess = str(row.get(side + 'Guess') or '')
            if member_guess and not guess:
                guess = member_guess
            if member_guess and member_guess != text:
                remembered = True
            if row.get(side + 'Override'):
                override = True
            if not image:
                image = str(row.get(side + 'Image') or '')
        payload.append({
            'key': f'g{entry["group"]}' if entry['group'] else f't{entry["order"] + 1}',
            'kind': entry['kind'],
            # 代表名は組の中で最多の OCR 表記（空が最多なら ''＝画面は「（OCR空）」）
            'name': entry['representative'],
            'variants': [{'text': text, 'count': counts[text]} for text in counts],
            'guess': guess,
            'remembered': remembered,
            'override': override,
            'fingerprints': fingerprints,
            'matches': len(indexes),
            'records': [{'index': int(m['index']), 'side': m['side']} for m in entry['members']],
            'sides': sorted({m['side'] for m in entry['members']}),
            'image': image,
        })
    payload.sort(key=lambda item: -item['matches'])
    return payload


class NameReview:
    """1本の動画に対する名前確認データ（試合ごとの画像・組・名前）。"""

    def __init__(self, cache_root: Path, ffmpeg: Path | None, recall=None,
                 recall_text=None, recall_visual=None, crop_dir: Path | None = None):
        self.cache_root = Path(cache_root)
        self.ffmpeg = ffmpeg
        self.recall = recall
        self.recall_text = recall_text
        self.recall_visual = recall_visual
        # 解析直後に切り出した名前画像（name-crops/<指紋>.png）の置き場。
        # ここに PNG があれば、元の動画が消えていても同じ人をまとめられる。
        self.crop_dir = Path(crop_dir) if crop_dir else name_crop_dir()
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
        for row in read_match_names(manifest, root):
            extra = winners.get(int(row['index']), {})
            rows.append({
                'index': row['index'], 'start': row['start'], 'end': row['end'],
                'p1': row['p1'], 'p2': row['p2'],
                'winner': extra.get('winner', ''),
                'winnerStatus': extra.get('winnerStatus', '')
            })
        data = {'matches': rows, 'source': str(root.get('source', '')), 'warning': ''}
        with self._lock:
            self._light[video_id] = data
        return data
    def _build(self, video_id: str, manifest: Path) -> dict:
        root = load_manifest(manifest)
        source = Path(str(root.get('source', '')))
        base = manifest.parent
        rows = []
        for row in read_match_names(manifest, root):
            rows.append({
                'index': row['index'], 'start': row['start'], 'end': row['end'],
                'p1': row['p1'], 'p2': row['p2'],
                'hash': row.get('hash', ''),
                'p1crop': row['p1crop'], 'p2crop': row['p2crop']
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

        self.cache_dir(video_id)
        for row in rows:
            row['p1Image'] = ''
            row['p2Image'] = ''
            row['p1Group'] = 0
            row['p2Group'] = 0

        # 名前画像の指紋を用意する。解析直後に残した name-fingerprints.json と
        # name-crops/<指紋>.png があれば、元の動画が消えていても同じ人をまとめられる
        # （無ければ動画から切り出して保存する＝次回からは動画が不要になる）。
        try:
            cropped = save_name_crops(manifest, self.ffmpeg, self.crop_dir)
        except Exception:
            cropped = {}
        prints = {int(item['index']): item for item in cropped.get('rows') or []}
        vectors: dict[str, tuple | None] = {}

        def vector_of(value: str):
            if not value:
                return None
            if value not in vectors:
                vectors[value] = crop_vector(Path(self.crop_dir) / f'{value}.png')
            return vectors[value]

        for row in rows:
            saved = prints.get(int(row['index'])) or {}
            for side in ('p1', 'p2'):
                fingerprint = str(saved.get(side + 'fingerprint') or '')
                row[side + 'fingerprint'] = fingerprint          # まとめる用（最後に消す）
                row[side + 'Fingerprint'] = fingerprint
                image = self.image_path(video_id, int(row['index']), side)
                if image.is_file():
                    row[side + 'Image'] = f"/api/videos/{video_id}/name-image/{row['index']}/{side}"
                elif fingerprint and (Path(self.crop_dir) / f'{fingerprint}.png').is_file():
                    # 動画を消した後でも、解析直後に残した名前画像を表示できる
                    row[side + 'Image'] = f'/api/name-crops/{fingerprint}.png'

        # 同じ人を1組にまとめる。第一キーは名前画像の指紋（P1/P2 をまたいでも同じ組）。
        entries = name_groups(rows, vector_of)
        by_index = {int(row['index']): row for row in rows}
        for entry in entries:
            if not entry['group']:
                continue
            for member in entry['members']:
                row = by_index.get(int(member['index']))
                if row is not None:
                    row[member['side'] + 'Group'] = entry['group']

        for row in rows:
            # 以前に確定した名前があれば、OCRより優先して候補にする
            for side in ('p1', 'p2'):
                guess = ''
                fingerprint = row.get(side + 'Fingerprint')
                if fingerprint and self.recall:
                    try:
                        guess = self.recall(fingerprint) or ''
                    except Exception:
                        guess = ''
                if not guess and self.recall_text:
                    # 指紋は画素の完全一致なので配信が変わると外れる。
                    # OCRが読んだ文字が既知の名前と一致するなら、それを使う。
                    try:
                        guess = self.recall_text(row.get(side, '')) or ''
                    except Exception:
                        guess = ''
                if not guess and self.recall_visual:
                    # 配信内のグルーピングと同じ「画像の類似度」で、過去の画像とも照合する。
                    vector = vector_of(fingerprint)
                    if vector:
                        try:
                            guess = self.recall_visual(vector) or ''
                        except Exception:
                            guess = ''
                row[side + 'Guess'] = guess
            row.pop('p1crop', None)
            row.pop('p2crop', None)
            row.pop('p1fingerprint', None)
            row.pop('p2fingerprint', None)
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
        groups = group_payload(entries, by_index)
        warning = '' if any(row.get('p1Image') or row.get('p2Image') for row in rows) else \
            'ffmpeg か元動画が見つからないため、名前画像は表示できません'
        return {'matches': rows, 'source': str(source), 'warning': warning,
                'groups': groups}


# ---------------------------------------------------------------------------
# 解析直後の名前画像の切り出し
# ---------------------------------------------------------------------------
def _manifest_source(root: dict, manifest: Path) -> Path:
    """manifest が指す元動画のパス（相対なら manifest の隣から）。"""
    source = Path(str(root.get('source', '')))
    if str(source) not in ('', '.') and not source.is_absolute():
        source = Path(manifest).parent / source
    return source


def fingerprint_state_path(manifest: Path) -> Path:
    """試合ごとの画像指紋を残すファイル（解析結果と同じフォルダ）。"""
    return Path(manifest).parent / FINGERPRINT_STATE_FILE


def _source_identity(source: Path) -> dict:
    try:
        info = Path(source).stat()
    except OSError:
        return {}
    return {'path': str(source), 'bytes': int(info.st_size),
            'mtime_ns': int(info.st_mtime_ns)}


def _read_fingerprint_state(manifest: Path) -> dict:
    try:
        data = json.loads(fingerprint_state_path(manifest).read_text(encoding='utf-8-sig'))
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(data, dict) or data.get('schema') != FINGERPRINT_SCHEMA:
        return {}
    return data


def _state_matches(state: dict) -> dict[int, dict]:
    """state の matches を {index: {'p1': {...}, 'p2': {...}}} に直す。"""
    result: dict[int, dict] = {}
    for entry in state.get('matches') or []:
        try:
            index = int(entry.get('index', 0))
        except (TypeError, ValueError):
            continue
        sides = {}
        for side in ('p1', 'p2'):
            value = entry.get(side)
            if isinstance(value, dict) and value.get('fingerprint'):
                sides[side] = {'text': str(value.get('text') or ''),
                               'fingerprint': str(value['fingerprint'])}
        result[index] = sides
    return result


def _same_path(left: Path, right: Path) -> bool:
    try:
        return (os.path.normcase(os.path.abspath(str(left)))
                == os.path.normcase(os.path.abspath(str(right))))
    except OSError:
        return False


def _state_usable(state: dict, source: Path, rows: list[dict], crop_dir: Path) -> bool:
    """前回の切り出しをそのまま使えるか。

    動画がまだあるときは中身（サイズと更新時刻）も確かめる。動画が消えて
    いても、解析時に切り出した指紋と PNG が残っていればそのまま使う。
    """
    if not state:
        return False
    saved = state.get('source') or {}
    if not saved.get('path') or not _same_path(Path(str(saved['path'])), source):
        return False
    current = _source_identity(source)
    if current:
        if int(saved.get('bytes') or -1) != current['bytes']:
            return False
        if int(saved.get('mtime_ns') or -1) != current['mtime_ns']:
            return False
    matches = _state_matches(state)
    for row in rows:
        entry = matches.get(int(row['index'])) or {}
        found = False
        for side in ('p1', 'p2'):
            fingerprint = (entry.get(side) or {}).get('fingerprint')
            if not fingerprint:
                continue
            if not (Path(crop_dir) / f'{fingerprint}.png').is_file():
                return False
            found = True
        if not found:
            return False
    return True


def _write_fingerprint_state(manifest: Path, source: Path, rows: list[dict]) -> bool:
    payload = {
        'schema': FINGERPRINT_SCHEMA,
        'manifest': Path(manifest).name,
        'source': _source_identity(source) or {'path': str(source)},
        'createdAt': int(time.time()),
        'matches': [
            {'index': int(row['index']),
             'p1': {'text': row.get('p1', ''),
                    'fingerprint': row.get('p1fingerprint', '')},
             'p2': {'text': row.get('p2', ''),
                    'fingerprint': row.get('p2fingerprint', '')}}
            for row in rows
        ]
    }
    target = fingerprint_state_path(manifest)
    pending = target.with_name(target.name + '.writing')
    try:
        pending.write_text(json.dumps(payload, ensure_ascii=False, indent=1),
                           encoding='utf-8')
        os.replace(pending, target)
        return True
    except OSError:
        try:
            pending.unlink()
        except OSError:
            pass
        return False


def _save_crop(image, crop_dir: Path, fingerprint: str) -> str:
    """名前画像を <crop_dir>/<指紋>.png に保存する。

    戻り値は 'saved'（新しく書いた）/ 'existing'（既にある）/ 'failed'（書けない）。
    書き込み中に落ちても壊れた PNG が残らないよう、一時ファイルに書いてから置き換える。
    """
    target = Path(crop_dir) / f'{fingerprint}.png'
    if target.is_file():
        return 'existing'
    pending = target.with_name(f'{fingerprint}.{os.getpid()}.'
                               f'{threading.get_ident()}.writing')
    try:
        Path(crop_dir).mkdir(parents=True, exist_ok=True)
        image.save(pending, 'PNG')
        os.replace(pending, target)
        return 'saved'
    except OSError:
        try:
            pending.unlink()
        except OSError:
            pass
        return 'failed'


def save_name_crops(manifest: Path, ffmpeg: Path | None, crop_dir: Path,
                    video: Path | None = None) -> dict:
    """全試合の P1/P2 の名前画像を切り出し、name-crops に保存する。

    解析が終わって動画を消す**前**に呼ぶこと。動画を消した後でも、残した
    指紋（name-fingerprints.json）と PNG だけで同じ人をまとめられる。

    - 指紋は NameReview と同じ（142x11 の画素の SHA-256 先頭32桁）
    - 保存先は <crop_dir>/<指紋>.png。既にあれば書かない（冪等）
    - 失敗しても例外にせず、結果を辞書で返す（解析そのものは止めない）
    """
    manifest = Path(manifest)
    result = {'rows': [], 'attempted': 0, 'saved': 0, 'skipped': 0, 'missing': 0,
              'failed': 0, 'error': '', 'state': ''}
    try:
        root = load_manifest(manifest)
    except ValueError as error:
        result['error'] = str(error)
        return result
    rows = read_match_names(manifest, root)
    result['rows'] = rows
    targets = [row for row in rows if plate_box(row['p1crop'], row['p2crop'])]
    result['attempted'] = len(targets)
    crop_dir = Path(crop_dir)
    source = Path(video) if video else _manifest_source(root, manifest)
    state = _read_fingerprint_state(manifest)
    if _state_usable(state, source, targets, crop_dir):
        # 前回切り出した指紋がそのまま使える（動画が消えていてもよい）。
        matches = _state_matches(state)
        for row in rows:
            entry = matches.get(int(row['index'])) or {}
            for side in ('p1', 'p2'):
                fingerprint = (entry.get(side) or {}).get('fingerprint')
                if fingerprint:
                    row[side + 'fingerprint'] = fingerprint
                    result['skipped'] += 1
        result['state'] = str(fingerprint_state_path(manifest))
        return result
    if not targets:
        return result
    if ffmpeg is None or not Path(ffmpeg).is_file():
        result['error'] = 'ffmpeg が見つかりません'
        return result
    if not source.is_file():
        result['error'] = f'元の動画が見つかりません: {source}'
        return result

    counters = {'saved': 0, 'skipped': 0, 'missing': 0, 'failed': 0}
    guard = threading.Lock()

    def work(row):
        box = plate_box(row['p1crop'], row['p2crop'])
        if not box:
            with guard:
                counters['missing'] += 2
            return
        try:
            with tempfile.TemporaryDirectory(prefix='ppt-name-crop-') as temp:
                p1, p2 = cut_match_names(
                    ffmpeg, source, Path(temp) / 'plate.png', box,
                    frame_seconds(row['start'], row['end']),
                    row['p1crop'], row['p2crop'])
        except OSError:
            p1 = p2 = None
        for side, thumb in (('p1', p1), ('p2', p2)):
            if not thumb:
                with guard:
                    counters['missing'] += 1
                continue
            image, _vector, fingerprint = thumb
            row[side + 'fingerprint'] = fingerprint
            outcome = _save_crop(image, crop_dir, fingerprint)
            with guard:
                counters['skipped' if outcome == 'existing' else outcome] += 1

    workers = min(4, max(1, (os.cpu_count() or 2) // 2))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        list(pool.map(work, targets))
    result['saved'] = counters['saved']
    result['skipped'] = counters['skipped']
    result['missing'] = counters['missing']
    result['failed'] = counters['failed']
    if counters['failed']:
        # 書けなかった画像は次回もう一度試す（解析そのものは止めない）。
        result['error'] = (f'名前画像を {counters["failed"]} 枚保存できませんでした: '
                           f'{crop_dir}')
    if any(row.get('p1fingerprint') or row.get('p2fingerprint') for row in rows):
        # 次回（動画を消した後）はこの記録だけで同じ人をまとめられる。
        if _write_fingerprint_state(manifest, source, rows):
            result['state'] = str(fingerprint_state_path(manifest))
    return result


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