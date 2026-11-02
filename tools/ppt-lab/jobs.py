"""PPT Lab のジョブ実行部。

1本の動画について「（URLなら）取得 → 解析 → 保存」を1スレッドで行い、
進捗を GET /api/jobs/<id> で読める形に保つ。解析は同時に1本だけ。

解析に入った時点で、次に待っている URL 動画の取得だけを裏で始める（先読み）。
取得は CPU とネットワークだけを使うので、GPU を使う解析とは競合しない。
先読みは同時1本まで。失敗しても本処理には影響させず、そのジョブが走るときに
普通に取り直す。同じ動画を二重に取りに行かないよう、取得は動画単位のロックで
囲ってあり、保存先に完成済みのファイルがあれば取り直さずそれを使う。
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

# 解析直後に名前画像を切り出す（名前の記憶と同じ指紋で保存する）。
from namereview import name_crop_dir, save_name_crops
# 解析モデル（models\<id>\manifest.json）の一覧・導入・削除。
import models_registry

VIDEO_EXTENSIONS = ('.mp4', '.mkv', '.webm', '.mov', '.avi', '.m4v', '.ts')
AUDIO_EXTENSIONS = ('.m4a', '.webm', '.opus', '.aac', '.mp3', '.ogg', '.wav')

DOWNLOAD_SHARE = 30          # 取得が全体の何%か
ANALYSIS_SHARE = 70

DOWNLOAD_PROGRESS = re.compile(r'\[download\]\s+([0-9.]+)%')
PROFILES_FILE = Path(__file__).with_name('profiles.json')
MODELS_DIR = models_registry.MODELS_DIR
# 既定の解析モデル（＝ models\fast320\ の Tetrace-V1.0-PPT）。
# 旧プロファイル id（standard / fast320）は同じモデルとして扱う。
DEFAULT_PROFILE = models_registry.DEFAULT_MODEL_ID
OTHER_GAME_PROFILE = 'other-game'

# 完成済みとして再利用してよい動画の拡張子（先読みの成果を本処理で使う）。
REUSABLE_VIDEO_EXTENSIONS = ('.mkv', '.mp4', '.webm')
# yt-dlp が書き込み中に残す一時ファイル。これがあれば「未完成」と見なす。
PARTIAL_DOWNLOAD_SUFFIXES = ('.part', '.ytdl')
PARTIAL_DOWNLOAD_FRAGMENT = '.part-'       # 分割取得の途中ファイル（.part-Frag1 など）
# 先読みを始めるのに必要な空き容量。本処理（_download）と同じ 3GB を基準にし、
# RAM ディスクは解析の作業領域を残すため別に余裕を見る。
PREFETCH_MIN_FREE_BYTES = 3 * 1024 ** 3
PREFETCH_RAM_MIN_FREE_BYTES = 2 * 1024 ** 3


def _profile_from_file(profile_id: str) -> dict | None:
    """profiles.json（従来のプロファイル定義）から1件読む。

    新しい仕組みではモデル（models\\<id>\\manifest.json）が本体で、ここは
    「ぷよテト以外」と、モデルが見つからないときの後方互換のためだけに使う。
    """
    try:
        data = json.loads(PROFILES_FILE.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError):
        return None
    for item in data.get('profiles') or []:
        if str(item.get('id')) == str(profile_id):
            if not any(item.get(key) for key in ('sceneModel', 'boardModel', 'settings', 'engine')):
                return None
            return item
    return None


def load_profile(profile_id: str) -> dict | None:
    """解析プロファイル（＝インストール済みモデル）を解決する。

    - モデル id（例 `tetrace-v1.0-ppt`）→ models\\<id>\\manifest.json の files から
      シーン／盤面／NEXT のパスを作る
    - 旧プロファイル id（standard / fast320）→ Tetrace-V1.0-PPT として扱う
    - `other-game` → 従来どおり profiles.json の定義（空なら従来の既定＝None）
    """
    value = str(profile_id or '').strip() or DEFAULT_PROFILE
    if value == OTHER_GAME_PROFILE:
        return _profile_from_file(value)
    profile = models_registry.model_profile(MODELS_DIR,
                                            models_registry.canonical_model_id(value))
    if profile is not None:
        return profile
    # 選んでいたモデルが削除された等で見つからないときは、入っているモデルの
    # 先頭（通常は組み込み）→ profiles.json → 既定（None）の順に落とす。
    installed = models_registry.list_models(MODELS_DIR)
    if installed:
        fallback = models_registry.model_profile(MODELS_DIR, installed[0]['id'])
        if fallback is not None:
            return fallback
    return _profile_from_file(value) or _profile_from_file(DEFAULT_PROFILE)


STUDY_PROGRESS = re.compile(r'study_progress=(\d+)\s+status=(.*)')
STUDY_MANIFEST = re.compile(r'study_manifest=(.+)')


class JobError(Exception):
    """利用者に見せる失敗（日本語の理由つき）。"""


_INVALID_NAME = re.compile(r'[<>:"/\\|?*\x00-\x1f]')

# ---- 内部名（パス長対策） ---------------------------------------------------
# 解析の出力は
#   batches\<バッチ名>\match_136\simulator\<入力名>_tetris_recovered_P1.url
# と深い。Windows のパスは 260 文字までなので、タイトルが長い動画は
# バッチ名と入力名の両方が長いと書けなくなる（実測 266 文字で失敗）。
# **DB に保存するタイトル（表示・タグ・検索）はそのまま**にして、
# 内部で使う名前だけをこの規則で短くする。
#   ・バッチ名 = タイトル 40 文字（切ったら …）+ ` [動画ID]`
#   ・取得した動画 = `[動画ID].mkv`（タイトルは使わない）
MAX_PATH_CHARS = 260
SAFE_PATH_CHARS = 250                  # 生成するパスはここまでに収める（余裕 10 文字）
BATCH_TITLE_CHARS = 40                 # バッチ名に入れるタイトル部分
BATCH_NAME_MAX_CHARS = 96              # バッチ名全体の上限（従来は 120）
IDENT_MAX_CHARS = 24                   # 内部名に添える動画IDの上限
MATCH_DIR_CHARS = len('match_1234')    # 試合フォルダ名の最長（4桁まで）
SIMULATOR_SUFFIX = '_tetris_recovered_report.html'   # エンジンが付ける最長の接尾辞
ELLIPSIS = '…'
YOUTUBE_ID = re.compile(r'(?:v=|youtu\.be/|/live/|/shorts/|/embed/)([A-Za-z0-9_-]{6,20})')
# タイトルの末尾に付いた YouTube の動画ID（`... 【ぷよぷよテトリス2】 [abcdEFGHijk]`）。
# 取り込んだ動画（ローカルの mkv）はタイトル側にしか ID が無い。
TRAILING_ID = re.compile(r'\[\s*([A-Za-z0-9_-]{11})\s*\]\s*$')


def _trim_name(text: str, limit: int) -> str:
    """名前を limit 文字以内に切る（切ったことが分かるよう末尾を … にする）。"""
    if limit <= 0:
        return ''
    if len(text) <= limit:
        return text
    if limit == 1:
        return ELLIPSIS
    return text[:limit - 1].rstrip(' .') + ELLIPSIS


def _safe_dir_name(value: str, fallback: str = 'video',
                   limit: int = BATCH_NAME_MAX_CHARS) -> str:
    """フォルダ名に使えない文字を落とし、内部用に長さも切る。"""
    name = _INVALID_NAME.sub('_', (value or '').strip()).strip(' .')
    return _trim_name(name, limit) or fallback


def _video_ident(video_id: str) -> str:
    """内部名に添える動画ID（YouTube なら11文字）。長すぎるときだけ切る。"""
    ident = _INVALID_NAME.sub('_', str(video_id or '').strip()).strip(' .')
    return ident[:IDENT_MAX_CHARS]


def analysis_ident(source: str = '', video_id: str = '') -> str:
    """内部名に使う識別子（YouTube の動画IDを優先し、無ければ内部ID）。

    表示用のタイトルとは無関係。同じタイトルの別動画を取り違えないための
    目印でもある（バッチ名の末尾と、取得した動画のファイル名に使う）。
    """
    found = YOUTUBE_ID.search(str(source or ''))
    if found:
        return _video_ident(found.group(1))
    return _video_ident(video_id)


def download_file_name(ident: str, suffix: str) -> str:
    """取得した動画の内部ファイル名（表示タイトルは使わない）。

    解析エンジンは入力ファイル名から simulator のファイル名を作るので、
    ここが長いと内部パスが長くなる。ID だけで十分。
    """
    stem = _video_ident(ident) or 'video'
    return f'[{stem}]{str(suffix or "").lower()}'


def _batch_name_budget(root: Path | str, input_stem: str) -> int:
    r"""root の下に作る最長パスが SAFE_PATH_CHARS 以下になるバッチ名の長さ。

    最長は `batches\<バッチ名>\match_1234\simulator\<入力名>_tetris_recovered_report.html`。
    """
    below = (1 + MATCH_DIR_CHARS + 1 + len('simulator') + 1
             + len(str(input_stem)) + len(SIMULATOR_SUFFIX))
    return SAFE_PATH_CHARS - len(str(root)) - 1 - below


def longest_output_path(root: Path | str, folder: str, input_stem: str) -> str:
    """そのバッチで作られる最長のパス（試合番号が4桁のとき）。"""
    return str(Path(root) / folder / 'match_1234' / 'simulator'
               / f'{input_stem}{SIMULATOR_SUFFIX}')


def batch_folder_name(title: str, fallback: str = 'video', source: str = '',
                      video_id: str = '', root: Path | str | None = None,
                      input_stem: str = '') -> str:
    """batches 直下に作るフォルダ名（解析時の出力先と同じ規則）。

    表示用のタイトルはそのまま DB に残し、**この名前だけ**を短くする:
    タイトル 40 文字（切ったら末尾に …）+ ` [動画ID]`。
    `root` を渡すと、その下に作る最長パスが SAFE_PATH_CHARS を超えないよう
    さらに詰める。`input_stem` は解析に渡す動画のファイル名（拡張子抜き）で、
    省略時は取得時の内部名（`[動画ID]`）を使う。

    削除側（server.py）が「この動画のバッチはどれか」を推定するのにも使う。
    """
    source_text = str(source or '')
    ident = analysis_ident(source_text, video_id)
    cleaned = _INVALID_NAME.sub('_', (title or '').strip()).strip(' .')
    trailing = TRAILING_ID.search(cleaned)
    if trailing:
        # タイトル自体が「... [動画ID]」で終わっている（取り込んだ動画など）。
        # そのまま添えると ID が二重になるので、外してから付け直す。
        cleaned = cleaned[:trailing.start()].rstrip(' .')
        if not YOUTUBE_ID.search(source_text):
            ident = _video_ident(trailing.group(1))
    if input_stem:
        stem = str(input_stem)
    else:
        stem = f'[{ident}]' if ident else 'video'
    suffix = f' [{ident}]' if ident else ''
    base = _safe_dir_name(cleaned, '', BATCH_TITLE_CHARS) or fallback
    budget = BATCH_NAME_MAX_CHARS
    if root is not None:
        budget = min(budget, _batch_name_budget(root, stem))
    if budget <= len(suffix):
        # 名前を残す余地が無いときは ID（または fallback）だけにする。
        return _trim_name(ident or fallback, max(1, budget))
    return _trim_name(base, budget - len(suffix)).rstrip(' .') + suffix


# ---- 動画単位のロック ------------------------------------------------------
# 先読み（裏の取得）と本処理（ジョブの取得）が同じ動画を同時に取りに行くと、
# 同じフォルダに2本書き込んで壊れる。プロセス内の辞書で1動画1ロックにする。
_VIDEO_LOCKS: dict[str, threading.Lock] = {}
_VIDEO_LOCKS_GUARD = threading.Lock()


def video_lock(video_id: str) -> threading.Lock:
    """動画1本ぶんのロックを返す（初回に作って以後は使い回す）。"""
    key = str(video_id or '')
    with _VIDEO_LOCKS_GUARD:
        lock = _VIDEO_LOCKS.get(key)
        if lock is None:
            lock = threading.Lock()
            _VIDEO_LOCKS[key] = lock
        return lock


def _partial_downloads(folder: Path) -> list[Path]:
    """書き込み中の一時ファイル（.part / .ytdl）を集める。"""
    try:
        entries = list(folder.iterdir())
    except OSError:
        return []
    found: list[Path] = []
    for entry in entries:
        try:
            if not entry.is_file():
                continue
        except OSError:
            continue
        name = entry.name.lower()
        if name.endswith(PARTIAL_DOWNLOAD_SUFFIXES) or PARTIAL_DOWNLOAD_FRAGMENT in name:
            found.append(entry)
    return found


def _download_marked_failed(folder: Path) -> bool:
    """中断した「書き込みながら解析」の印（done.txt=failed）が残っているか。"""
    try:
        text = (folder / 'done.txt').read_text(encoding='ascii', errors='replace')
    except OSError:
        return False
    return text.strip().lower() == 'failed'


def _complete_video(folder: Path) -> Path | None:
    """保存先にある「完成済み」の動画を返す（無ければ None）。

    判定はマーカーファイルではなく、サイズが 0 より大きく、書き込み中の
    一時ファイル（.part / .ytdl）が残っていないこと。中断した取得の
    残骸（done.txt=failed）は使わない。
    """
    if _download_marked_failed(folder):
        return None
    if _partial_downloads(folder):
        return None
    try:
        entries = list(folder.iterdir())
    except OSError:
        return None
    found: list[tuple[int, Path]] = []
    for entry in entries:
        try:
            if not entry.is_file():
                continue
            if entry.suffix.lower() not in REUSABLE_VIDEO_EXTENSIONS:
                continue
            if entry.name.lower().startswith('audio.'):
                continue        # 音声だけの後追い保存（audio.webm など）は動画ではない
            size = entry.stat().st_size
        except OSError:
            continue
        if size > 0:
            found.append((size, entry))
    if not found:
        return None
    return max(found, key=lambda item: item[0])[1]


_RAM_LABEL = re.compile(r'(?i)(ram ?disk|ramdisk|imdisk|ppt-ram|tmpfs|osfmount)')


def find_ram_disk() -> Path | None:
    """PPT Lab が使える RAM ディスクのルートを返す（無ければ None）。

    判定は run-ppt-stream-analysis.ps1 / ppt-workdir.ps1 と同じ。
    マーカーファイル、ボリュームラベル、環境変数 PPT_RAMDISK の順に見る。
    """
    candidates: list[Path] = []
    configured = os.environ.get('PPT_RAMDISK', '').strip()
    if configured:
        candidates.append(Path(configured))
    for letter in ('R', 'S', 'T'):
        root = Path(f'{letter}:\\')
        if root not in candidates:
            candidates.append(root)
    for root in candidates:
        try:
            if not root.exists():
                continue
        except OSError:
            continue
        if (root / '.ppt-ramdisk.json').is_file():
            return root
        if configured and str(root) == configured:
            return root
        try:
            label = _volume_label(root)
        except OSError:
            label = ''
        if label and _RAM_LABEL.search(label):
            return root
    return None


def _volume_label(root: Path) -> str:
    """Windows のボリュームラベルを取る（失敗しても例外にしない）。"""
    try:
        import ctypes
        buffer = ctypes.create_unicode_buffer(261)
        volume = ctypes.create_unicode_buffer(261)
        if not ctypes.windll.kernel32.GetVolumeNameForVolumeMountPointW(
                str(root), volume, 261):
            return ''
        if not ctypes.windll.kernel32.GetVolumeInformationW(
                volume, buffer, 261, None, None, None, None, 0):
            return ''
        return buffer.value
    except Exception:
        return ''


RAM_DISK_TASK = 'PPT RAM disk'
RAM_DISK_SCRIPT = Path(__file__).resolve().parents[1] / 'ppt-ramdisk-install.ps1'
try:
    RAM_DISK_SIZE_MB = int(os.environ.get('PPT_RAMDISK_MB') or 4096)
except ValueError:
    RAM_DISK_SIZE_MB = 4096
_NO_WINDOW = 0x08000000 if os.name == 'nt' else 0

_RAM_LOCK = threading.Lock()
_RAM_CREATING = threading.Event()
_RAM_ERROR = ['']


def _run_quiet(args, timeout: float = 20.0):
    """出力を捨てて外部コマンドを動かす（失敗しても例外にしない）。"""
    try:
        return subprocess.run(args, capture_output=True, timeout=timeout,
                              creationflags=_NO_WINDOW)
    except (OSError, subprocess.SubprocessError):
        return None


def ram_disk_task_registered() -> bool:
    """ログオン時に RAM ディスクを作るタスクが登録済みか。"""
    result = _run_quiet(['schtasks', '/query', '/tn', RAM_DISK_TASK], timeout=15)
    return bool(result is not None and result.returncode == 0)


def ram_disk_info() -> dict:
    """設定画面と API に出す RAM ディスクの状態。"""
    root = find_ram_disk()
    info = {
        'available': root is not None,
        'root': str(root) if root else '',
        'drive': root.drive if root else '',
        'totalMB': 0,
        'freeMB': 0,
        'task': ram_disk_task_registered(),
        'busy': _RAM_CREATING.is_set(),
        'error': _RAM_ERROR[0],
        'sizeMB': RAM_DISK_SIZE_MB,
    }
    if root is not None:
        try:
            usage = shutil.disk_usage(str(root))
            info['totalMB'] = int(usage.total // (1024 * 1024))
            info['freeMB'] = int(usage.free // (1024 * 1024))
        except OSError:
            pass
    return info


def _wait_for_ram_disk(seconds: float) -> Path | None:
    deadline = time.monotonic() + max(1.0, seconds)
    while True:
        root = find_ram_disk()
        if root is not None:
            return root
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.4)


def _start_ram_disk_task(seconds: float = 15.0) -> Path | None:
    """登録済みタスクを今すぐ走らせる（管理者の確認は出ない）。"""
    if not ram_disk_task_registered():
        return None
    _run_quiet(['schtasks', '/run', '/tn', RAM_DISK_TASK], timeout=20)
    return _wait_for_ram_disk(seconds)


def _create_ram_disk_elevated(seconds: float = 120.0) -> Path | None:
    """管理者の確認（UAC）を1回出して作成する。初回だけ通る道。"""
    if not RAM_DISK_SCRIPT.is_file():
        return None
    inner = (
        "Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -ArgumentList "
        f"'-NoProfile','-ExecutionPolicy','Bypass','-File','\"{RAM_DISK_SCRIPT}\"',"
        f"'-SizeMB','{RAM_DISK_SIZE_MB}'"
    )
    _run_quiet(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                '-Command', inner], timeout=seconds)
    return _wait_for_ram_disk(30.0)


def ensure_ram_disk(wait_seconds: float = 90.0, elevate: bool = True) -> Path | None:
    """RAM ディスクを用意する（あればそのまま、無ければ作る）。

    1. 既にある → それを返す
    2. ログオン時のタスクが登録済み → タスクを起こすだけ（管理者の確認なし）
    3. まだ何も無い → 管理者の確認（UAC）を1回出して作成する
    """
    root = find_ram_disk()
    if root is not None:
        return root
    if _RAM_CREATING.is_set():
        return _wait_for_ram_disk(wait_seconds)
    if not _RAM_LOCK.acquire(timeout=max(5.0, wait_seconds)):
        return _wait_for_ram_disk(5.0)
    try:
        _RAM_CREATING.set()
        _RAM_ERROR[0] = ''
        # タスクが登録済みなら、UAC を出す前にタスクの完了を長めに待つ
        root = _start_ram_disk_task(max(15.0, min(wait_seconds, 60.0)))
        if root is not None:
            return root
        if not elevate:
            _RAM_ERROR[0] = 'RAM ディスクがありません'
            return None
        root = _create_ram_disk_elevated(wait_seconds)
        if root is None:
            _RAM_ERROR[0] = 'RAM ディスクを作成できませんでした（管理者の確認を許可してください）'
        return root
    finally:
        _RAM_CREATING.clear()
        _RAM_LOCK.release()


def _fetch_title(yt_dlp: str, url: str) -> str:
    """yt-dlp から動画タイトルだけ取る（失敗したら空）。"""
    try:
        completed = subprocess.run(
            [yt_dlp, '--ignore-config', '--no-playlist', '--no-warnings', '--get-title', url],
            capture_output=True,
            timeout=45, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except (OSError, subprocess.TimeoutExpired):
        return ''
    if completed.returncode != 0:
        return ''
    for line in _decode(completed.stdout or b'').splitlines():
        value = line.strip()
        if value:
            return value
    return ''


def _target_dir(default: Path) -> Path:
    """動画の保存先（設定があればその下に、無ければ既定の場所）。"""
    store = STORE_REF[0]
    value = ''
    try:
        if store is not None:
            value = str(store.settings().get('downloadDir') or '')
    except Exception:
        value = ''
    if value:
        path = Path(value) / default.name
        try:
            path.mkdir(parents=True, exist_ok=True)
            return path
        except OSError:
            return default
    return default


STORE_REF = [None]


def _quality_height(quality: str = '1080') -> int:
    # YouTube の解像度は 144 / 240 / 360 / 480 / 720 / 1080。320p は存在しないため、
    # 「320」を指定すると 240p に落ちて NEXT の箱が小さくなりすぎる。360p を使うこと。
    return {'1080': 1080, '720': 720, '480': 480, '360': 360, '320': 360}.get(str(quality), 1080)


def _quality_format(quality: str = '1080') -> str:
    """設定の画質上限から yt-dlp の書式を作る。

    60fps を優先する。720p 未満の解像度は YouTube 側が 30fps しか出さないため、
    ここで 50fps 以上を要求しておかないと解析の時間分解能が落ちる。
    `_quality_sort` と組み合わせると、指定解像度の中で最も軽い符号化
    （現在は AV1）が選ばれ、取得量とデコード時間の両方が減る。
    """
    height = _quality_height(quality)
    return (f'bv*[height<={height}][vcodec^=avc1][fps>=50]+ba/'
            f'bv*[height<={height}][vcodec^=avc1]+ba/'
            f'bv*[height<={height}][fps>=50]+ba/'
            f'bv*[height<={height}]+ba/'
            f'b[height<={height}]/b')


def _quality_sort(quality: str = '1080') -> str:
    """指定解像度を保ったまま、いちばん軽い映像を選ぶ。

    `+size` だけだと 720p など小さい解像度に落ちてしまうため、先に解像度を固定する。
    """
    return f'res:{_quality_height(quality)},+size'


QUALITY_VALUES = ('1080', '720', '480', '360', '320')
QUALITY_DEFAULT_SAVED = '1080'   # 動画をPCに保存するとき（きれいさ優先）
QUALITY_DEFAULT_STREAM = '360'   # 解析のためだけに取得するとき（速度優先）


def _truthy(value) -> bool:
    """設定の真偽値（JSON の true / 文字列の on など）を bool にする。"""
    if isinstance(value, str):
        return value.strip().lower() not in ('', '0', 'false', 'off', 'no')
    return bool(value)


def _effective_quality(store=None, saved: bool = False) -> str:
    """取得する解像度を「動画をPCに保存するか」で切り替える。

    保存する動画は `downloadQualitySaved`（既定 1080p）、解析のためだけに
    取得する動画は `downloadQualityStream`（既定 360p）を使う。
    旧キー `downloadQuality` しか無い DB でも落ちないよう、新しいキーが
    無い／不正なときは既定値に戻す（移行処理はせず、読むだけ）。
    """
    key = 'downloadQualitySaved' if saved else 'downloadQualityStream'
    default = QUALITY_DEFAULT_SAVED if saved else QUALITY_DEFAULT_STREAM
    target = store if store is not None else STORE_REF[0]
    try:
        values = target.settings() if target is not None else {}
    except Exception:
        values = {}
    value = str(values.get(key) or '').strip()
    if value in QUALITY_VALUES:
        return value
    return default


def _cookie_args(store=None) -> list[str]:
    """yt-dlp に渡す Cookie 引数（設定 `ytdlpCookies` があれば）。

    YouTube の「Sign in to confirm you're not a bot」を回避するための任意設定。
    未設定なら何も足さない（従来と同じ挙動）。
    """
    target = store if store is not None else STORE_REF[0]
    try:
        value = str((target.settings() if target else {}).get('ytdlpCookies') or '').strip()
    except Exception:
        value = ''
    if not value:
        return []
    path = Path(value)
    if not path.is_file():
        return []
    return ['--cookies', str(path)]


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
        STORE_REF[0] = store
        self.data = Path(data_dir)
        self.tools = Path(tools_dir)
        self.runtime = Path(runtime_dir)
        self.media = self.data / 'media'
        self.media.mkdir(parents=True, exist_ok=True)
        self._jobs: dict[str, dict] = {}
        self._lock = threading.Lock()
        self._worker = threading.Lock()   # 解析は同時1本
        self._prefetch = threading.Lock()  # 先読みも同時1本（保持している間だけ走る）
        self.ram_disk_missing = False     # RAM 設定オンなのにドライブが無い

    def _download_root(self, video_id: str) -> Path:
        """動画を置くディレクトリ（動画1本ぶん）。

        「RAM ディスクを使う」がオンで RAM ディスクが見つかれば、**そこだけ**を使う。
        SSD には動画を書き残さない。オフ（またはドライブが無い）なら従来どおり。
        """
        default = self.media / video_id
        try:
            raw = str(self.store.settings().get('useRamDisk') or '').strip().lower()
        except Exception:
            raw = ''
        if raw in ('1', 'true', 'yes', 'on'):
            # 設定がオンなら、まず自分で用意する（初回だけ管理者の確認が出る）
            root = ensure_ram_disk(wait_seconds=120.0, elevate=True)
            if root is None:
                # 「RAM にだけ置く」設定なのに RAM が無いなら、勝手に SSD へは書かない。
                raise JobError(
                    'RAM ディスクを用意できませんでした。管理者の確認（UAC）を許可すれば自動作成されます。'
                    'もう一度解析を開始するか、設定の「RAMディスクを使う」を一度オフにすると'
                    '通常のドライブに保存します。')
            target = root / 'PPTLab' / 'media' / video_id
            try:
                target.mkdir(parents=True, exist_ok=True)
            except OSError as error:
                raise JobError(f'RAM ディスクに作業フォルダを作れません: {error}') from error
            self.ram_disk_missing = False
            return target
        return _target_dir(default)

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

    def _update(self, job_id: str, only_if_queued: bool = False, **fields) -> None:
        """ジョブの表示を更新する。

        `only_if_queued` は先読み用。走り始めたジョブの「準備しています…」などを
        裏のスレッドから上書きしないよう、待機中のときだけ書き換える。
        """
        with self._lock:
            job = self._jobs.get(job_id)
            if not job:
                return
            if only_if_queued and job.get('status') != 'queued':
                return
            job.update(fields)
            job['updatedAt'] = time.time()
            terminal = job.get('status') in ('done', 'failed')
            if terminal or time.time() - job.get('_persistedAt', 0) > 2:
                job['_persistedAt'] = time.time()
                self._persist(job_id, job.get('status', ''), job.get('message', ''))

    # ---- 起動 -------------------------------------------------------------
    def start(self, video_id: str, source: str, save_video: bool = True) -> str:
        job_id = self.store.add_job('video', 'queued',
                                    {'videoId': video_id, 'source': source,
                                     'saveVideo': bool(save_video)})
        with self._lock:
            self._jobs[job_id] = {'id': job_id, 'status': 'queued', 'progress': 0,
                                  'message': '待機中', 'error': '', 'videoId': video_id,
                                  'source': source, 'saveVideo': bool(save_video),
                                  'updatedAt': time.time()}
        threading.Thread(target=self._run, args=(job_id, video_id, source, bool(save_video)),
                         daemon=True).start()
        return job_id

    # ---- 本体 -------------------------------------------------------------
    def _run(self, job_id: str, video_id: str, source: str, save_video: bool = True) -> None:
        video_path: Path | None = None
        downloaded = False
        try:
            with self._worker:
                self._update(job_id, status='running', message='準備しています…')
                self.store.set_video_status(video_id, 'analyzing')
                if source.lower().startswith(('http://', 'https://')):
                    # 先に全部取得してから解析する。
                    # 追記型 MP4 を書きながら読ませると、シークできない容器と
                    # 読み取り待ちのせいで解析が約 2.8 倍遅くなる。
                    # 実測（30分/360p）: 並行 371 秒 → 取得完了後 133 秒。
                    # 360p は 53MB / 15 秒で落ちるので、重ねる利得より損失が大きい。
                    video_path = self._download(job_id, video_id, source)
                    downloaded = True
                    # 解析に入る。この裏で「次に待っている URL 動画」の取得を始める。
                    self._start_prefetch(job_id)
                    manifest = self._analyze(job_id, video_path, video_id=video_id)
                else:
                    video_path = self._resolve_source(job_id, video_id, source)
                    self._start_prefetch(job_id)
                    manifest = self._analyze(job_id, video_path, video_id=video_id)
                matches = self._match_count(manifest)
                # 動画はこの後に消えることがある（save_video=False）。名前画像を
                # 先に残しておかないと、後から「同じ画像＝同じ人」でまとめられない。
                crop_warning = self._cut_name_crops(manifest, video_path)
                self.store.set_video_status(video_id, 'needs-names', matches,
                                             str(manifest))
                warning = ''
                if downloaded and save_video and video_path:
                    try:
                        warning = self._attach_audio(video_path, video_id, source, job_id)
                    except Exception as error:
                        warning = f'音声の保存に失敗: {error}'
                message = f'{matches}試合を検出しました'
                if crop_warning:
                    message += f'（{crop_warning}）'
                if warning:
                    message += f'（{warning}）'
                self._update(job_id, status='done', progress=100,
                             message=message, manifest=str(manifest))
        except JobError as error:
            self._fail(job_id, video_id, str(error))
        except Exception as error:                       # 想定外も握りつぶさない
            self._fail(job_id, video_id, f'予期しない失敗: {error}')
        finally:
            # 「PCに保存しない」なら、解析後にダウンロードした動画を消す。
            if downloaded and video_path:
                for aux in ('ready.txt', 'done.txt'):
                    try:
                        (video_path.parent / aux).unlink()
                    except OSError:
                        pass
                if not save_video:
                    try:
                        video_path.unlink()
                    except OSError:
                        pass
                    try:
                        video_path.parent.rmdir()   # 空のときだけ消える
                    except OSError:
                        pass

    def _fail(self, job_id: str, video_id: str, message: str) -> None:
        self.store.set_video_status(video_id, 'failed')
        self._update(job_id, status='failed', error=message, message=message)

    # ---- 名前画像の切り出し -----------------------------------------------
    def _cut_name_crops(self, manifest: Path, video: Path | None) -> str:
        """解析結果から P1/P2 の名前画像を切り出して残す（失敗は警告だけ）。

        画像の指紋は名前の記憶（name_memory）と同じ値なので、ここで残しておけば
        動画を消した後でも「同じ画像＝同じ人」でプレイヤー名をまとめられる。
        保存先が同じなら作り直さない（冪等）。
        """
        if not manifest or not Path(manifest).is_file():
            return ''
        ffmpeg = self.runtime / 'bin' / 'ffmpeg.exe'
        try:
            summary = save_name_crops(manifest, ffmpeg if ffmpeg.is_file() else None,
                                      name_crop_dir(), video)
        except Exception as error:                      # 解析そのものは止めない
            return f'名前画像の切り出しに失敗: {error}'
        error = str(summary.get('error') or '')
        if error:
            return f'名前画像の切り出しに失敗: {error}'
        return ''

    # ---- 先読み（解析の裏で次の動画を取得） --------------------------------
    def _pending_url_job(self, current_job_id: str) -> dict | None:
        """まだ走っていない URL ジョブのうち、次に実行される1本。

        `_jobs` は登録順（＝おおむね実行順）なので、先頭から見て最初の
        「queued かつ URL」を返す。1本だけのときは None を返すので、
        単発の解析では先読みが起きない。
        """
        with self._lock:
            rows = [dict(job) for job in self._jobs.values()]
        for row in rows:
            if str(row.get('id')) == current_job_id:
                continue
            if row.get('status') != 'queued':
                continue
            source = str(row.get('source') or '')
            if source.lower().startswith(('http://', 'https://')):
                return row
        return None

    def _prefetch_blocker(self, video_id: str) -> str:
        """先読みを始めてよいか。駄目ならその理由（空文字なら開始してよい）。"""
        try:
            settings = self.store.settings()
        except Exception:
            settings = {}
        use_ram = str(settings.get('useRamDisk') or '').strip().lower() in (
            '1', 'true', 'yes', 'on')
        # 取得先は本処理（_download）と同じ場所。空きが本処理の基準に満たないなら
        # 先読みしても本処理で失敗するので、先に諦める。
        folder = self._download_dir(video_id)
        try:
            free = shutil.disk_usage(str(folder)).free
        except OSError:
            return '保存先の空き容量を確認できませんでした'
        if free < PREFETCH_MIN_FREE_BYTES:
            return f'保存先の空きが足りません（{free / 1024 ** 3:.1f} GB）'
        if not use_ram:
            return ''
        # RAM ディスクは解析の作業領域でもある。先読みで埋めないよう余裕を見る。
        root = find_ram_disk()
        if root is None:
            return 'RAM ディスクが見つかりません'
        try:
            ram_free = shutil.disk_usage(str(root)).free
        except OSError:
            return 'RAM ディスクの空き容量を確認できませんでした'
        if ram_free < PREFETCH_RAM_MIN_FREE_BYTES:
            return f'RAM ディスクの空きが足りません（{ram_free / 1024 ** 3:.1f} GB）'
        return ''

    def _start_prefetch(self, job_id: str) -> None:
        """解析に入るタイミングで、次に待っている URL 動画の取得を裏で始める。

        - 同時に走る先読みは1本まで（`_prefetch` ロックが空いているときだけ）。
        - 解析は GPU、取得は CPU とネットワークなので競合しない。
        - ここでの失敗は本処理に影響させない（取得は走るときにやり直せる）。
        """
        if not self._prefetch.acquire(blocking=False):
            return
        handed_off = False
        try:
            pending = self._pending_url_job(job_id)
            if pending is None:
                return
            video_id = str(pending.get('videoId') or '')
            source = str(pending.get('source') or '')
            if not video_id or not source:
                return
            blocker = self._prefetch_blocker(video_id)
            if blocker:
                # 容量が足りないときは先読みしない（本処理は通常どおり）。
                self._update(pending['id'], prefetch='skipped', prefetchNote=blocker)
                return
            threading.Thread(target=self._prefetch_job,
                             args=(str(pending['id']), video_id, source),
                             name=f'ppt-prefetch-{video_id}', daemon=True).start()
            handed_off = True
        except Exception:
            # 先読みの段取りに失敗しても、本処理はそのまま続ける。
            pass
        finally:
            if not handed_off:
                self._prefetch.release()

    def _prefetch_job(self, job_id: str, video_id: str, url: str) -> None:
        """待機中のジョブの動画だけを先に取得しておく（解析はしない）。"""
        started_at = time.time()
        error = ''
        message = ''
        try:
            self._update(job_id, only_if_queued=True, prefetch='running',
                         message='次の動画を先に取得しています…')
            self._download(job_id, video_id, url, prefetch=True)
            message = '先に取得を終えました（解析待ち）'
        except Exception as failure:              # 先読みの失敗は本処理に影響させない
            error = str(failure) or failure.__class__.__name__
            message = '先読みに失敗しました（実行時に取り直します）'
        finally:
            try:
                self._update(job_id, prefetch='failed' if error else 'done',
                             prefetchError=error,
                             prefetchSeconds=round(time.time() - started_at, 1))
                if message:
                    # もう走り始めていたら、そのジョブの表示は上書きしない。
                    self._update(job_id, only_if_queued=True, message=message)
            finally:
                self._prefetch.release()

    # ---- 取得しながら解析 -------------------------------------------------
    def _download_streaming(self, job_id: str, video_id: str, url: str) -> tuple[Path, dict]:
        """yt-dlp の出力を ffmpeg で追記型 MP4 に詰め、書き込みながら解析させる。

        取得（数分）と解析（十数分）が重なるので、全体の待ち時間が縮む。
        追記型に詰める都合で音声は落とす（解析には使わない）。
        """
        managed = self.runtime / 'tools' / 'yt-dlp' / 'yt-dlp.exe'
        yt_dlp = str(managed) if managed.is_file() else (shutil.which('yt-dlp') or '')
        if not yt_dlp:
            raise JobError('yt-dlp が見つかりません。README の手順で導入してください。')
        ffmpeg = self.runtime / 'bin' / 'ffmpeg.exe'
        if not ffmpeg.is_file():
            raise JobError('ffmpeg が見つかりません（runtime\\bin\\ffmpeg.exe）。')

        # タイトルは先に取る（取得に失敗しても解析は続ける）。
        self._update(job_id, message='タイトルを取得しています…')
        title = _fetch_title(yt_dlp, url)
        if title:
            try:
                self.store.set_video_title(video_id, title)
            except Exception:
                pass

        # RAM ディスク設定がオンなら、動画は RAM 上だけに置く（SSD には書かない）。
        work = self._download_root(video_id)
        work.mkdir(parents=True, exist_ok=True)
        try:
            free_bytes = shutil.disk_usage(str(work)).free
        except OSError:
            free_bytes = None
        if free_bytes is not None and free_bytes < 2 * 1024 ** 3:
            raise JobError(
                f'動画の保存先の空きが足りません（{free_bytes / 1024 ** 3:.1f} GB）。'
                'RAM ディスクの容量を増やすか、「RAM ディスクを使う」をオフにしてください。')
        video = work / 'video.mp4'
        ready = work / 'ready.txt'
        done = work / 'done.txt'
        for path in (video, done):
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        ready.write_text('0', encoding='ascii')

        # 追記型で解析しながら取得するだけ（保存しない）ので、速度優先の解像度。
        quality = _effective_quality(self.store, saved=False)
        height = _quality_height(quality)
        # 追記型に詰めるので映像のみ。60fps を優先し、無ければ解像度優先で落とす。
        video_format = (f'bv*[height<={height}][vcodec^=avc1][fps>=50]/'
                        f'bv*[height<={height}][vcodec^=avc1]/'
                        f'bv*[height<={height}][fps>=50]/'
                        f'bv*[height<={height}]/b[height<={height}]/b')
        downloader = subprocess.Popen(
            [yt_dlp, *_cookie_args(self.store), '--ignore-config', '--newline',
             '--no-playlist', '--no-warnings',
             '--no-part', '-N', '8', '-f', video_format, '-o', '-', url],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        remux = subprocess.Popen(
            [str(ffmpeg), '-v', 'error', '-y', '-i', 'pipe:0', '-c', 'copy', '-an',
             '-movflags', '+frag_keyframe+empty_moov+default_base_moof',
             '-frag_duration', '1000000', '-min_frag_duration', '500000',
             '-f', 'mp4', str(video), '-progress', 'pipe:1'],
            stdin=downloader.stdout, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        downloader.stdout.close()
        assert remux.stdout is not None

        def pump() -> None:
            baseline = None
            try:
                for raw in iter(remux.stdout.readline, b''):
                    text = raw.decode('utf-8', 'replace').strip()
                    if not text.startswith('out_time_us='):
                        continue
                    try:
                        seconds = int(text.split('=', 1)[1]) / 1_000_000.0
                    except ValueError:
                        continue
                    if baseline is None:
                        baseline = seconds
                    value = max(0.0, seconds - baseline)
                    try:
                        ready.write_text(f'{value:.3f}', encoding='ascii')
                    except OSError:
                        pass
            finally:
                try:
                    remux.stdout.close()
                except Exception:
                    pass
                remux.wait()
                downloader.wait()
                ok = remux.returncode == 0 and downloader.returncode == 0
                try:
                    done.write_text('done' if ok else 'failed', encoding='ascii')
                except OSError:
                    pass

        threading.Thread(target=pump, daemon=True).start()

        deadline = time.time() + 240
        while time.time() < deadline:
            try:
                if video.is_file() and video.stat().st_size > 1_000_000:
                    break
            except OSError:
                pass
            if done.exists():
                break
            time.sleep(0.5)
        if not (video.is_file() and video.stat().st_size > 1_000_000):
            try:
                done.write_text('failed', encoding='ascii')
            except OSError:
                pass
            raise JobError('動画の取得を開始できませんでした（回線か yt-dlp の応答を確認してください）。')

        self._update(job_id, message='取得しながら解析しています…')
        return video, {'ready': ready, 'done': done}

    # ---- 取得 -------------------------------------------------------------
    def _resolve_source(self, job_id: str, video_id: str, source: str) -> Path:
        if source.lower().startswith(('http://', 'https://')):
            return self._download(job_id, video_id, source)
        path = Path(source)
        if not path.is_file():
            raise JobError(f'動画ファイルが見つかりません: {source}')
        return path

    def _download_dir(self, video_id: str) -> Path:
        """動画の保存先フォルダ（本処理と先読みで同じ計算を使う）。"""
        target = self.media / video_id
        target.mkdir(parents=True, exist_ok=True)
        # 保存先は設定 downloadDir を優先する（RAM ディスクを指定できる）。
        # 取得したファイルもこのディレクトリに現れるので、探索先を同じ値にする。
        return _target_dir(target)

    def _download(self, job_id: str, video_id: str, url: str, prefetch: bool = False) -> Path:
        """URL の動画を取得して、保存先のパスを返す。

        同じ動画を先読みと本処理で同時に取りに行かないよう、動画単位の
        ロックの中で行う。先読みで完成していれば、そのファイルをそのまま返す。
        取得する解像度は、そのジョブが動画を保存するかどうかで切り替える。
        """
        lock = video_lock(video_id)
        if lock.locked():
            # 先読みの取得がまだ終わっていない（終わるまで待ってから使う）。
            self._update(job_id, only_if_queued=prefetch,
                         message='先に取得した動画の完了を待っています…')
        with lock:
            return self._download_locked(job_id, video_id, url, prefetch,
                                         self._saves_video(job_id))

    def _saves_video(self, job_id: str = '') -> bool:
        """そのジョブが動画をPCに保存するか（設定 downloadVideo も見る）。

        保存するなら取得解像度は `downloadQualitySaved`、保存しないなら
        `downloadQualityStream` を使う。先読みは「走る前のジョブ」なので、
        キューに登録された saveVideo（＝追加画面のチェック）で判断する。
        """
        try:
            settings = self.store.settings()
        except Exception:
            settings = {}
        if _truthy(settings.get('downloadVideo')):
            return True
        try:
            job = self.get(job_id) or {}
        except Exception:
            job = {}
        return _truthy(job.get('saveVideo'))

    def _download_locked(self, job_id: str, video_id: str, url: str,
                         prefetch: bool = False, saved: bool = False) -> Path:
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
        download_root = self._download_dir(video_id)
        ident = analysis_ident(url, video_id)
        # 完成済み（先読みの成果や前回の取得物）があれば、取り直さずそれを使う。
        existing = _complete_video(download_root)
        if existing is not None:
            self._update(job_id, only_if_queued=prefetch,
                         message='取得済みの動画を使います…')
            return self._normalize_video_name(existing, ident)
        # 中断した取得の残骸（.part / .ytdl）は消してから取り直す。
        for leftover in _partial_downloads(download_root):
            try:
                leftover.unlink()
            except OSError:
                pass
        title = _fetch_title(yt_dlp, url)
        if title:
            try:
                self.store.set_video_title(video_id, title)
            except Exception:
                pass
        try:
            free_bytes = shutil.disk_usage(str(download_root)).free
        except OSError:
            free_bytes = None
        if free_bytes is not None and free_bytes < 3 * 1024 ** 3:
            raise JobError(
                f'動画の保存先の空きが足りません（{free_bytes / 1024 ** 3:.1f} GB）。'
                f'RAM ディスクなら容量を増やすか、設定の「動画の保存先」を空に戻してください。')
        started_at = time.time()
        # 保存する動画はきれいさ優先、解析だけなら速度優先の解像度にする。
        quality = _effective_quality(self.store, saved=saved)
        command = [yt_dlp, *_cookie_args(self.store), '--newline', '--no-playlist', '--no-warnings',
                   '--merge-output-format', 'mkv',
                   '-N', '8',
                   '-f', _quality_format(quality),
                   '-S', _quality_sort(quality),
                   # 内部名は `[動画ID].mkv`。タイトルを入れると、エンジンが入力名から
                   # 作る simulator のファイル名まで長くなり、パス長を超える。
                   '-o', str(download_root / f'[{ident}].%(ext)s')]
        ffmpeg = self.runtime / 'bin' / 'ffmpeg.exe'
        if ffmpeg.is_file():
            command += ['--ffmpeg-location', str(ffmpeg.parent)]
        command.append(url)
        self._run_ytdlp(command, job_id, prefetch=prefetch)
        files = [p for p in download_root.iterdir()
                 if p.is_file() and p.suffix.lower() in VIDEO_EXTENSIONS
                 and (video_id in p.name or f'[{ident}]' in p.name)]
        if not files:
            # ファイル名に ID が入らない命名規則でも拾えるよう、今回の取得分に限定する。
            files = [p for p in download_root.iterdir()
                     if p.is_file() and p.suffix.lower() in VIDEO_EXTENSIONS
                     and p.stat().st_mtime >= started_at - 60]
        if not files:
            raise JobError('取得した動画ファイルが見つかりません。')
        return self._normalize_video_name(max(files, key=lambda p: p.stat().st_size), ident)

    @staticmethod
    def _normalize_video_name(video: Path, ident: str) -> Path:
        """取得した動画を内部名（`[動画ID].mkv`）にそろえる。

        古い版が付けたタイトル入りの名前をそのまま使うと、解析エンジンが
        入力名から作る simulator のファイル名も長くなる。解析対象は同じ
        ファイルなので、名前だけを短くする（内容は変わらない）。
        """
        target = video.with_name(download_file_name(ident, video.suffix))
        if video.name == target.name or target.exists():
            return video
        try:
            video.replace(target)
        except OSError:
            return video            # 変えられなくても解析は続けられる
        return target

    def _run_ytdlp(self, command: list[str], job_id: str, prefetch: bool = False) -> None:
        """yt-dlp を動かし、進捗をジョブへ流す（失敗は JobError）。

        取得そのものを差し替えられるよう、外部プロセスの起動はここに閉じる。
        """
        message = '次の動画を先に取得しています…' if prefetch else '動画を取得しています…'
        self._update(job_id, only_if_queued=prefetch, message=message)
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
                self._update(job_id, only_if_queued=prefetch,
                             progress=percent * DOWNLOAD_SHARE / 100,
                             message=f'{message} {percent:.0f}%')
        code = process.wait()
        if code != 0:
            raise JobError('動画を取得できませんでした。' +
                           ((' ' + tail[-1]) if tail else ''))

    # ---- 音声の後追い保存 -------------------------------------------------
    def _find_yt_dlp(self) -> str:
        managed = self.runtime / 'tools' / 'yt-dlp' / 'yt-dlp.exe'
        if managed.is_file():
            return str(managed)
        found = shutil.which('yt-dlp') or shutil.which('yt-dlp.exe')
        if found:
            return found
        local = self.tools / 'bin' / 'yt-dlp.exe'
        return str(local) if local.is_file() else ''

    def _download_audio(self, yt_dlp: str, url: str, work: Path) -> Path | None:
        command = [yt_dlp, '--ignore-config', '--newline', '--no-playlist',
                   '--no-warnings', '--no-part', '-N', '4',
                   '-f', 'bestaudio/best',
                   '-o', str(work / 'audio.%(ext)s'), url]
        try:
            completed = subprocess.run(command, capture_output=True, timeout=900,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except (OSError, subprocess.TimeoutExpired):
            return None
        if completed.returncode != 0:
            return None
        files = [path for path in work.iterdir()
                 if path.is_file() and path.suffix.lower() in AUDIO_EXTENSIONS
                 and path.name.startswith('audio.')]
        if not files:
            return None
        return max(files, key=lambda path: path.stat().st_mtime)

    def _mux_audio(self, ffmpeg: Path, video: Path, audio: Path, output: Path) -> bool:
        try:
            output.unlink()
        except OSError:
            pass
        command = [str(ffmpeg), '-y', '-v', 'error',
                   '-i', str(video), '-i', str(audio),
                   '-map', '0:v:0', '-map', '1:a:0', '-c', 'copy', '-shortest',
                   str(output)]
        try:
            completed = subprocess.run(command, capture_output=True, timeout=600,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except (OSError, subprocess.TimeoutExpired):
            return False
        return (completed.returncode == 0 and output.is_file()
                and output.stat().st_size > 0)

    @staticmethod
    def _same_file(left: Path, right: Path) -> bool:
        """同じファイルを指しているか（表記ゆれをならして比べる）。"""
        try:
            if not (left.exists() and right.exists()):
                return False
            return (os.path.normcase(os.path.abspath(str(left)))
                    == os.path.normcase(os.path.abspath(str(right))))
        except OSError:
            return False

    def _attach_audio(self, video: Path, video_id: str, url: str, job_id: str) -> str:
        """解析後に音声だけ取得し、映像と結合して1本の mkv にする。"""
        yt_dlp = self._find_yt_dlp()
        if not yt_dlp:
            return 'yt-dlp が見つかりません'
        ffmpeg = self.runtime / 'bin' / 'ffmpeg.exe'
        if not ffmpeg.is_file():
            return 'ffmpeg が見つかりません'
        work = video.parent
        # 保存する動画の名前も内部名（`[動画ID].mkv`）にそろえる。タイトルを
        # 使うと、この動画を解析し直すときに simulator の名前が長くなる。
        output = work / download_file_name(analysis_ident(url, video_id), '.mkv')
        if self._same_file(video, output):
            # 取得した動画がすでに最終名になっている（yt-dlp が音声込みで mkv に
            # している）。自分自身へ結合すると結合元を消してしまうため、そのまま残す。
            # 先読みで取得済みのファイルを再解析するときもここを通る。
            return ''
        self._update(job_id, message='音声を取得しています…')
        audio = self._download_audio(yt_dlp, url, work)
        if not audio:
            return '音声を取得できませんでした'
        self._update(job_id, message='音声を結合しています…')
        if not self._mux_audio(ffmpeg, video, audio, output):
            try:
                audio.unlink()
            except OSError:
                pass
            return '音声の結合に失敗しました'
        for path in (video, audio):
            try:
                path.unlink()
            except OSError:
                pass
        return ''

    # ---- 解析 -------------------------------------------------------------
    def _short_input_alias(self, video: Path, ident: str) -> Path | None:
        """長いファイル名の動画に短い別名を作る（ハードリンクなので容量は増えない）。

        エンジンは入力ファイル名から simulator のファイル名を作るため、入力名が
        長いと出力がパス長を超える。別名を作れないときは None（解析は元の名前）。
        """
        target = video.with_name(download_file_name(ident, video.suffix))
        if target == video or target.exists():
            return None
        try:
            os.link(video, target)
        except OSError:
            return None
        return target

    def _analyze(self, job_id: str, video: Path, stream: dict | None = None,
                 video_id: str = '') -> Path:
        script = self.tools / 'run-ppt-stream-analysis.ps1'
        if not script.is_file():
            raise JobError('解析スクリプトが見つかりません（tools\\run-ppt-stream-analysis.ps1）。')
        # 解析エンジン・DLL・モデルなどの不足は、ラッパー内の解析前チェック
        # （tools\ppt-preflight.ps1、終了コード 2）が日本語の対処つきで知らせる。
        # 出力先は `batches\<短縮したバッチ名>`。表示タイトル（DB）はそのままで、
        # 内部名だけを短くする。パス長が上限を超えないよう、バッチ名を詰め、
        # それでも足りないときは入力名も短い別名に置き換える。
        source_url = ''
        title = ''
        try:
            row = self.store.video(video_id) if video_id else None
        except Exception:
            row = None
        if row:
            source_url = str(row.get('source') or '')
            title = str(row.get('title') or '')
        ident = analysis_ident(source_url, video_id)
        try:
            configured = str(self.store.settings().get('analysisOutputDir') or '').strip()
        except Exception:
            configured = ''
        # 設定「解析の出力先」は親フォルダとして扱う（バッチ名は下に1段作る）。
        output_root = Path(configured) if configured else (self.runtime / 'data' / 'batches')
        folder = batch_folder_name(title, source=source_url, video_id=video_id,
                                   root=output_root, input_stem=video.stem)
        alias: Path | None = None
        if len(longest_output_path(output_root, folder, video.stem)) > SAFE_PATH_CHARS:
            alias = self._short_input_alias(video, ident)
            if alias is not None:
                video = alias
                folder = batch_folder_name(title, source=source_url, video_id=video_id,
                                           root=output_root, input_stem=video.stem)
        batch_dir = output_root / folder
        projected = longest_output_path(output_root, folder, video.stem)
        if len(projected) > SAFE_PATH_CHARS:
            raise JobError(
                f'解析の出力パスが長すぎます（{len(projected)} 文字、上限 {SAFE_PATH_CHARS} 文字）。'
                f'設定の「解析の出力先」を短い場所にしてください: {batch_dir}')
        command = ['powershell.exe', '-NoLogo', '-NoProfile', '-STA', '-ExecutionPolicy', 'Bypass',
                   '-File', str(script), '-VideoPath', str(video), '-MachineReadable',
                   '-OutputDirectory', str(batch_dir)]
        # 取得しながら解析する場合、エンジンへ「どこまで書けたか」を教える。
        # 環境変数は PowerShell ラッパーを素通りして子プロセスへ継承される。
        env = None
        if stream:
            env = os.environ.copy()
            env['TR_STREAM_READY_FILE'] = str(stream['ready'])
            env['TR_STREAM_DONE_FILE'] = str(stream['done'])
            env['TR_STREAM_MARGIN_SECONDS'] = '8'
            env['TR_STREAM_READ_MARGIN_SECONDS'] = '30'
        try:
            _s = self.store.settings()
            if _s.get('useRamDisk'):
                command.append('-UseRamDisk')
            if _s.get('detailedLog'):
                command.append('-DetailedLog')
            # 解析デバイスは GPU 固定（CPU では実用速度にならないため、切り替えは廃止）。
        except Exception:
            pass
        # 解析プロファイル（モデルの切り替え）。未指定なら従来どおり。
        try:
            profile = load_profile(self.store.settings().get('analysisProfile', DEFAULT_PROFILE))
        except Exception:
            profile = None
        if profile:
            if profile.get('engine'):
                command += ['-BatchExecutable', str(profile['engine'])]
            if profile.get('sceneModel'):
                command += ['-SceneModelPath', str(profile['sceneModel'])]
            if profile.get('boardModel'):
                command += ['-BoardModelPath', str(profile['boardModel'])]
            if profile.get('settings'):
                command += ['-SettingsPath', str(profile['settings'])]
        self._update(job_id, progress=DOWNLOAD_SHARE, message='配信を解析しています…')
        try:
            process = subprocess.Popen(command, cwd=str(self.tools), stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, env=env,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            manifest: Path | None = None
            tail: list[str] = []
            # 解析前チェックの ❌ / ⚠️ 行だけ残す（長い解析でもメモリを増やさない）。
            check_lines: list[str] = []
            assert process.stdout is not None
            for raw in process.stdout:
                line = _decode(raw).strip()
                if line:
                    tail = (tail + [line])[-12:]
                    if line.startswith(('❌', '⚠️')):
                        check_lines.append(line)
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
        finally:
            if alias is not None:
                try:
                    alias.unlink()
                except OSError:
                    pass
        if code == 2:
            # 解析前チェック（tools\ppt-preflight.ps1）で不足が見つかった。
            # 解析は始まっていないので、失敗の一言ではなく対処を出す。
            reason = ' / '.join(line.lstrip('❌⚠️ ').strip() for line in check_lines[:4])
            raise JobError('解析を開始できませんでした。'
                           + (reason or '必要なファイルが足りません。'))
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
