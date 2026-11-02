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
import stat
import subprocess
import sys
import threading
import time
import uuid
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path



def _dir_size(path: Path) -> int:
    if not path.exists():
        return 0
    total = 0
    for item in path.rglob('*'):
        if item.is_file():
            try:
                total += item.stat().st_size
            except OSError:
                pass
    return total


def data_info() -> dict:
    """データ管理タブに出す情報。"""
    data_dir = Path(os.environ.get('LOCALAPPDATA') or Path.home()) / 'PPTLab'
    db = data_dir / 'pptlab.sqlite3'
    batches = ROOT_RUNTIME / 'data' / 'batches'
    caches = list(batches.rglob('analysis-cache.v1.json')) if batches.exists() else []
    return {
        'version': VERSION,
        'dataDir': str(data_dir),
        'dbSize': db.stat().st_size if db and db.is_file() else 0,
        'mediaSize': _dir_size(data_dir / 'media'),
        'cropSize': _dir_size(CROP_DIR),
        'analysisCacheSize': sum(item.stat().st_size for item in caches if item.is_file()),
        'analysisCacheCount': len(caches),
        'videos': len(STORE.videos()),
        'matches': sum(int(item.get('matchCount') or 0) for item in STORE.videos()),
    }

SETTINGS_SCHEMA = [
    {'key': 'startView', 'group': '一般', 'label': '起動時に開く画面', 'type': 'enum',
     'default': 'videos', 'options': [['videos', '動画一覧'], ['last', '前回の画面']]},
    {'key': 'openLinks', 'group': '一般', 'label': 'リンクの開き方', 'type': 'enum',
     'default': 'newTab', 'options': [['newTab', '新しいブラウザ'], ['sameWindow', 'このウィンドウ内']],
     'note': '「このウィンドウ内」は準備中です（選んでも新しいタブで開きます）'},
    {'key': 'notifyAnalysis', 'group': '一般', 'label': '解析完了の通知', 'type': 'bool', 'default': True},
    {'key': 'tagPromptOnSave', 'group': '一般', 'label': '保存時にタグ選択を出す', 'type': 'bool', 'default': True},
    {'key': 'theme', 'group': '表示', 'label': 'テーマ', 'type': 'enum', 'default': 'light',
     'options': [['light', 'ライト'], ['dark', 'ダーク']]},
    {'key': 'pageSize', 'group': '表示', 'label': '1ページの件数', 'type': 'int', 'default': 40,
     'options': [[20, '20'], [40, '40'], [60, '60'], [100, '100']]},
    {'key': 'playbackRate', 'group': '表示', 'label': '既定の再生速度', 'type': 'enum', 'default': '1',
     'options': [['0.25', 'x0.25'], ['0.5', 'x0.5'], ['1', 'x1.0'], ['1.5', 'x1.5']]},
    {'key': 'sortOrder', 'group': '表示', 'label': '既定の並び順', 'type': 'enum', 'default': 'new',
     'options': [['new', '新しい順'], ['old', '古い順'], ['matches', '試合数が多い順'], ['title', '名前順']]},
    {'key': 'downloadQuality', 'group': '解析', 'label': '画質の上限', 'type': 'enum', 'default': '360',
     'options': [['1080', '1080p'], ['720', '720p'], ['480', '480p'], ['360', '360p (最速)']]},
    {'key': 'downloadDir', 'group': '解析', 'label': '動画の保存先', 'type': 'path', 'default': ''},
    {'key': 'ytdlpCookies', 'group': '解析', 'label': 'YouTube Cookieファイル', 'type': 'path', 'default': '',
     'note': '「Botではないことを確認」で取得に失敗する場合、ブラウザ拡張で書き出した cookies.txt を指定します'},
    {'key': 'downloadVideo', 'group': '解析', 'label': 'YouTubeの動画をPCに保存', 'type': 'bool',
     'default': False,
     'note': 'オンにすると、解析後に音声も取得して映像と結合し、PCに保存します（追加画面のチェックの初期値）'},
    {'key': 'analysisProfile', 'group': '解析', 'label': '解析モデル', 'type': 'enum', 'default': 'fast320',
     'options': [['standard', '標準 (1080p)'], ['fast320', '高速 (360p)'], ['other-game', 'ぷよテト以外']],
     'note': '変えると次回の解析は最初からやり直しになります'},
    {'key': 'analysisDevice', 'group': '解析', 'label': '精度モード', 'type': 'enum', 'default': 'gpu',
     'options': [['gpu', 'GPU'], ['cpu', 'CPU'], ['auto', '自動']],
     'note': '変えると次回の解析は最初からやり直しになります'},
    {'key': 'analysisOutputDir', 'group': '解析', 'label': '解析の出力先', 'type': 'path', 'default': ''},
    {'key': 'useRamDisk', 'group': '解析', 'label': 'RAMディスクを使う', 'type': 'bool', 'default': False},
    {'key': 'detailedLog', 'group': '解析', 'label': '詳細ログを残す', 'type': 'bool', 'default': False},
    {'key': 'autoApplyNames', 'group': '名前とタグ', 'label': 'プレイヤー名の記憶を自動適用', 'type': 'bool', 'default': True},
    {'key': 'autoTags', 'group': '名前とタグ', 'label': '自動タグを付ける', 'type': 'bool', 'default': True},
    {'key': 'videoStorage', 'group': 'データ管理', 'label': '動画ファイルの扱い', 'type': 'enum', 'default': 'reference',
     'options': [['reference', '元の場所を参照'], ['copy', 'アプリ内にコピー']]},
    {'key': 'missingVideoAction', 'group': 'データ管理', 'label': '元ファイルが見つからないとき', 'type': 'enum',
     'default': 'warn', 'options': [['warn', '警告を出す'], ['refetch', '自動で再取得'], ['ignore', '無視']]},
]
SETTING_DEFAULTS = {item['key']: item['default'] for item in SETTINGS_SCHEMA}
SETTING_TYPES = {item['key']: item['type'] for item in SETTINGS_SCHEMA}

PROFILES = []
try:
    PROFILES = json.loads((Path(__file__).with_name('profiles.json')).read_text(encoding='utf-8')).get('profiles') or []
except (OSError, json.JSONDecodeError):
    PROFILES = []


ROOT_RUNTIME = Path(__file__).resolve().parents[1] / 'video-recovery-runtime'
VERSION = '0.1.0'
CROP_DIR = Path(os.environ.get('LOCALAPPDATA') or Path.home()) / 'PPTLab' / 'name-crops'

VIDEO_EXTENSIONS = {'.mp4', '.mkv', '.webm', '.mov', '.avi', '.ts', '.m4v', '.flv'}
from urllib.parse import parse_qs, unquote, urlsplit

APP_ROOT = Path(__file__).resolve().parent
TOOLS_ROOT = APP_ROOT.parent
RUNTIME = TOOLS_ROOT / 'video-recovery-runtime'
WEB_ROOT = APP_ROOT / 'web'
# 画面は公開版と同じ /ppt-lab/ に置き、盤面ビューワー（F/）とシミュレータは
# 同じサイトのもの（＝ネットに取りに行かない）を配信する。
# ルート側は GitHub Pages と同じビルド（dist/pages）を優先して使う。
SITE_ROOT = APP_ROOT.parents[1]
SITE_BUILD = SITE_ROOT / 'dist' / 'pages'
LAB_PREFIX = '/ppt-lab'
SITE_DIRS = ('F', 'hub', 'simulator', 'shared', 'styles', 'icons', 'ai-lab', 'Load PPT')
SITE_FILES = ('index.html', 'manifest.webmanifest', 'sw.js')
SITE_EXTRA = ('tools/tetris-lab/preview-theme.css',)
SITE_TYPES = {'.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm',
              '.json': 'application/json', '.webmanifest': 'application/manifest+json',
              '.svg': 'image/svg+xml', '.woff2': 'font/woff2', '.onnx': 'application/octet-stream'}
DEFAULT_DATA = Path(os.environ.get('LOCALAPPDATA', Path.home())) / 'PPTLab'
VERSION = '0.1.0'
STARTED_AT = time.time()

sys.path.insert(0, str(APP_ROOT))
from store import Store  # noqa: E402
from jobs import JobManager, ensure_ram_disk, ram_disk_info  # noqa: E402
from jobs import batch_folder_name, find_ram_disk  # noqa: E402
from namereview import NameReview, read_player_names, apply_names  # noqa: E402
from namereview import SAME_IMAGE_THRESHOLD, _correlation, crop_vector  # noqa: E402
from namereview import save_name_crops  # noqa: E402
# 表記ゆれの畳み方と「いちばん多い表記」は namereview と共通（同じ規則でまとめる）
from namereview import fold_name, most_common, name_groups  # noqa: E402

STORE: Store | None = None
JOBS: JobManager | None = None
REVIEW: NameReview | None = None
_RAM_SETUP = threading.Event()


# ---- 動画の削除（DB の行と、それに紐づくファイルをまとめて消す）-----------
# 解析結果（runtime\data\batches\<バッチ>）とダウンロード動画
# （<データ>\media\<video_id> / R:\PPTLab\media\<video_id>）を消す。
# 消し先は必ず「基準フォルダの直下」だけを許可する（.. や絶対パスで外に出さない）。
BATCHES_ROOT = RUNTIME / 'data' / 'batches'
MB = 1024 * 1024


def cleanup_log_path() -> Path:
    """削除・点検の記録先（データフォルダ配下）。"""
    base = Path(JOBS.data) if JOBS is not None else DEFAULT_DATA
    return base / 'logs' / 'cleanup.log'


def log_cleanup(message: str) -> None:
    """削除の結果を stderr とログファイルの両方に残す（失敗しても処理は続ける）。"""
    line = f'{time.strftime("%Y-%m-%d %H:%M:%S")} {message}'
    sys.stderr.write(line + '\n')
    try:
        path = cleanup_log_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('a', encoding='utf-8') as handle:
            handle.write(line + '\n')
    except OSError:
        pass


def direct_child(base: Path, candidate: Path) -> Path | None:
    """base の**直下**のフォルダだけを返す（外を指していたら None）。"""
    try:
        base_path = Path(base).resolve()
        target = Path(candidate).resolve()
    except (OSError, RuntimeError):
        return None
    if target == base_path or target.parent != base_path:
        return None
    if target.name in ('', '.', '..'):
        return None
    return target


def remove_tree(target: Path) -> str:
    """フォルダを丸ごと消す。消せなかったときだけ理由を返す。"""
    try:
        shutil.rmtree(target)
        return ''
    except OSError as first:
        problem = str(first)
    # 読み取り専用のファイルが混じっていると消せないので、属性を外して一度だけやり直す
    for root, dirs, files in os.walk(target, topdown=False):
        for name in list(files) + list(dirs):
            try:
                os.chmod(os.path.join(root, name), stat.S_IWRITE)
            except OSError:
                pass
    try:
        shutil.rmtree(target)
        return ''
    except OSError as error:
        return str(error) or problem


def media_roots() -> list[Path]:
    """ダウンロード動画を置き得るフォルダ（重複なし）。

    通常のデータフォルダ、設定 downloadDir、RAM ディスク（R:\\PPTLab\\media）を見る。
    """
    roots: list[Path] = [Path(JOBS.data if JOBS is not None else DEFAULT_DATA) / 'media']
    try:
        configured = str(STORE.settings().get('downloadDir') or '').strip() if STORE else ''
    except Exception:
        configured = ''
    if configured:
        roots.append(Path(configured))
    try:
        ram = find_ram_disk()
    except Exception:
        ram = None
    if ram is not None:
        roots.append(Path(ram) / 'PPTLab' / 'media')
    if Path('R:\\').is_dir():
        roots.append(Path('R:\\PPTLab\\media'))
    unique: list[Path] = []
    seen: set[str] = set()
    for root in roots:
        try:
            key = str(Path(root).resolve()).lower()
        except OSError:
            continue
        if key in seen:
            continue
        seen.add(key)
        unique.append(Path(root))
    return unique


def batch_candidates(video: dict) -> list[Path]:
    """その動画の解析結果フォルダの候補（manifest 優先、無ければタイトルから推定）。"""
    manifest = str(video.get('manifest') or '').strip()
    if manifest:
        path = Path(manifest)
        if path.name.lower() != 'batch-results.json':
            # 取り込み（imported\...\manifest.json）はバッチではないので対象外
            return []
        if path.parent.is_dir():
            return [path.parent]
        # manifest の置き場所ごと消えている → タイトルから推定する
    title = str(video.get('title') or '').strip() or str(video.get('id') or '').strip()
    if not title:
        return []
    # 解析側（jobs.batch_folder_name）と同じ規則で名前を推定する。
    # タイトルは 40 文字に切り、末尾に動画IDを残す（表示タイトルは DB のまま）。
    return [BATCHES_ROOT / batch_folder_name(
        title, source=str(video.get('source') or ''),
        video_id=str(video.get('id') or ''), root=BATCHES_ROOT)]


def batch_used_by_other(video_id: str, target: Path) -> str:
    """対象フォルダを別の動画が参照していたら、その動画の id を返す。"""
    assert STORE is not None
    for row in STORE.videos():
        if row['id'] == video_id:
            continue
        manifest = str(row.get('manifest') or '').strip()
        if not manifest:
            continue
        try:
            path = Path(manifest).resolve()
        except OSError:
            continue
        if target == path.parent or target in path.parents:
            return str(row['id'])
    return ''


def purge_video_files(video: dict) -> dict:
    """動画に紐づくファイルを消す（DB の行を消したあとに呼ぶ）。

    消せないものがあっても例外にせず、`warnings` に入れて返す。
    """
    video_id = str(video.get('id') or '').strip()
    removed: list[str] = []
    warnings: list[str] = []
    freed = 0

    # 解析が走っている最中に書き込み先を消さない（DB 行はもう消えている）
    active = ''
    try:
        job = JOBS.latest_for_video(video_id) if JOBS is not None else None
        if job and job.get('status') in ('queued', 'analyzing'):
            active = str(job.get('status'))
    except Exception:
        active = ''
    if active:
        message = f'解析中（{active}）のためファイルは残しました: {video_id}'
        log_cleanup(f'skip(active) id={video_id} status={active}')
        return {'removed': removed, 'warnings': [message], 'freedBytes': freed}

    # 1) 解析結果（バッチ）のフォルダ
    for candidate in batch_candidates(video):
        target = direct_child(BATCHES_ROOT, candidate)
        if target is None:
            warnings.append(f'バッチの場所が想定外のため削除しません: {candidate}')
            log_cleanup(f'skip(outside) id={video_id} path={candidate}')
            continue
        if not target.is_dir():
            continue
        owner = batch_used_by_other(video_id, target)
        if owner:
            warnings.append(f'別の動画（{owner}）が使っているため残しました: {target}')
            log_cleanup(f'skip(shared) id={video_id} with={owner} path={target}')
            continue
        size = _dir_size(target)
        problem = remove_tree(target)
        if problem:
            warnings.append(f'バッチを消せませんでした: {target.name}（{problem}）')
            log_cleanup(f'batch failed id={video_id} path={target} error={problem}')
        else:
            removed.append(str(target))
            freed += size
            log_cleanup(f'batch removed id={video_id} path={target} mb={size / MB:.1f}')

    # 2) ダウンロードした動画（データフォルダ / RAM ディスク）
    if video_id:
        for root in media_roots():
            target = direct_child(root, Path(root) / video_id)
            if target is None or target.name != video_id:
                warnings.append(f'動画フォルダの場所が不正です: {root} / {video_id}')
                log_cleanup(f'skip(outside) id={video_id} path={root} / {video_id}')
                continue
            if not target.is_dir():
                continue
            size = _dir_size(target)
            problem = remove_tree(target)
            if problem:
                warnings.append(f'動画フォルダを消せませんでした: {target}（{problem}）')
                log_cleanup(f'media failed id={video_id} path={target} error={problem}')
            else:
                removed.append(str(target))
                freed += size
                log_cleanup(f'media removed id={video_id} path={target} mb={size / MB:.1f}')

    return {'removed': removed, 'warnings': warnings, 'freedBytes': freed}


def orphan_batches() -> list[dict]:
    """videos から参照されていないバッチを列挙する（**消さない**）。"""
    assert STORE is not None
    referenced: set[str] = set()
    for row in STORE.videos():
        manifest = str(row.get('manifest') or '').strip()
        if not manifest:
            continue
        try:
            path = Path(manifest).resolve()
        except OSError:
            continue
        if path.name.lower() == 'batch-results.json':
            referenced.add(str(path.parent).lower())
    result: list[dict] = []
    if not BATCHES_ROOT.is_dir():
        return result
    for item in sorted(BATCHES_ROOT.iterdir()):
        if not item.is_dir():
            continue
        try:
            key = str(item.resolve()).lower()
        except OSError:
            continue
        if key in referenced:
            continue
        result.append({'path': str(item), 'mb': round(_dir_size(item) / MB, 1)})
    return result


def report_orphan_batches() -> list[dict]:
    """起動時に未参照バッチを調べる（**自動削除はしない**。一覧をログに残すだけ）。"""
    orphans = orphan_batches()
    total = sum(item['mb'] for item in orphans)
    log_cleanup(f'orphan-scan dirs={len(orphans)} mb={total:.1f} '
                '（自動削除はしません。一覧は --orphans で確認できます）')
    return orphans


def scan_orphans_quietly() -> None:
    """起動直後に裏で点検する（失敗してもサーバーは止めない）。"""
    try:
        report_orphan_batches()
    except Exception as error:
        log_cleanup(f'orphan-scan failed error={error!r}')


def ram_disk_note(info: dict) -> str:
    """設定画面の「RAMディスクを使う」に出す注記（状態で書き分ける）。"""
    if info.get('available'):
        total = (info.get('totalMB') or 0) / 1024
        free = (info.get('freeMB') or 0) / 1024
        return (f'{info.get("drive") or ""} 使用中（{total:.1f}GB 中 空き {free:.1f}GB）。'
                '動画はここだけに置き、SSD には書きません')
    if info.get('busy'):
        return '作成しています…（管理者の確認が出たら「はい」を選んでください）'
    if info.get('error'):
        return f'{info["error"]}。もう一度オンにすると再試行します'
    if info.get('task'):
        return 'オンにすると自動作成します（ログオン時の自動再作成も登録済み）'
    return ('オンにすると管理者の確認（UAC）が1回出て、RAM ディスクを作成します。'
            '以後はログオン時に自動で作り直します')


def settings_schema() -> list[dict]:
    """設定画面のスキーマ。RAM ディスクの行だけ、いまの状態を注記に入れる。"""
    info = ram_disk_info()
    rows = []
    for item in SETTINGS_SCHEMA:
        row = dict(item)
        if row.get('key') == 'useRamDisk':
            row['note'] = ram_disk_note(info)
        rows.append(row)
    return rows


def start_ram_disk_setup(wait_seconds: float = 180.0) -> bool:
    """RAM ディスクの用意を裏で始める（UAC が出るのは初回だけ）。"""
    info = ram_disk_info()
    if info.get('available') or info.get('busy') or _RAM_SETUP.is_set():
        return False
    _RAM_SETUP.set()

    def work() -> None:
        try:
            ensure_ram_disk(wait_seconds=wait_seconds, elevate=True)
        finally:
            _RAM_SETUP.clear()

    threading.Thread(target=work, daemon=True, name='ramdisk-setup').start()
    return True


def engine_path() -> Path:
    return RUNTIME / 'bin' / 'TetrisVideoRecovery_stream.exe'


def site_root() -> Path:
    """公開版（GitHub Pages）と同じビルドがあればそれを使う。無ければリポジトリ直下。"""
    if (SITE_BUILD / 'index.html').is_file():
        return SITE_BUILD.resolve()
    return SITE_ROOT.resolve()


def site_build_stale(marker: Path) -> bool:
    try:
        stamp = marker.stat().st_mtime
    except OSError:
        return True
    # sw.js / hub/sw.js は tools/web-service-worker.js から作る。ここを見落とすと
    # テンプレートを直しても配信中の Service Worker が変わらず、ブラウザは
    # 更新に気づけない（＝古いキャッシュのまま居座る）。
    for name in ('index.html', 'F/index.html', 'hub/index.html', 'manifest.webmanifest',
                 'tools/web-service-worker.js', 'tools/build-web.py'):
        path = SITE_ROOT / name
        try:
            if path.is_file() and path.stat().st_mtime > stamp:
                return True
        except OSError:
            continue
    for folder in ('F/app', 'simulator/app', 'shared', 'styles', 'hub/js'):
        base = SITE_ROOT / folder
        if not base.is_dir():
            continue
        for path in base.rglob('*'):
            try:
                if path.is_file() and path.stat().st_mtime > stamp:
                    return True
            except OSError:
                continue
    return False


def ensure_site_build() -> None:
    """dist/pages が無い／本体より古いときに、公開版と同じビルドを作り直す。

    ビルドは appearance / motion / ラベルの差し込みを含むので、これが無いと
    ローカルのシミュレータだけ古い見た目になる。失敗しても配信は続ける。
    """
    if os.environ.get('PPT_LAB_SKIP_BUILD'):
        return
    script = TOOLS_ROOT / 'build-web.py'
    if not script.is_file():
        return
    marker = SITE_BUILD / 'index.html'
    if marker.is_file() and not site_build_stale(marker):
        return
    try:
        subprocess.run([sys.executable, str(script)], cwd=str(SITE_ROOT), timeout=600,
                       capture_output=True,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except (OSError, subprocess.SubprocessError):
        pass


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
# batch-results.json の試合ごとの解析状態（盤面復元に失敗した試合の表示に使う）
_MATCH_ANALYSIS_CACHE: dict[str, tuple[float, dict[int, dict]]] = {}
# 名前画像の指紋を第一キーにする。グループ数が多すぎる（＝画像の分かれ方が
# 破綻している）ときだけ従来の文字ベースに戻す。
# 実測: 124試合・配信者1人＋対戦相手4人の動画は正しく5組に分かれるため、
# 3では正解を捨ててしまう（MT6 のハロウィン動画で 14表記に分裂した）。
_CROP_VECTORS: dict[str, tuple[tuple[int, int], tuple | None]] = {}


# 表記ゆれの畳み方と「いちばん多い表記」は namereview と共通（同じ規則でまとめる）
_fold_name = fold_name
_most_common = most_common


def _crop_vector(fingerprint: str) -> tuple | None:
    """name-crops/<指紋>.png の比較用ベクトル（無ければ None）。

    画像は 142x11 のサムネイルなので、動画が無くても同じ人かどうかを
    namereview と同じ相関（SAME_IMAGE_THRESHOLD）で判定できる。
    """
    if not fingerprint:
        return None
    path = CROP_DIR / f'{fingerprint}.png'
    try:
        info = path.stat()
    except OSError:
        return None
    stamp = (int(info.st_size), int(info.st_mtime_ns))
    cached = _CROP_VECTORS.get(fingerprint)
    if cached and cached[0] == stamp:
        return cached[1]
    vector = crop_vector(path)
    if len(_CROP_VECTORS) > 4000:        # 増え続けないよう、あふれたら捨てる
        _CROP_VECTORS.clear()
    _CROP_VECTORS[fingerprint] = (stamp, vector)
    return vector


def _group_display_name(group: dict) -> str:
    """名前確認画面に出ている名前（過去の確定名 > 代表名 > 最多の表記）を返す。

    画面の入力欄は `entry.guess || entry.name` を出している。タグも同じ名前で
    揃えないと「画面では ぶぶ、タグは JS」のようにズレる。
    """
    def clean(text: str) -> str:
        text = str(text or '').strip()
        # 「???」「?????」のような OCR 失敗の残骸は名前として扱わない。
        return '' if text.strip('?？ ') == '' else text

    variants = {clean(item.get('text')) for item in group.get('variants') or []}
    variants.discard('')
    guess = clean(group.get('guess'))
    if guess and guess not in variants:
        return guess
    name = clean(group.get('name'))
    if name:
        return name
    counts: dict[str, int] = {}
    for item in group.get('variants') or []:
        text = clean(item.get('text'))
        if text:
            counts[text] = counts.get(text, 0) + int(item.get('count') or 0)
    if not counts:
        return ''
    return sorted(counts.items(), key=lambda item: (-item[1], item[0]))[0][0]


def _image_player_names(manifest: Path, video_id: str = '') -> list[str] | None:
    """名前画像の指紋で同じ人をまとめ、画面に出ている名前を返す。

    指紋がまだ無い解析（この機能より前の解析）でも、元の動画が残っていれば
    その場で切り出す（初回だけ数秒〜十数秒かかり、結果は次回から使う）。
    画像が1つも取れない動画だけ None を返し、呼び出し側を従来の文字ベースに戻す。
    人数の上限は設けない（グループ数を理由に正解を捨てない）。
    """
    # 名前確認画面と同じ組・同じ名前を使う（過去の確定名が OCR より優先）。
    if video_id and REVIEW is not None:
        try:
            payload = REVIEW.load(video_id, manifest)
        except Exception:
            payload = None
        names: list[str] = []
        for group in (payload or {}).get('groups') or []:
            name = _group_display_name(group)
            if name and name not in names:
                names.append(name)
        if names:
            return names
    try:
        rows = save_name_crops(manifest, find_ffmpeg(), CROP_DIR).get('rows') or []
    except Exception:
        return None
    if not rows:
        return None
    # まとめ方は名前確認画面（NameReview）とまったく同じ（namereview.name_groups）。
    entries = name_groups(rows, _crop_vector)
    if not any(entry['group'] for entry in entries):
        return None
    names = []
    for entry in entries:
        name = entry.get('representative') or _most_common(entry['texts'])
        if name and name not in names:
            names.append(name)
    return names or None


def group_guesses(groups: list[dict], matches: list[dict]) -> list[dict]:
    """組（＝画面の1エントリ）ごとの候補名と「この動画だけ」の印を作り直す。

    「この動画だけ」の上書きは試合の行（p1Guess / p1Override）に載るので、
    組の表示もその行に合わせる（保存の形式は変えない）。
    """
    rows = {int(row.get('index') or 0): row for row in matches}
    result = []
    for group in groups:
        item = dict(group)
        guesses: list[str] = []
        override = False
        for record in group.get('records') or []:
            row = rows.get(int(record.get('index') or 0)) or {}
            side = str(record.get('side') or '')
            guess = str(row.get(side + 'Guess') or '')
            if guess and guess not in guesses:
                guesses.append(guess)
            if row.get(side + 'Override'):
                override = True
        item['guess'] = guesses[0] if guesses else ''
        item['override'] = override
        result.append(item)
    return result


def video_players(manifest: str, video_id: str = '') -> list[str]:
    """登場プレイヤー名。名前を確認済みならその名前を使い、OCR の揺れは畳む。

    名前画像（解析直後に切り出した <指紋>.png）が残っていれば、それを第一キーに
    同じ人をまとめる。画像が1枚も無い動画だけ、従来の文字ベースにする
    （人数の上限は設けない。名前確認画面とも同じまとめ方を使う）。
    """
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

    # 1) 名前画像（解析直後に切り出した <指紋>.png）を第一キーにする。
    #    動画を消した後でも、残した画像と指紋だけで同じ人をまとめられる。
    image_names = _image_player_names(path, video_id)
    if image_names:
        _PLAYER_CACHE[manifest] = (stamp, image_names)
        return image_names

    # 2) 画像が無いときは、確認済みの名前（autosave）を優先する
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

    # 3) 画像もautosaveも無いときは、OCR 名を文字で畳む（従来どおり）
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

def manifest_player_names(manifest: str) -> set[str]:
    """manifest に入っている OCR 名（修正前の名前も含む）。"""
    if not manifest:
        return set()
    try:
        data = json.loads(Path(manifest).read_text(encoding='utf-8-sig'))
    except (OSError, json.JSONDecodeError):
        return set()
    names: set[str] = set()
    for match in data.get('matches') or []:
        for side in ('p1', 'p2'):
            value = (match.get('player_names') or {}).get(side)
            text = value.get('text') if isinstance(value, dict) else value
            text = str(text or '').strip()
            if text:
                names.add(text)
    return names


def _read_quality_warnings(manifest: Path, data: dict) -> dict[int, dict]:
    """試合ごとの品質警告（quality_warning）を index -> dict で返す。

    batch-results.json の各試合に入っている。古い解析結果など本体に無いときだけ、
    同じフォルダーの quality-warnings.json を読む（解析結果は読むだけ）。
    """
    warnings: dict[int, dict] = {}
    matches = data.get('matches') or []
    for item in matches:
        try:
            index = int(item.get('index') or 0)
        except (TypeError, ValueError):
            continue
        warning = item.get('quality_warning')
        if index and isinstance(warning, dict):
            warnings[index] = warning
    if len(warnings) >= len(matches):
        return warnings
    fallback = manifest.parent / 'quality-warnings.json'
    if not fallback.is_file():
        return warnings
    try:
        payload = json.loads(fallback.read_text(encoding='utf-8-sig'))
    except (OSError, json.JSONDecodeError):
        return warnings
    for item in payload.get('matches') or []:
        try:
            index = int(item.get('index') or 0)
        except (TypeError, ValueError):
            continue
        warning = item.get('quality_warning')
        if index and index not in warnings and isinstance(warning, dict):
            warnings[index] = warning
    return warnings


def match_analysis_info(manifest: Path) -> dict[int, dict]:
    """batch-results.json の試合ごとの解析状態を返す（読むだけで書き換えない）。

    index -> {'status': str, 'opening': bool, 'quality': dict}
    - status  : complete / needs_correction / failed
    - opening : 開始盤面のガードが効かなかった試合。start_decision.final_rule が
                'forced-empty-opening-export' のとき True（開始直後の盤面を読み違え、
                ゴミが乗った状態から解析が始まっている）。ガード成功は
                'empty-opening-guard-target' なので False。
    - quality : quality_warning（suspicious は試合の途中で一致率が低い）。
    """
    try:
        stamp = manifest.stat().st_mtime
    except OSError:
        return {}
    key = str(manifest)
    cached = _MATCH_ANALYSIS_CACHE.get(key)
    if cached and cached[0] == stamp:
        return cached[1]
    try:
        data = json.loads(manifest.read_text(encoding='utf-8-sig'))
    except (OSError, json.JSONDecodeError):
        return {}
    quality = _read_quality_warnings(manifest, data)
    info: dict[int, dict] = {}
    for item in data.get('matches') or []:
        try:
            index = int(item.get('index') or 0)
        except (TypeError, ValueError):
            continue
        if not index:
            continue
        decision = item.get('start_decision') or {}
        info[index] = {
            'status': str(item.get('status') or ''),
            'opening': str(decision.get('final_rule') or '') == 'forced-empty-opening-export',
            'quality': quality.get(index) or {}
        }
    _MATCH_ANALYSIS_CACHE[key] = (stamp, info)
    return info


def match_status_map(manifest: Path) -> dict[int, str]:
    """（互換用）batch-results.json の matches[].status を index -> status で返す。"""
    return {index: item['status']
            for index, item in match_analysis_info(manifest).items()
            if item.get('status')}


# 開始盤面の読み取り誤り（ガードが効かなかった試合）の短い説明
OPENING_FAILED_REASON = '開始時の盤面が空でない（読み取り誤り）'


def describe_quality_warning(warning: dict) -> str:
    """品質警告を、画面にも出せる短い日本語にする（試合の途中の崩れも対象）。"""
    issues = [item for item in (warning.get('issues') or []) if isinstance(item, dict)]
    if not issues:
        return '盤面と読み取った盤面の一致率が低い区間があります'

    def start_of(item) -> float:
        try:
            return float(item.get('relative_start_seconds') or 0)
        except (TypeError, ValueError):
            return 0.0

    def ratio_of(item) -> float:
        try:
            return float(item.get('minimum_match_ratio') or 1.0)
        except (TypeError, ValueError):
            return 1.0

    # いちばん一致率が低い区間を例に出す（同じなら早い方）。
    first = min(issues, key=lambda item: (ratio_of(item), start_of(item)))
    parts: list[str] = []
    try:
        player = int(first.get('player') or 0)
    except (TypeError, ValueError):
        player = 0
    if player in (1, 2):
        parts.append(f'P{player}')
    seconds = start_of(first)
    if seconds > 0:
        parts.append(f'{seconds:.1f}秒〜')
    try:
        ratio = float(first.get('minimum_match_ratio') or 0)
    except (TypeError, ValueError):
        ratio = 0.0
    if ratio > 0:
        parts.append(f'最低一致率 {ratio * 100:.0f}%')
    detail = f'（{"・".join(parts)}）' if parts else ''
    return f'試合の途中で盤面の一致率が低くなっています{detail}'


def mark_analysis_status(data: dict, manifest: Path) -> dict:
    """試合ごとに解析状態を足す（既存のフィールドは変えない）。

    - status が complete 以外: failed=true（盤面データが無い）。要修正は
      analysisStatus='needs_correction'、それ以外は 'failed'。
    - start_decision.final_rule が forced-empty-opening-export: 開始盤面の読み取り
      誤り。盤面データは残っているので failed は変えず 'opening_failed'。
    - quality_warning.suspicious: 試合の途中で盤面の一致率が低い。'quality_failed'。
    - analysisReason に画面に出せる短い日本語を入れる（複数の原因は「。」で連結）。
    status を持たない解析結果（Lab から取り込んだものなど）には何も足さない。
    """
    info = match_analysis_info(manifest)
    if not info:
        return data
    for row in data.get('matches') or []:
        try:
            index = int(row.get('index') or 0)
        except (TypeError, ValueError):
            continue
        item = info.get(index)
        if not item:
            continue
        status = str(item.get('status') or '')
        quality = item.get('quality') or {}
        suspicious = bool(quality.get('suspicious'))
        opening = bool(item.get('opening'))
        if not status and not suspicious and not opening:
            continue
        reasons: list[str] = []
        failed = bool(status and status != 'complete')
        if failed:
            reasons.append('盤面復元が途中で止まっています（要修正）'
                           if status == 'needs_correction' else '盤面復元に失敗しました')
        if opening:
            reasons.append(OPENING_FAILED_REASON)
        if suspicious:
            reasons.append(describe_quality_warning(quality))
        if status:
            row['failed'] = failed
            if failed:
                row['analysisStatus'] = ('needs_correction'
                                         if status == 'needs_correction' else 'failed')
            else:
                row['analysisStatus'] = 'complete'
        row.pop('analysisReason', None)
        if opening and not failed:
            row['analysisStatus'] = 'opening_failed'
        if suspicious and not failed and not opening:
            row['analysisStatus'] = 'quality_failed'
        if reasons:
            row['analysisReason'] = '。'.join(reasons)
    return data


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
    # Lab は試合ごとに3つのURLを持つ。p1 / p2 は片側だけの1Pコレクションで、
    # 両者の盤面が入っているのは combined（2P）だけ。盤面ビューワーには
    # combined を渡さないと P1 しか表示されない。
    combined = str(simulator.get('combined') or '')
    p1 = str(simulator.get('p1') or '')
    p2 = str(simulator.get('p2') or '')
    if not combined and not p1 and not p2:
        return None
    state = decode_state_hash(combined) or decode_state_hash(p1) or decode_state_hash(p2)
    return {
        'urls': {'combined': combined or p1 or p2, 'p1': p1, 'p2': p2},
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
        if not combined:
            return None
        folder = Path(combined).parent
        if not folder.is_dir() or folder.name != 'simulator':
            return None
        files = [f for f in sorted(folder.glob('*.json')) if f.name != 'profiles.json']
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
            if path == LAB_PREFIX:
                # 画面は /ppt-lab/ に置いた（相対パスが公開版と同じになる）
                return self.redirect(LAB_PREFIX + '/')
            if path.startswith(LAB_PREFIX + '/'):
                return self.serve_static(path[len(LAB_PREFIX):])
            return self.serve_site(path)
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
        if path == '/api/ramdisk':
            return self.send_json(ram_disk_info())
        if path == '/api/videos':
            videos = STORE.videos()
            auto_tags = bool(STORE.settings().get('autoTags', True))
            for video in videos:
                players = video_players(video.get('manifest', ''), str(video.get('id') or ''))
                video['players'] = players
                auto = ['動画解析'] + [name for name in players if name]
                # 自動タグは毎回作り直す。名前を直したあとに残る OCR の旧名もここで外す。
                if auto_tags:
                    STORE.reconcile_tags(video['id'], auto,
                                         manifest_player_names(video.get('manifest', '')))
                stored = STORE.tags_for(video['id'])
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
            if auto_tags:
                STORE.prune_tags()
            return self.send_json({'videos': videos})
        if path == '/api/tetofu':
            return self.send_json({'tetofu': STORE.tetofu()})
        if path == '/api/names/rename':
            body = self.read_json() or {}
            old = str(body.get('from') or '').strip()
            new = str(body.get('to') or '').strip()
            if not old or not new:
                raise ValueError('変更前と変更後の名前を入れてください')
            changed = STORE.rename_name(old, new)
            return self.send_json({'ok': True, 'changed': changed})
        if path == '/api/names/merge':
            body = self.read_json() or {}
            source = str(body.get('from') or '').strip()
            target = str(body.get('to') or '').strip()
            if not source or not target:
                raise ValueError('統合する名前を2つ選んでください')
            changed = STORE.rename_name(source, target)
            return self.send_json({'ok': True, 'changed': changed})
        if path == '/api/data/info':
            return self.send_json(data_info())
        if path == '/api/settings/schema':
            return self.send_json({'schema': settings_schema()})
        if path == '/api/settings':
            values = dict(SETTING_DEFAULTS)
            values.update({k: v for k, v in STORE.settings().items() if k in SETTING_TYPES})
            return self.send_json({'settings': values})
        if path == '/api/analysis/profiles':
            return self.send_json({'profiles': PROFILES, 'active': STORE.settings().get('analysisProfile', 'standard')})
        if path == '/api/data/cache':
            body = self.read_json() or {}
            kind = str(body.get('kind') or '')
            freed = 0
            if kind == 'crops':
                if CROP_DIR.exists():
                    for item in CROP_DIR.glob('*.png'):
                        try:
                            freed += item.stat().st_size
                            item.unlink()
                        except OSError:
                            pass
            elif kind == 'analysis':
                batches = ROOT_RUNTIME / 'data' / 'batches'
                for item in list(batches.rglob('analysis-cache.v1.json')) if batches.exists() else []:
                    try:
                        freed += item.stat().st_size
                        item.unlink()
                    except OSError:
                        pass
            else:
                raise ValueError('対象の指定が正しくありません')
            return self.send_json({'ok': True, 'freed': freed})
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
            manifest = Path(video['manifest'])
            query = parse_qs(urlsplit(self.path).query)
            if query.get('light', [''])[0] == '1':
                return self.send_json(mark_analysis_status(
                    REVIEW.light(video_id, manifest), manifest))
            data = REVIEW.load(video_id, manifest)
            overrides = STORE.name_overrides_for(video_id)
            if overrides:
                for row in data.get('matches', []):
                    for side in ('p1', 'p2'):
                        fingerprint = row.get(side + 'Fingerprint')
                        if fingerprint and fingerprint in overrides:
                            row[side + 'Guess'] = overrides[fingerprint]
                            row[side + 'Override'] = True
                # 組（＝1エントリ）の候補名も、いま反映した上書きに合わせる
                data = dict(data)
                data['groups'] = group_guesses(data.get('groups') or [],
                                               data.get('matches') or [])
            return self.send_json(mark_analysis_status(data, manifest))
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
        if path == '/api/ramdisk':
            body = self.read_json() or {}
            if str(body.get('action') or 'create') == 'create':
                started = start_ram_disk_setup()
                return self.send_json({'ok': True, 'started': started, 'info': ram_disk_info()})
            return self.send_json({'ok': True, 'info': ram_disk_info()})
        if path == '/api/settings':
            body = self.read_json() or {}
            for key, value in body.items():
                kind = SETTING_TYPES.get(str(key))
                if kind is None:
                    continue
                if kind == 'bool':
                    value = bool(value)
                elif kind == 'int':
                    value = int(value)
                else:
                    value = str(value)
                STORE.set_setting(str(key), value)
            if str(body.get('useRamDisk', '')).strip().lower() in ('true', '1', 'on', 'yes'):
                # オンにした瞬間に用意を始める（初回だけ UAC、以後は登録済みタスク）
                start_ram_disk_setup()
            values = dict(SETTING_DEFAULTS)
            values.update({k: v for k, v in STORE.settings().items() if k in SETTING_TYPES})
            return self.send_json({'ok': True, 'settings': values})
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
                auto = {'動画解析', *[name for name in
                                      video_players(video.get('manifest', ''), str(video.get('id') or ''))
                                      if name]}
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
        if path.startswith('/api/tetofu/') and path.endswith('/title'):
            tetofu_id = path[len('/api/tetofu/'):-len('/title')]
            if not STORE.tetofu_item(tetofu_id):
                return self.send_error_json('テト譜が見つかりません', 404)
            body = self.read_json()
            title = str(body.get('title', '')).strip()
            if not title:
                raise ValueError('タイトルを入力してください')
            STORE.set_tetofu_title(tetofu_id, title)
            return self.send_json({'id': tetofu_id, 'title': title})
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
            save_video = body.get('saveVideo')
            if save_video is None:
                save_video = bool(STORE.settings().get('downloadVideo', True))
            elif isinstance(save_video, str):
                save_video = save_video.strip().lower() not in ('0', 'false', 'off', 'no', '')
            else:
                save_video = bool(save_video)
            video = STORE.add_video(title, source)
            job_id = JOBS.start(video['id'], source, save_video) if JOBS else ''
            return self.send_json({'id': video['id'], 'jobId': job_id,
                                   'status': 'queued', 'saveVideo': save_video})
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
            players_before = set(video_players(str(video['manifest']), video_id)) if cleaned else set()
            result = apply_names(Path(video['manifest']), cleaned) if cleaned else {'changed': 0}
            # 名前が変わったら自動タグも付け替える（0件になったタグは消す）
            if cleaned:
                try:
                    _PLAYER_CACHE.pop(str(video['manifest']), None)
                    players_after = set(video_players(str(video['manifest']), video_id))
                    for name in players_before - players_after:
                        STORE.remove_tag_link(video_id, name)
                    for name in players_after - players_before:
                        STORE.materialize_tags(video_id, [name])
                    STORE.prune_tags()
                except Exception:
                    pass
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
            video = STORE.video(video_id)
            if not video:
                return self.send_error_json('動画が見つかりません', 404)
            # DB の行を先に消す。ファイルが使用中で消せなくても、動画は消えたままにする。
            STORE.delete_video(video_id)
            try:
                cleanup = purge_video_files(video)
            except Exception as error:      # 想定外でも削除そのものは成功させる
                cleanup = {'removed': [], 'warnings': [str(error)], 'freedBytes': 0}
                log_cleanup(f'purge crashed id={video_id} error={error!r}')
            return self.send_json({'ok': True, 'id': video_id,
                                   'removed': cleanup['removed'],
                                   'removedMB': round(cleanup['freedBytes'] / MB, 1),
                                   'warnings': cleanup['warnings']})
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
        # /ppt-lab/ は「直したら次に開いたとき必ず出る」画面。ブラウザの
        # HTTP キャッシュに残すと、Service Worker を外しても古い HTML /
        # JS / CSS が返るので、毎回取りに来させる（HTML だけでなく JS /
        # CSS / 画像も同じ扱いでよい。ローカル配信なので費用は小さい）。
        self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args) -> None:
        if os.environ.get('PPT_LAB_VERBOSE'):
            sys.stderr.write('%s - %s\n' % (self.address_string(), fmt % args))

    def redirect(self, location: str) -> None:
        self.send_response(302)
        self.send_header('Location', location)
        self.send_header('Content-Length', '0')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()

    # ---- 本体（ビューワー / シミュレータ）----------------------------------
    def serve_site(self, path: str) -> None:
        """シミュレータ / ビューワー / ハブを配信する（公開版と同じビルドを優先）。"""
        relative = path.lstrip('/') or 'index.html'
        if relative.endswith('/'):
            relative += 'index.html'
        first = relative.split('/', 1)[0]
        if relative not in SITE_FILES and relative not in SITE_EXTRA and first not in SITE_DIRS:
            return self.send_error_json('ファイルがありません', 404)
        root = site_root()
        target = (root / relative).resolve()
        if root not in target.parents:
            return self.send_error_json('参照先が不正です', 403)
        if not target.is_file() and root is not SITE_ROOT.resolve():
            # ビルドに無いものはリポジトリ直下から拾う
            fallback = (SITE_ROOT.resolve() / relative).resolve()
            if SITE_ROOT.resolve() in fallback.parents and fallback.is_file():
                target = fallback
        if not target.is_file():
            return self.send_error_json('ファイルがありません', 404)
        body = target.read_bytes()
        mime = SITE_TYPES.get(target.suffix.lower())
        if not mime:
            mime, _ = mimetypes.guess_type(target.name)
        self.send_response(200)
        self.send_header('Content-Type', mime or 'application/octet-stream')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)


def make_visual_name_recall(crop_dir: Path, store, threshold: float = 0.985):
    """過去の配信で確定した名前画像と、いま切り出した名前画像を照合する。

    指紋は画素の完全一致ハッシュなので、配信が変わって画素が1つでもずれると
    当たらない（実測: 同じ人でも配信ごとに別ハッシュになる）。ここでは
    142x11 のサムネイルを z-score 正規化したベクトル同士の相関で見る。
    実測で同じ人は 0.996 前後、別人は 0.5 以下なので 0.985 を境にする。
    """
    from namereview import SAME_IMAGE_THRESHOLD, THUMB_HEIGHT, THUMB_WIDTH

    limit = threshold if threshold is not None else SAME_IMAGE_THRESHOLD
    state = {'stamp': None, 'names': (), 'matrix': None}
    lock = threading.Lock()

    def snapshot():
        """crop_dir の中身が変わったときだけ読み直す（毎回の走査を避ける）。"""
        try:
            files = sorted(crop_dir.glob('*.png'))
        except OSError:
            files = []
        stamp = (str(crop_dir), len(files),
                 max((f.stat().st_mtime_ns for f in files), default=0))
        with lock:
            if state['stamp'] == stamp:
                return state
            state.update({'stamp': stamp, 'names': (), 'matrix': None})
            if not files:
                return state
            try:
                import numpy as np
                from PIL import Image
            except Exception:
                return state
            rows = []
            fingerprints = []
            for path in files:
                try:
                    with Image.open(path) as image:
                        thumb = image.convert('L').resize(
                            (THUMB_WIDTH, THUMB_HEIGHT), Image.LANCZOS)
                        pixels = np.asarray(thumb, dtype=np.float32).reshape(-1)
                except (OSError, ValueError):
                    continue
                deviation = float(pixels.std())
                if deviation < 1.0:  # _normalize と同じ判定（平坦な画像は捨てる）
                    continue
                rows.append((pixels - pixels.mean()) / deviation)
                fingerprints.append(path.stem)
            named = store.names_for_fingerprints(fingerprints) if rows else {}
            keep = [i for i, fingerprint in enumerate(fingerprints) if named.get(fingerprint)]
            if not keep:
                return state
            state['matrix'] = np.stack([rows[i] for i in keep])
            state['names'] = tuple(named[fingerprints[i]] for i in keep)
            return state

    def recall(vector) -> str | None:
        if vector is None:
            return None
        data = snapshot()
        matrix = data['matrix'] if data else None
        if matrix is None:
            return None
        try:
            import numpy as np
            query = np.asarray(vector, dtype=np.float32).reshape(-1)
        except Exception:
            return None
        if query.size != matrix.shape[1]:
            return None
        scores = matrix @ query / query.size  # _correlation と同じ「積の平均」
        best = int(scores.argmax())
        if float(scores[best]) < limit:
            return None
        return data['names'][best]

    return recall


def main() -> int:
    global STORE, JOBS, REVIEW
    parser = argparse.ArgumentParser(description='PPT Lab local server')
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--data', type=Path, default=DEFAULT_DATA)
    parser.add_argument('--open', action='store_true')
    parser.add_argument('--orphans', action='store_true',
                        help='videos から参照されていないバッチを一覧する（削除はしません）')
    args = parser.parse_args()

    STORE = Store(args.data)
    STORE.reset_stuck_videos()
    JOBS = JobManager(STORE, args.data, TOOLS_ROOT, RUNTIME)
    if args.orphans:
        # 一覧を出すだけ。**ここでは何も消さない**（誤爆を避けるため）。
        print(json.dumps(orphan_batches(), ensure_ascii=False, indent=1), flush=True)
        return 0
    REVIEW = NameReview(args.data / 'cache', find_ffmpeg(), STORE.recall_name,
                        STORE.recall_name_by_text,
                        make_visual_name_recall(CROP_DIR, STORE), crop_dir=CROP_DIR)
    ensure_site_build()
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    url = f'http://127.0.0.1:{server.server_port}/ppt-lab/'
    print(url, flush=True)
    # 未参照バッチの点検は裏で走らせる（一覧をログに残すだけで、削除はしない）
    threading.Thread(target=scan_orphans_quietly, daemon=True,
                     name='ppt-orphan-scan').start()
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
