"""PPT Lab のジョブ実行部。

1本の動画について「（URLなら）取得 → 解析 → 保存」を1スレッドで行い、
進捗を GET /api/jobs/<id> で読める形に保つ。解析は同時に1本だけ。
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import threading
import time
from pathlib import Path

VIDEO_EXTENSIONS = ('.mp4', '.mkv', '.webm', '.mov', '.avi', '.m4v', '.ts')

DOWNLOAD_SHARE = 30          # 取得が全体の何%か
ANALYSIS_SHARE = 70

DOWNLOAD_PROGRESS = re.compile(r'\[download\]\s+([0-9.]+)%')
PROFILES_FILE = Path(__file__).with_name('profiles.json')


def load_profile(profile_id: str) -> dict | None:
    """profiles.json から解析プロファイルを読む（標準は空＝従来どおり）。"""
    try:
        data = json.loads(PROFILES_FILE.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError):
        return None
    for item in data.get('profiles') or []:
        if str(item.get('id')) == str(profile_id):
            if not any(item.get(key) for key in ('sceneModel', 'boardModel', 'settings')):
                return None
            return item
    return None


STUDY_PROGRESS = re.compile(r'study_progress=(\d+)\s+status=(.*)')
STUDY_MANIFEST = re.compile(r'study_manifest=(.+)')


class JobError(Exception):
    """利用者に見せる失敗（日本語の理由つき）。"""


def _decode(raw: bytes) -> str:
    for encoding in ('utf-8', 'cp932'):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            continue
    return raw.decode('utf-8', errors='replace')


class JobManager:
    def __init__(self, store, data_dir: Path, tools_dir: Path, runtime_dir: Path):
        self.store = store
        self.data = Path(data_dir)
        self.tools = Path(tools_dir)
        self.runtime = Path(runtime_dir)
        self.media = self.data / 'media'
        self.media.mkdir(parents=True, exist_ok=True)
        self._jobs: dict[str, dict] = {}
        self._lock = threading.Lock()
        self._worker = threading.Lock()   # 解析は同時1本

    # ---- 参照 -------------------------------------------------------------
    def get(self, job_id: str) -> dict | None:
        with self._lock:
            job = self._jobs.get(job_id)
            return dict(job) if job else None

    def latest_for_video(self, video_id: str) -> dict | None:
        with self._lock:
            candidates = [job for job in self._jobs.values() if job['videoId'] == video_id]
            if not candidates:
                return None
            return dict(max(candidates, key=lambda job: job['updatedAt']))

    def _persist(self, job_id: str, status: str, message: str) -> None:
        """DB の jobs 行も更新する（再起動後に queued のまま残らないように）。"""
        try:
            self.store.set_job_status(job_id, status, message)
        except Exception:
            pass

    def _update(self, job_id: str, **fields) -> None:
        with self._lock:
            job = self._jobs.get(job_id)
            if not job:
                return
            job.update(fields)
            job['updatedAt'] = time.time()
            terminal = job.get('status') in ('done', 'failed')
            if terminal or time.time() - job.get('_persistedAt', 0) > 2:
                job['_persistedAt'] = time.time()
                self._persist(job_id, job.get('status', ''), job.get('message', ''))

    # ---- 起動 -------------------------------------------------------------
    def start(self, video_id: str, source: str) -> str:
        job_id = self.store.add_job('video', 'queued', {'videoId': video_id, 'source': source})
        with self._lock:
            self._jobs[job_id] = {'id': job_id, 'status': 'queued', 'progress': 0,
                                  'message': '待機中', 'error': '', 'videoId': video_id,
                                  'source': source, 'updatedAt': time.time()}
        threading.Thread(target=self._run, args=(job_id, video_id, source),
                         daemon=True).start()
        return job_id

    # ---- 本体 -------------------------------------------------------------
    def _run(self, job_id: str, video_id: str, source: str) -> None:
        try:
            with self._worker:
                self._update(job_id, status='running', message='準備しています…')
                self.store.set_video_status(video_id, 'analyzing')
                video_path = self._resolve_source(job_id, video_id, source)
                manifest = self._analyze(job_id, video_path)
                matches = self._match_count(manifest)
                self.store.set_video_status(video_id, 'needs-names', matches,
                                             str(manifest))
                self._update(job_id, status='done', progress=100,
                             message=f'{matches}試合を検出しました', manifest=str(manifest))
        except JobError as error:
            self._fail(job_id, video_id, str(error))
        except Exception as error:                       # 想定外も握りつぶさない
            self._fail(job_id, video_id, f'予期しない失敗: {error}')

    def _fail(self, job_id: str, video_id: str, message: str) -> None:
        self.store.set_video_status(video_id, 'failed')
        self._update(job_id, status='failed', error=message, message=message)

    # ---- 取得 -------------------------------------------------------------
    def _resolve_source(self, job_id: str, video_id: str, source: str) -> Path:
        if source.lower().startswith(('http://', 'https://')):
            return self._download(job_id, video_id, source)
        path = Path(source)
        if not path.is_file():
            raise JobError(f'動画ファイルが見つかりません: {source}')
        return path

    def _download(self, job_id: str, video_id: str, url: str) -> Path:
        # STUDY が管理している本体（runtime\tools\yt-dlp\yt-dlp.exe）を最優先で使う。
        managed = self.runtime / 'tools' / 'yt-dlp' / 'yt-dlp.exe'
        candidates = [str(managed)] if managed.is_file() else []
        found = shutil.which('yt-dlp') or shutil.which('yt-dlp.exe')
        if found:
            candidates.append(found)
        local = self.tools / 'bin' / 'yt-dlp.exe'
        if local.is_file():
            candidates.append(str(local))
        if not candidates:
            raise JobError('yt-dlp が見つかりません。README の手順で導入してください。')
        yt_dlp = candidates[0]
        target = self.media / video_id
        target.mkdir(parents=True, exist_ok=True)
        command = [yt_dlp, '--newline', '--no-playlist', '--no-warnings',
                   '--merge-output-format', 'mkv',
                   '-f', 'bv*+ba/b',
                   '-o', str(target / '%(title)s [%(id)s].%(ext)s')]
        ffmpeg = self.runtime / 'bin' / 'ffmpeg.exe'
        if ffmpeg.is_file():
            command += ['--ffmpeg-location', str(ffmpeg.parent)]
        command.append(url)
        self._update(job_id, message='動画を取得しています…')
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        tail: list[str] = []
        assert process.stdout is not None
        for raw in process.stdout:
            line = _decode(raw).strip()
            if line:
                tail = (tail + [line])[-8:]
            found = DOWNLOAD_PROGRESS.search(line)
            if found:
                percent = min(100.0, float(found.group(1)))
                self._update(job_id, progress=percent * DOWNLOAD_SHARE / 100,
                             message=f'動画を取得しています… {percent:.0f}%')
        code = process.wait()
        if code != 0:
            raise JobError('動画を取得できませんでした。' +
                           ((' ' + tail[-1]) if tail else ''))
        files = [p for p in target.iterdir()
                 if p.is_file() and p.suffix.lower() in VIDEO_EXTENSIONS]
        if not files:
            raise JobError('取得した動画ファイルが見つかりません。')
        return max(files, key=lambda p: p.stat().st_size)

    # ---- 解析 -------------------------------------------------------------
    def _analyze(self, job_id: str, video: Path) -> Path:
        script = self.tools / 'run-ppt-stream-analysis.ps1'
        if not script.is_file():
            raise JobError('解析スクリプトが見つかりません（tools\\run-ppt-stream-analysis.ps1）。')
        if not (self.runtime / 'bin' / 'TetrisVideoRecovery_stream.exe').is_file():
            raise JobError('解析エンジンが見つかりません（runtime\\bin\\TetrisVideoRecovery_stream.exe）。')
        command = ['powershell.exe', '-NoLogo', '-NoProfile', '-STA', '-ExecutionPolicy', 'Bypass',
                   '-File', str(script), '-VideoPath', str(video), '-MachineReadable']
        # 解析プロファイル（モデルの切り替え）。未指定なら従来どおり。
        try:
            profile = load_profile(self.store.settings().get('analysisProfile', 'standard'))
        except Exception:
            profile = None
        if profile:
            if profile.get('sceneModel'):
                command += ['-SceneModelPath', str(profile['sceneModel'])]
            if profile.get('boardModel'):
                command += ['-BoardModelPath', str(profile['boardModel'])]
            if profile.get('settings'):
                command += ['-SettingsPath', str(profile['settings'])]
        self._update(job_id, progress=DOWNLOAD_SHARE, message='配信を解析しています…')
        process = subprocess.Popen(command, cwd=str(self.tools), stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT,
                                   creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        manifest: Path | None = None
        tail: list[str] = []
        assert process.stdout is not None
        for raw in process.stdout:
            line = _decode(raw).strip()
            if line:
                tail = (tail + [line])[-12:]
            found = STUDY_PROGRESS.search(line)
            if found:
                percent = min(100, int(found.group(1)))
                self._update(job_id,
                             progress=DOWNLOAD_SHARE + percent * ANALYSIS_SHARE / 100,
                             message=found.group(2).strip() or '配信を解析しています…')
            found = STUDY_MANIFEST.search(line)
            if found:
                manifest = Path(found.group(1).strip())
        code = process.wait()
        if code != 0 or manifest is None or not manifest.is_file():
            joined = ' '.join(tail)
            if 'no confirmed matches' in joined or 'matches=0' in joined:
                raise JobError('試合を検出できませんでした。動画が短いか、試合区間が見つかりません。')
            if 'exit code 4' in joined:
                raise JobError('試合を検出できませんでした。別の動画で試してください。')
            raise JobError('解析に失敗しました。' + ((' ' + tail[-1]) if tail else ''))
        return manifest

    @staticmethod
    def _match_count(manifest: Path) -> int:
        try:
            data = json.loads(manifest.read_text(encoding='utf-8-sig'))
        except (OSError, json.JSONDecodeError):
            return 0
        matches = data.get('matches')
        return len(matches) if isinstance(matches, list) else 0
