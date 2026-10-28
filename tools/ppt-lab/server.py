"""PPT Lab のローカルサーバ（127.0.0.1 のみ）。

起動:  python server.py [--port 8765] [--data <dir>] [--open]
画面:  web/index.html をそのまま配信し、/api/* で JSON を返す。
"""
from __future__ import annotations

import argparse
import base64
import json
import mimetypes
import os
import re
import shutil
import subprocess
import sys
import threading
import unicodedata
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

CROP_DIR = Path(os.environ.get('LOCALAPPDATA') or Path.home()) / 'PPTLab' / 'name-crops'

VIDEO_EXTENSIONS = {'.mp4', '.mkv', '.webm', '.mov', '.avi', '.ts', '.m4v', '.flv'}
from urllib.parse import parse_qs, unquote, urlsplit

APP_ROOT = Path(__file__).resolve().parent
TOOLS_ROOT = APP_ROOT.parent
RUNTIME = TOOLS_ROOT / 'video-recovery-runtime'
WEB_ROOT = APP_ROOT / 'web'
DEFAULT_DATA = Path(os.environ.get('LOCALAPPDATA', Path.home())) / 'PPTLab'
VERSION = '0.1.0'
STARTED_AT = time.time()

sys.path.insert(0, str(APP_ROOT))
from store import Store  # noqa: E402
from jobs import JobManager  # noqa: E402
from namereview import NameReview, read_player_names, apply_names  # noqa: E402

STORE: Store | None = None
JOBS: JobManager | None = None
REVIEW: NameReview | None = None


def engine_path() -> Path:
    return RUNTIME / 'bin' / 'TetrisVideoRecovery_stream.exe'


_ENV_CACHE: dict = {'at': 0.0, 'value': None}


def find_ffmpeg() -> Path | None:
    for candidate in (RUNTIME / 'bin' / 'ffmpeg.exe', Path(shutil.which('ffmpeg') or '')):
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


def gpu_available() -> bool:
    """NVDEC と CUDA のONNXが使えるかを、DLLとドライバの有無で判定する。

    nvcuvid.dll は NVIDIA ドライバが System32 に入れるものなので、
    runtime\\bin ではなくシステム側を見る。
    """
    bin_dir = RUNTIME / 'bin'
    needed = ('onnxruntime_providers_cuda.dll', 'cudnn64_9.dll')
    if not all((bin_dir / name).is_file() for name in needed):
        return False
    system32 = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'
    if not (system32 / 'nvcuvid.dll').is_file():
        return False
    try:
        result = subprocess.run(['nvidia-smi', '-L'], capture_output=True, timeout=5,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        return result.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


PICK_SCRIPT = r"""
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
# ブラウザの後ろに隠れないよう、最前面のダミー親ウィンドウを所有者にする
$owner = New-Object System.Windows.Forms.Form
$owner.TopMost = $true
$owner.ShowInTaskbar = $false
$owner.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
$owner.StartPosition = 'CenterScreen'
$owner.Size = New-Object System.Drawing.Size(1, 1)
$owner.Opacity = 0
$owner.Show()
$d = New-Object System.Windows.Forms.OpenFileDialog
$d.Title = '解析する動画を選ぶ'
$d.Filter = '動画ファイル|*.mp4;*.mkv;*.webm;*.mov;*.avi;*.m4v;*.ts|すべてのファイル|*.*'
$d.RestoreDirectory = $true
$result = $d.ShowDialog($owner)
$owner.Close()
if ($result -eq [System.Windows.Forms.DialogResult]::OK) {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    [Console]::Write($d.FileName)
}
"""


_PLAYER_CACHE: dict[str, tuple[float, list[str]]] = {}


def _fold_name(text: str) -> str:
    """表記ゆれを畳むためのキー（NFKC・記号除去・小文字化）。"""
    normalized = unicodedata.normalize('NFKC', str(text or ''))
    return re.sub(r'[\s_\-・.,:;!?"\'()（）\[\]]+', '', normalized).lower()


def video_players(manifest: str) -> list[str]:
    """登場プレイヤー名。名前を確認済みならその名前を使い、OCR の揺れは畳む。"""
    if not manifest:
        return []
    path = Path(manifest)
    try:
        stamp = path.stat().st_mtime
    except OSError:
        return []
    cached = _PLAYER_CACHE.get(manifest)
    if cached and cached[0] == stamp:
        return cached[1]
    try:
        data = json.loads(path.read_text(encoding='utf-8-sig'))
    except (OSError, json.JSONDecodeError):
        return []

    # 1) 確認済みの名前（autosave）を優先する
    confirmed: list[str] = []
    for match in data.get('matches') or []:
        autosave = match.get('autosave')
        if not autosave:
            continue
        saved = path.parent / str(autosave)
        try:
            payload = read_player_names(saved)
        except Exception:
            continue
        for side in ('p1', 'p2'):
            text = str(payload.get(side) or '').strip()
            if text and text not in confirmed:
                confirmed.append(text)
    if confirmed:
        _PLAYER_CACHE[manifest] = (stamp, confirmed)
        return confirmed

    # 2) 未確認なら OCR 名を使う（表記ゆれは同じ人として畳む）
    counter: dict[str, list] = {}
    for match in data.get('matches') or []:
        for side in ('p1', 'p2'):
            value = (match.get('player_names') or {}).get(side)
            text = value.get('text') if isinstance(value, dict) else value
            text = str(text or '').strip()
            if not text:
                continue
            key = _fold_name(text)
            entry = counter.setdefault(key, [0, {}])
            entry[0] += 1
            entry[1][text] = entry[1].get(text, 0) + 1
    names = []
    for _key, (count, variants) in sorted(counter.items(), key=lambda item: -item[1][0]):
        best = max(variants.items(), key=lambda item: item[1])[0]
        names.append(best)
    _PLAYER_CACHE[manifest] = (stamp, names)
    return names

def decode_state_hash(url: str) -> dict | None:
    """F/ の URL のハッシュ（base64url のJSON）を取り出す。"""
    try:
        fragment = str(url).split('#', 1)[1]
    except IndexError:
        return None
    try:
        raw = base64.urlsafe_b64decode(fragment + '=' * (-len(fragment) % 4))
        return json.loads(raw.decode('utf-8'))
    except (ValueError, KeyError, UnicodeDecodeError):
        return None


def lab_replay(video: dict, index: int) -> dict | None:
    """取り込んだ動画（公開Lab由来）のリプレイを、保存してあるURLから組み立てる。"""
    manifest = video.get('manifest') or ''
    if not manifest:
        return None
    try:
        data = json.loads(Path(manifest).read_text(encoding='utf-8-sig'))
    except (OSError, json.JSONDecodeError):
        return None
    match = next((item for item in data.get('matches') or []
                  if int(item.get('index') or 0) == index), None)
    if not match:
        return None
    simulator = match.get('labSimulator') or {}
    p1 = str(simulator.get('p1') or '')
    p2 = str(simulator.get('p2') or '')
    if not p1 and not p2:
        return None
    state = decode_state_hash(p1 or p2)
    return {
        'urls': {'combined': p1 or p2, 'p1': p1, 'p2': p2},
        'simulatorData': state or {},
        'imported': True
    }


def replay_file(manifest: Path, index: int) -> Path | None:
    """試合のリプレイJSON（simulator フォルダ内の *.json）を返す。"""
    try:
        data = json.loads(manifest.read_text(encoding='utf-8-sig'))
    except (OSError, json.JSONDecodeError):
        return None
    for item in data.get('matches') or []:
        if int(item.get('index', 0)) != index:
            continue
        combined = str((item.get('simulator') or {}).get('combined', ''))
        folder = Path(combined).parent
        if not folder.is_dir():
            return None
        files = sorted(folder.glob('*.json'))
        return files[0] if files else None
    return None

def pick_video_file() -> str:
    """ローカルアプリなので、サーバー側でOSのファイル選択ダイアログを開く。"""
    try:
        result = subprocess.run(['powershell.exe', '-NoProfile', '-STA', '-Command', PICK_SCRIPT],
                                capture_output=True, timeout=600,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except (OSError, subprocess.SubprocessError):
        return ''
    return result.stdout.decode('utf-8-sig', errors='replace').strip()


class Handler(BaseHTTPRequestHandler):
    server_version = 'PPTLab/' + VERSION
    protocol_version = 'HTTP/1.1'

    # ---- 返却ヘルパ -------------------------------------------------------
    def send_json(self, value, status: int = 200) -> None:
        body = json.dumps(value, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def send_media(self, source: Path) -> None:
        """ローカル動画を Range 対応で返す（<video> のシークに必要）。"""
        size = source.stat().st_size
        start, end = 0, size - 1
        status = 200
        header = self.headers.get('Range') or ''
        match = re.match(r'bytes=(\d*)-(\d*)', header)
        if match and size:
            if match.group(1):
                start = int(match.group(1))
                end = int(match.group(2)) if match.group(2) else size - 1
            elif match.group(2):
                start = max(0, size - int(match.group(2)))
            if start >= size:
                self.send_response(416)
                self.send_header('Content-Range', f'bytes */{size}')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return None
            end = min(end, size - 1)
            status = 206
        length = end - start + 1
        mime, _ = mimetypes.guess_type(source.name)
        self.send_response(status)
        self.send_header('Content-Type', mime or 'application/octet-stream')
        self.send_header('Content-Length', str(length))
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('X-Content-Type-Options', 'nosniff')
        if status == 206:
            self.send_header('Content-Range', f'bytes {start}-{end}/{size}')
        self.end_headers()
        with source.open('rb') as handle:
            handle.seek(start)
            remaining = length
            while remaining > 0:
                chunk = handle.read(min(1 << 20, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)
        return None
    def send_error_json(self, message: str, status: int = 400) -> None:
        self.send_json({'error': message}, status)

    def read_json(self) -> dict:
        length = int(self.headers.get('Content-Length') or 0)
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        try:
            value = json.loads(raw.decode('utf-8'))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ValueError('送信内容を読み取れませんでした') from error
        if not isinstance(value, dict):
            raise ValueError('送信内容の形式が正しくありません')
        return value

    # ---- ルーティング -----------------------------------------------------
    def do_GET(self) -> None:
        path = unquote(urlsplit(self.path).path)
        try:
            if path.startswith('/api/'):
                return self.api_get(path)
            return self.serve_static(path)
        except Exception as error:  # 想定外でも落とさず、画面に理由を出す
            return self.send_error_json(str(error), 500)

    def do_POST(self) -> None:
        path = unquote(urlsplit(self.path).path)
        try:
            return self.api_post(path)
        except ValueError as error:
            return self.send_error_json(str(error), 400)
        except Exception as error:
            return self.send_error_json(str(error), 500)

    def do_DELETE(self) -> None:
        path = unquote(urlsplit(self.path).path)
        try:
            return self.api_delete(path)
        except ValueError as error:
            return self.send_error_json(str(error), 400)
        except Exception as error:
            return self.send_error_json(str(error), 500)

    def api_get(self, path: str) -> None:
        assert STORE is not None
        if path == '/api/health':
            return self.send_json({'ok': True, 'version': VERSION, 'startedAt': STARTED_AT})
        if path == '/api/env':
            if _ENV_CACHE['value'] is None or time.time() - _ENV_CACHE['at'] > 60:
                _ENV_CACHE['value'] = {'engine': engine_path().is_file(),
                                       'ffmpeg': find_ffmpeg() is not None,
                                       'gpu': gpu_available(),
                                       'python': sys.version.split()[0]}
                _ENV_CACHE['at'] = time.time()
            return self.send_json(_ENV_CACHE['value'])
        if path == '/api/videos':
            videos = STORE.videos()
            for video in videos:
                players = video_players(video.get('manifest', ''))
                video['players'] = players
                auto = ['動画解析'] + [name for name in players if name]
                stored = STORE.tags_for(video['id'])
                # 自動タグは必ず付く。手動タグはその後ろに足す。
                STORE.materialize_tags(video['id'], auto)
                merged = list(auto)
                for name in stored:
                    if name not in merged:
                        merged.append(name)
                video['tags'] = merged
                video['autoTags'] = auto
                job = JOBS.latest_for_video(video['id']) if JOBS else None
                if job:
                    video['jobId'] = job['id']
                    video['progress'] = round(job['progress'])
                    video['message'] = job['message']
                    video['error'] = job['error']
            return self.send_json({'videos': videos})
        if path == '/api/tetofu':
            return self.send_json({'tetofu': STORE.tetofu()})
        if path == '/api/settings':
            return self.send_json({'settings': STORE.settings()})
        if path == '/api/names/mappings':
            rows = STORE.name_mappings()
            grouped = {}
            for item in rows:
                name = str(item['name'])
                entry = grouped.setdefault(name, {'name': name, 'fingerprints': [],
                                                  'hits': 0, 'updatedAt': item['updatedAt'],
                                                  'image': ''})
                entry['fingerprints'].append(item['fingerprint'])
                entry['hits'] += int(item.get('hits') or 0)
                if item['updatedAt'] > entry['updatedAt']:
                    entry['updatedAt'] = item['updatedAt']
                if not entry['image']:
                    crop = CROP_DIR / (str(item['fingerprint']) + '.png')
                    if crop.is_file():
                        entry['image'] = f"/api/name-crops/{item['fingerprint']}.png"
            mappings = sorted(grouped.values(), key=lambda item: -item['hits'])
            return self.send_json({'mappings': mappings})
        if path == '/api/tags':
            return self.send_json({'tags': STORE.tags()})
        if path == '/api/jobs':
            return self.send_json({'jobs': STORE.jobs()})
        if path.startswith('/api/videos/') and '/replay/' in path:
            rest = path[len('/api/videos/'):]
            video_id, _, tail = rest.partition('/replay/')
            index_text = tail.strip('/')
            video = STORE.video(video_id)
            if not video or not video['manifest']:
                return self.send_error_json('解析結果がありません', 404)
            if not index_text.isdigit():
                return self.send_error_json('試合の指定が正しくありません', 400)
            replay = replay_file(Path(video['manifest']), int(index_text))
            if not replay or not replay.is_file():
                # 取り込んだ動画は、保存してあるLabのURLから組み立てる
                fallback = lab_replay(video, int(index_text))
                if fallback:
                    return self.send_json(fallback)
                return self.send_error_json('リプレイが見つかりません', 404)
            body = replay.read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'max-age=3600')
            self.end_headers()
            self.wfile.write(body)
            return None
        if path.startswith('/api/videos/') and path.endswith('/media'):
            video_id = path[len('/api/videos/'):-len('/media')]
            video = STORE.video(video_id)
            if not video:
                return self.send_error_json('動画が見つかりません', 404)
            source = Path(video['source'])
            if source.suffix.lower() not in VIDEO_EXTENSIONS:
                return self.send_error_json('動画ファイルではありません', 400)
            if not source.is_file():
                return self.send_error_json('動画ファイルが見つかりません', 404)
            return self.send_media(source)
        if path.startswith('/api/videos/') and path.endswith('/matches'):
            video_id = path[len('/api/videos/'):-len('/matches')]
            video = STORE.video(video_id)
            if not video:
                return self.send_error_json('動画が見つかりません', 404)
            if not video['manifest'] or not Path(video['manifest']).is_file():
                return self.send_error_json('解析結果がまだありません', 409)
            query = parse_qs(urlsplit(self.path).query)
            if query.get('light', [''])[0] == '1':
                return self.send_json(REVIEW.light(video_id, Path(video['manifest'])))
            data = REVIEW.load(video_id, Path(video['manifest']))
            overrides = STORE.name_overrides_for(video_id)
            if overrides:
                for row in data.get('matches', []):
                    for side in ('p1', 'p2'):
                        fingerprint = row.get(side + 'Fingerprint')
                        if fingerprint and fingerprint in overrides:
                            row[side + 'Guess'] = overrides[fingerprint]
                            row[side + 'Override'] = True
            return self.send_json(data)
        if path.startswith('/api/videos/') and '/name-image/' in path:
            rest = path[len('/api/videos/'):]
            video_id, _, tail = rest.partition('/name-image/')
            index_text, _, side = tail.partition('/')
            if side not in ('p1', 'p2') or not index_text.isdigit():
                return self.send_error_json('画像の指定が正しくありません', 400)
            image = REVIEW.image_path(video_id, int(index_text), side)
            if not image.is_file():
                return self.send_error_json('画像がありません', 404)
            body = image.read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', 'image/png')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'max-age=3600')
            self.end_headers()
            self.wfile.write(body)
            return None
        if path.startswith('/api/name-crops/'):
            name = path[len('/api/name-crops/'):]
            if not name.endswith('.png') or '/' in name or '\\' in name:
                return self.send_error_json('画像の指定が正しくありません', 400)
            image = CROP_DIR / name
            if not image.is_file():
                return self.send_error_json('画像がありません', 404)
            body = image.read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', 'image/png')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'max-age=3600')
            self.end_headers()
            self.wfile.write(body)
            return None
        if path.startswith('/api/jobs/'):
            job_id = path.rsplit('/', 1)[-1]
            job = JOBS.get(job_id) if JOBS else None
            if not job:
                return self.send_error_json('ジョブが見つかりません', 404)
            return self.send_json(job)
        return self.send_error_json('不明なAPIです', 404)

    def api_post(self, path: str) -> None:
        assert STORE is not None
        if path == '/api/settings':
            body = self.read_json()
            for key, value in (body or {}).items():
                STORE.set_setting(str(key), str(value))
            return self.send_json({'ok': True, 'settings': STORE.settings()})
        if path == '/api/tags':
            body = self.read_json()
            return self.send_json(STORE.add_tag(str(body.get('name', ''))))
        if path == '/api/tags/apply':
            body = self.read_json()
            target = str(body.get('target', '')).strip()
            tags = body.get('tags')
            if not target:
                raise ValueError('対象がありません')
            if not isinstance(tags, list):
                raise ValueError('タグの形式が正しくありません')
            video = STORE.video(target)
            auto = set()
            if video:
                auto = {'動画解析', *[name for name in video_players(video.get('manifest', '')) if name]}
            manual = [str(tag) for tag in tags if str(tag) not in auto]
            STORE.set_tags(target, manual)
            merged = [str(tag) for tag in tags]
            return self.send_json({'tags': merged})
        if path == '/api/tetofu/tags':
            body = self.read_json()
            target = str(body.get('id', '')).strip()
            tags = body.get('tags')
            if not target:
                raise ValueError('対象のテト譜がありません')
            if not isinstance(tags, list):
                raise ValueError('タグの形式が正しくありません')
            return self.send_json({'tags': STORE.set_tags(target, [str(tag) for tag in tags])})
        if path == '/api/tetofu':
            body = self.read_json()
            return self.send_json(STORE.add_tetofu(str(body.get('kind', '')),
                                                   str(body.get('title', '')),
                                                   str(body.get('url', ''))))
        if path == '/api/videos':
            body = self.read_json()
            source = str(body.get('source', '')).strip()
            if not source:
                raise ValueError('動画のURLかファイルのパスを指定してください')
            if '://' not in source:
                candidate = Path(source)
                if not candidate.is_file():
                    raise ValueError('動画ファイルが見つかりません: ' + source[:200])
                if candidate.suffix.lower() not in VIDEO_EXTENSIONS:
                    raise ValueError('動画ファイルではありません: ' + candidate.suffix)
            title = source if '://' in source else Path(source).stem
            video = STORE.add_video(title, source)
            job_id = JOBS.start(video['id'], source) if JOBS else ''
            return self.send_json({'id': video['id'], 'jobId': job_id, 'status': 'queued'})
        if path.startswith('/api/videos/') and path.endswith('/title'):
            video_id = path[len('/api/videos/'):-len('/title')]
            video = STORE.video(video_id)
            if not video:
                return self.send_error_json('動画が見つかりません', 404)
            body = self.read_json()
            STORE.set_video_title(video_id, str(body.get('title', '')))
            return self.send_json({'id': video_id, 'title': str(body.get('title', '')).strip()})
        if path.startswith('/api/videos/') and path.endswith('/names'):
            video_id = path[len('/api/videos/'):-len('/names')]
            video = STORE.video(video_id)
            if not video:
                return self.send_error_json('動画が見つかりません', 404)
            body = self.read_json()
            updates = body.get('updates') or []
            overrides_input = body.get('overrides') or []
            if not isinstance(updates, list):
                raise ValueError('名前の形式が正しくありません')
            if not updates and not overrides_input:
                raise ValueError('更新する名前がありません')
            if not video['manifest'] or not Path(video['manifest']).is_file():
                return self.send_error_json('解析結果がまだありません', 409)
            cleaned = []
            for update in updates:
                try:
                    index = int(update.get('index'))
                except (TypeError, ValueError):
                    raise ValueError('試合番号が正しくありません')
                for side in ('p1', 'p2'):
                    value = str(update.get(side) or '')
                    if len(value) > 40:
                        raise ValueError('名前は40文字までです')
                cleaned.append({'index': index, 'p1': update.get('p1'), 'p2': update.get('p2')})
            if cleaned:
                result = apply_names(Path(video['manifest']), cleaned)
            else:
                result = {'changed': 0}
            for override in (body.get('overrides') or []):
                fingerprint = str(override.get('fingerprint') or '').strip()
                name = str(override.get('name') or '').strip()
                if fingerprint and name:
                    STORE.set_name_override(video_id, fingerprint, name)
                elif fingerprint:
                    STORE.clear_name_override(video_id, fingerprint)
            # 次回のために「画像の指紋 → 確定した名前」を覚える（OCRより優先して使う）
            try:
                rows = REVIEW.load(video_id, Path(video['manifest'])).get('matches', [])
                by_index = {int(row['index']): row for row in rows}
                for update in updates:
                    row = by_index.get(int(update.get('index', 0)))
                    if not row:
                        continue
                    for side in ('p1', 'p2'):
                        name = str(update.get(side) or '').strip()
                        fingerprint = row.get(side + 'Fingerprint')
                        if name and fingerprint:
                            STORE.remember_name(fingerprint, name)
                            try:
                                CROP_DIR.mkdir(parents=True, exist_ok=True)
                                source = REVIEW.image_path(video_id, int(update.get('index', 0)), side)
                                if source.is_file():
                                    shutil.copyfile(source, CROP_DIR / f'{fingerprint}.png')
                            except Exception:
                                pass
            except Exception:
                pass
            if result['changed'] > 0:
                STORE.set_video_status(video_id, 'ready')
            REVIEW.forget(video_id)
            status = 'ready' if result['changed'] > 0 else (video.get('status') or 'needs-names')
            return self.send_json({'changed': result['changed'], 'status': status})
        if path == '/api/videos/pick':
            picked = pick_video_file()
            if not picked:
                return self.send_json({'cancelled': True})
            return self.send_json({'path': picked, 'title': Path(picked).stem})
        return self.send_error_json('不明なAPIです', 404)

    def api_delete(self, path: str) -> None:
        assert STORE is not None
        if path == '/api/tags':
            body = self.read_json()
            STORE.delete_tag(str(body.get('name', '')))
            return self.send_json({'ok': True})
        if path == '/api/names/mappings':
            body = self.read_json()
            name = str(body.get('name', '')).strip()
            if name:
                removed = 0
                for item in STORE.name_mappings():
                    if str(item['name']) == name:
                        removed += 1 if STORE.forget_name(item['fingerprint']) else 0
                return self.send_json({'ok': True, 'removed': removed})
            fingerprint = str(body.get('fingerprint', '')).strip()
            if not fingerprint:
                raise ValueError('対象の指定がありません')
            return self.send_json({'ok': True, 'removed': STORE.forget_name(fingerprint)})
        if path.startswith('/api/videos/'):
            video_id = path[len('/api/videos/'):]
            if not STORE.video(video_id):
                return self.send_error_json('動画が見つかりません', 404)
            STORE.delete_video(video_id)
            return self.send_json({'ok': True, 'id': video_id})
        if path.startswith('/api/tetofu/'):
            tetofu_id = path[len('/api/tetofu/'):]
            if not STORE.tetofu_item(tetofu_id):
                return self.send_error_json('テト譜が見つかりません', 404)
            STORE.delete_tetofu(tetofu_id)
            return self.send_json({'ok': True, 'id': tetofu_id})
        return self.send_error_json('不明なAPIです', 404)

    # ---- 静的ファイル -----------------------------------------------------
    def serve_static(self, path: str) -> None:
        relative = 'index.html' if path in ('', '/') else path.lstrip('/')
        target = (WEB_ROOT / relative).resolve()
        if WEB_ROOT.resolve() not in target.parents and target != WEB_ROOT.resolve():
            return self.send_error_json('参照先が不正です', 403)
        if not target.is_file():
            return self.send_error_json('ファイルがありません', 404)
        body = target.read_bytes()
        mime, _ = mimetypes.guess_type(target.name)
        self.send_response(200)
        self.send_header('Content-Type', (mime or 'application/octet-stream') + '; charset=utf-8'
                         if (mime or '').startswith(('text/', 'application/json')) else
                         (mime or 'application/octet-stream'))
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args) -> None:
        if os.environ.get('PPT_LAB_VERBOSE'):
            sys.stderr.write('%s - %s\n' % (self.address_string(), fmt % args))


def main() -> int:
    global STORE, JOBS, REVIEW
    parser = argparse.ArgumentParser(description='PPT Lab local server')
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--data', type=Path, default=DEFAULT_DATA)
    parser.add_argument('--open', action='store_true')
    args = parser.parse_args()

    STORE = Store(args.data)
    STORE.reset_stuck_videos()
    JOBS = JobManager(STORE, args.data, TOOLS_ROOT, RUNTIME)
    REVIEW = NameReview(args.data / 'cache', find_ffmpeg(), STORE.recall_name)
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    url = f'http://127.0.0.1:{server.server_port}/'
    print(url, flush=True)
    if args.open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
