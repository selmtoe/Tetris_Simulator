"""PPT Lab の解析モデル（`.tetracemodel`）の一覧・導入・削除。

モデルは `models\\<id>\\` に置き、`manifest.json` に素性（表示名・版・
ファイル・sha256）を書く。配布形式 `.tetracemodel` は中身が ZIP で、
ルートに `manifest.json`、同じ階層に `*.onnx` を持つ。

    models/
      fast320/                  ← 組み込み（表示名 Tetrace-V1.0-PPT）
        manifest.json
        scene-classifier.onnx
        tetris.onnx
        next_hold.onnx

`install_package()` は受け取った ZIP を検証してから展開する。壊れた入力
（サイズ超過・manifest 欠落・sha256 不一致・id 不正・zip-slip）は
`ModelError`（ValueError の派生）で弾く。呼び出し側（server.py）は
そのメッセージをそのまま画面に出せる。
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import re
import shutil
import uuid
import zipfile
from pathlib import Path

MB = 1024 * 1024

MODELS_DIR = Path(__file__).resolve().parent / 'models'
MANIFEST_NAME = 'manifest.json'
SCHEMA = 'tetrace-model/v1'
ONNX_SUFFIX = '.onnx'
# manifest の files に書ける種別（scene と board は必須、next は任意）。
FILE_KINDS = ('scene', 'board', 'next')
REQUIRED_KINDS = ('scene', 'board')

# 組み込みモデル。物理フォルダ名は fast320 のままだが、表示名と id はこちら。
DEFAULT_MODEL_ID = 'tetrace-v1.0-ppt'
# 旧プロファイル id（settings.analysisProfile に残っている値）の読み替え。
LEGACY_MODEL_IDS = {'standard': DEFAULT_MODEL_ID, 'fast320': DEFAULT_MODEL_ID}

# 配布ファイル（ZIP）の上限と、展開後の合計サイズの上限（zip 爆弾よけ）。
MAX_PACKAGE_BYTES = 200 * MB
MAX_UNCOMPRESSED_BYTES = 200 * MB
MODEL_ID_MAX_CHARS = 64
NAME_MAX_CHARS = 80

# id はフォルダ名になる。仕様の「英小文字と - _」に加えて、組み込み id
# `tetrace-v1.0-ppt` に合わせて数字と `.` も許可する（`..` は拒否）。
MODEL_ID_PATTERN = re.compile(r'^[a-z0-9][a-z0-9._-]*$')
# Windows でフォルダ名に使えない名前。
RESERVED_NAMES = ({'con', 'prn', 'aux', 'nul'}
                  | {f'com{n}' for n in range(1, 10)}
                  | {f'lpt{n}' for n in range(1, 10)})
SHA256_PATTERN = re.compile(r'^[0-9a-f]{64}$')


class ModelError(ValueError):
    """利用者に見せる失敗（日本語の理由つき）。"""


# ---- 名前の検証 -----------------------------------------------------------
def _clean_name(value) -> str:
    """パス区切りを `/` にそろえ、前後の空白を落とす。"""
    return str(value or '').replace('\\', '/').strip()


def _is_absolute(name: str) -> bool:
    """絶対パス（`/x` や `C:x`）かどうか。"""
    return name.startswith('/') or bool(re.match(r'^[A-Za-z]:', name))


def _valid_member(value) -> str:
    """ZIP のエントリ名を検証し、ルート直下のファイル名だけを返す。

    `..` を含むもの・絶対パス・サブフォルダは zip-slip になるので拒否する。
    """
    name = _clean_name(value)
    if not name or _is_absolute(name) or any(ord(char) < 32 for char in name):
        raise ModelError(f'パッケージ内のパスが不正です: {value!r}')
    parts = name.split('/')
    if len(parts) != 1 or parts[0] in ('', '.', '..'):
        raise ModelError(
            f'パッケージ内のパスが不正です: {value!r}'
            '（manifest.json と *.onnx は ZIP のルートに置いてください）')
    return parts[0]


def _valid_file_ref(value) -> str:
    """manifest の files に書かれたファイル名を検証する。"""
    name = _clean_name(value)
    if not name or _is_absolute(name) or '/' in name:
        raise ModelError(f'モデルのファイル名が正しくありません: {value!r}')
    if name in ('.', '..') or name.startswith('..'):
        raise ModelError(f'モデルのファイル名が正しくありません: {value!r}')
    if not name.lower().endswith(ONNX_SUFFIX):
        raise ModelError(
            f'モデルのファイルは {ONNX_SUFFIX} でなければなりません: {name}')
    return name


def _validate_model_id(value) -> str:
    """モデル id（＝フォルダ名）を検証する。"""
    model_id = str(value or '').strip()
    if not model_id:
        raise ModelError('manifest.json に id がありません')
    if len(model_id) > MODEL_ID_MAX_CHARS:
        raise ModelError(f'id が長すぎます（{MODEL_ID_MAX_CHARS} 文字まで）')
    if (not MODEL_ID_PATTERN.match(model_id) or '..' in model_id
            or model_id.endswith(('.', ' '))):
        raise ModelError(
            'id が正しくありません（英小文字・数字・- _ . のみ、先頭は英数字）: '
            + model_id)
    if model_id in RESERVED_NAMES:
        raise ModelError(f'id に使えない名前です: {model_id}')
    return model_id


def _safe_child(root: Path, name: str) -> Path:
    """root 直下のパスを作る（検証済みの名前だけを渡すこと）。"""
    if not name or name in ('.', '..') or '/' in name or '\\' in name:
        raise ModelError(f'モデルのフォルダ名が正しくありません: {name!r}')
    target = root / name
    try:
        if target.resolve().parent != root.resolve():
            raise ModelError(f'モデルのフォルダ名が正しくありません: {name!r}')
    except OSError:
        pass
    return target


# ---- manifest の読み取り --------------------------------------------------
def _read_model(folder: Path) -> dict | None:
    """1フォルダ分の manifest を読み、一覧に出す形にする（不正なら None）。"""
    try:
        raw = (folder / MANIFEST_NAME).read_text(encoding='utf-8-sig')
        manifest = json.loads(raw)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(manifest, dict) or manifest.get('schema') != SCHEMA:
        return None
    model_id = str(manifest.get('id') or '').strip()
    if not MODEL_ID_PATTERN.match(model_id) or '..' in model_id:
        return None
    declared = manifest.get('files')
    if not isinstance(declared, dict):
        return None
    files = {}
    for kind, value in declared.items():
        if str(kind) not in FILE_KINDS:
            continue
        try:
            files[str(kind)] = _valid_file_ref(value)
        except ModelError:
            return None                     # 危険なパスを含むモデルは一覧に出さない
    size = 0
    for filename in dict.fromkeys(files.values()):
        path = folder / filename
        try:
            if path.is_file():
                size += path.stat().st_size
        except OSError:
            pass
    name = str(manifest.get('name') or '').strip() or model_id
    return {
        'id': model_id,
        'name': name,
        'version': str(manifest.get('version') or '').strip(),
        'description': str(manifest.get('description') or '').strip(),
        'author': str(manifest.get('author') or '').strip(),
        'license': str(manifest.get('license') or '').strip(),
        'createdAt': str(manifest.get('createdAt') or '').strip(),
        'files': files,
        'sha256': manifest.get('sha256') if isinstance(manifest.get('sha256'), dict) else {},
        'builtin': bool(manifest.get('builtin')),
        'dir': str(folder),
        'dirname': folder.name,
        'size': size,
    }


def list_models(models_dir=None) -> list[dict]:
    """インストール済みモデルを返す（組み込みが先頭、続いて表示名順）。"""
    root = Path(models_dir) if models_dir is not None else MODELS_DIR
    try:
        folders = [item for item in root.iterdir() if item.is_dir()]
    except OSError:
        return []
    models = []
    for folder in sorted(folders, key=lambda item: item.name.lower()):
        if folder.name.startswith('.'):          # インストール途中の作業フォルダ
            continue
        model = _read_model(folder)
        if model is not None:
            models.append(model)
    models.sort(key=lambda item: (not item.get('builtin'),
                                  str(item.get('name') or '').lower()))
    return models


def find_model(models_dir, model_id) -> dict | None:
    """id からモデル1件を探す（組み込みはフォルダ名と id が違うため走査する）。"""
    text = str(model_id or '').strip()
    if not text:
        return None
    for item in list_models(models_dir):
        if item['id'] == text:
            return item
    return None


def canonical_model_id(model_id) -> str:
    """旧プロファイル id をモデル id に読み替える（空なら既定）。"""
    text = str(model_id or '').strip()
    if not text:
        return DEFAULT_MODEL_ID
    return LEGACY_MODEL_IDS.get(text, text)


def model_profile(models_dir=None, model_id=None) -> dict | None:
    """解析エンジンに渡すモデルのパス一式（見つからなければ None）。"""
    model = find_model(models_dir, model_id)
    if model is None:
        return None
    folder = Path(model['dir'])
    files = dict(model.get('files') or {})
    profile = {
        'id': model['id'],
        'label': model['name'],
        'note': model.get('description', ''),
        'modelId': model['id'],
        'settings': '',
        'env': {},
    }
    for kind, key in (('scene', 'sceneModel'), ('board', 'boardModel'),
                      ('next', 'nextModel')):
        filename = files.get(kind)
        if filename:
            profile[key] = str(folder / filename)
    return profile


# ---- パッケージの検証 -----------------------------------------------------
def _read_entry(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> bytes:
    try:
        return archive.read(info)
    except (OSError, RuntimeError, zipfile.BadZipFile) as error:
        raise ModelError(
            f'パッケージを読み取れません: {getattr(info, "filename", "")}') from error


def _read_entries(archive: zipfile.ZipFile) -> tuple[dict, list[str]]:
    """ZIP のエントリ名を検証して、ルート直下のファイルだけを集める。"""
    entries: dict[str, zipfile.ZipInfo] = {}
    warnings: list[str] = []
    total = 0
    for info in archive.infolist():
        raw = str(info.filename or '')
        normalized = raw.replace('\\', '/')
        if info.is_dir() or normalized.endswith('/'):
            continue
        # macOS の ZIP が入れる補助ファイルは黙って捨てる。
        if normalized.startswith('__MACOSX/') or any(
                part.startswith('._') for part in normalized.split('/')):
            warnings.append(f'次のファイルは使いません: {raw}')
            continue
        name = _valid_member(normalized)
        if name in entries:
            raise ModelError(f'パッケージ内に同じ名前のファイルが複数あります: {name}')
        total += int(info.file_size)
        if total > MAX_UNCOMPRESSED_BYTES:
            raise ModelError(
                f'展開後のサイズが大きすぎます。上限は {MAX_UNCOMPRESSED_BYTES / MB:.0f}MB です')
        entries[name] = info
    if not entries:
        raise ModelError('パッケージにファイルがありません')
    return entries, warnings


def _validate_files(declared, entries: dict, warnings: list[str]) -> dict:
    """manifest の files を検証し、種別 → ファイル名の辞書にする。"""
    if not isinstance(declared, dict):
        raise ModelError('manifest.json に files がありません')
    files: dict[str, str] = {}
    for kind, value in declared.items():
        kind_text = str(kind or '').strip()
        filename = _valid_file_ref(value)
        if kind_text not in FILE_KINDS:
            warnings.append(f'未知のファイル種別は使いません: {kind_text}')
            continue
        if filename not in entries:
            raise ModelError(f'パッケージにファイルがありません: {filename}')
        files[kind_text] = filename
    for kind in REQUIRED_KINDS:
        if kind not in files:
            raise ModelError(
                f'manifest.json の files に {kind} がありません（scene と board は必須です）')
    return files


def _verify_hashes(archive: zipfile.ZipFile, manifest: dict,
                   files: dict, entries: dict) -> dict:
    """宣言された sha256 と実際のファイル内容を照合する。"""
    declared = manifest.get('sha256')
    if not isinstance(declared, dict):
        raise ModelError('manifest.json に sha256 がありません')
    verified: dict[str, str] = {}
    for filename in dict.fromkeys(files.values()):
        expected = str(declared.get(filename) or '').strip().lower()
        if not expected:
            raise ModelError(f'sha256 がありません: {filename}')
        if not SHA256_PATTERN.match(expected):
            raise ModelError(f'sha256 の形式が正しくありません: {filename}')
        actual = hashlib.sha256(_read_entry(archive, entries[filename])).hexdigest()
        if actual != expected:
            raise ModelError(
                f'sha256 が一致しません: {filename}（ファイルが壊れているか、別の版です）')
        verified[filename] = actual
    return verified


def _validate_package(archive: zipfile.ZipFile, entries: dict,
                      warnings: list[str]) -> dict:
    """manifest を検証し、保存用に正規化した dict を返す。"""
    if MANIFEST_NAME not in entries:
        raise ModelError('manifest.json がありません（ZIP のルートに置いてください）')
    raw = _read_entry(archive, entries[MANIFEST_NAME])
    try:
        manifest = json.loads(raw.decode('utf-8-sig'))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ModelError('manifest.json を読み取れません（UTF-8 の JSON にしてください）') from error
    if not isinstance(manifest, dict):
        raise ModelError('manifest.json の形式が正しくありません')
    if str(manifest.get('schema') or '') != SCHEMA:
        raise ModelError(
            f'schema が {SCHEMA} ではありません: {manifest.get("schema")!r}')
    model_id = _validate_model_id(manifest.get('id'))
    display_name = str(manifest.get('name') or '').strip()
    if not display_name:
        raise ModelError('manifest.json に name（モデルの表示名）がありません')
    if len(display_name) > NAME_MAX_CHARS:
        raise ModelError(f'name が長すぎます（{NAME_MAX_CHARS} 文字まで）')
    files = _validate_files(manifest.get('files'), entries, warnings)
    hashes = _verify_hashes(archive, manifest, files, entries)
    # manifest から参照されていないファイルは展開しない（警告だけ残す）。
    used = set(files.values()) | {MANIFEST_NAME}
    for filename in sorted(entries):
        if filename not in used:
            warnings.append(f'次のファイルは使いません: {filename}')
    if manifest.get('builtin'):
        # 配布パッケージを「組み込み」と名乗らせない（削除できなくなるため）。
        warnings.append('パッケージの builtin 指定は無視しました（組み込み扱いにはしません）')
    return {
        'schema': SCHEMA,
        'id': model_id,
        'name': display_name,
        'version': str(manifest.get('version') or '').strip(),
        'description': str(manifest.get('description') or '').strip(),
        'author': str(manifest.get('author') or '').strip(),
        'license': str(manifest.get('license') or '').strip(),
        'createdAt': str(manifest.get('createdAt') or '').strip(),
        'files': files,
        'sha256': hashes,
        'builtin': False,
    }


def _inspect_package(zip_bytes) -> tuple[zipfile.ZipFile, dict, dict, list[str]]:
    """ZIP のバイト列を検証し、(開いた ZIP, manifest, エントリ, 警告) を返す。"""
    if not isinstance(zip_bytes, (bytes, bytearray, memoryview)):
        raise ModelError('パッケージの内容を読み取れませんでした')
    data = bytes(zip_bytes)
    if not data:
        raise ModelError('ファイルが空です（.tetracemodel を選んでください）')
    if len(data) > MAX_PACKAGE_BYTES:
        raise ModelError(
            f'パッケージが大きすぎます（{len(data) / MB:.1f}MB）。'
            f'上限は {MAX_PACKAGE_BYTES / MB:.0f}MB です')
    try:
        archive = zipfile.ZipFile(io.BytesIO(data))
    except (zipfile.BadZipFile, OSError) as error:
        raise ModelError(
            'ZIP として読み取れません（.tetracemodel を選んでください）') from error
    try:
        entries, warnings = _read_entries(archive)
        model = _validate_package(archive, entries, warnings)
    except BaseException:
        archive.close()
        raise
    return archive, model, entries, warnings


# ---- 導入・削除 -----------------------------------------------------------
def install_package(zip_bytes, models_dir=None, replace: bool = False) -> dict:
    """`.tetracemodel`（ZIP のバイト列）を検証して `models\\<id>\\` に展開する。

    - 既存 id は上書きしない（`replace=True` のときだけ置き換える）
    - 組み込み（builtin）モデルは置き換えない
    - 検証に失敗したら何も残さない（一時フォルダは必ず片付ける）
    """
    root = Path(models_dir) if models_dir is not None else MODELS_DIR
    archive, model, entries, warnings = _inspect_package(zip_bytes)
    try:
        existing = find_model(root, model['id'])
        if existing is not None:
            if existing.get('builtin'):
                raise ModelError(
                    f'組み込みモデルは置き換えられません: {existing.get("name") or existing["id"]}')
            if not replace:
                raise ModelError(
                    f'モデル {model["id"]} はすでにインストールされています'
                    '（置き換えるときは replace を指定してください）')
        folder_name = model['id'] if existing is None else str(existing.get('dirname') or model['id'])
        target = _safe_child(root, folder_name)
        if target.exists() and existing is None:
            # manifest が壊れていて一覧に出ないフォルダ。黙って消さずに断る。
            raise ModelError(
                f'保存先のフォルダがすでにあります: {target.name}'
                '（中身を確認してから削除してください）')
        try:
            root.mkdir(parents=True, exist_ok=True)
        except OSError as error:
            raise ModelError(f'モデルの保存先を作れません: {root}') from error
        staging = root / ('.installing-' + uuid.uuid4().hex)
        try:
            staging.mkdir()
        except OSError as error:
            raise ModelError(f'モデルの保存先を作れません: {root}') from error
        try:
            for filename in dict.fromkeys(model['files'].values()):
                (staging / filename).write_bytes(
                    _read_entry(archive, entries[filename]))
            (staging / MANIFEST_NAME).write_text(
                json.dumps(model, ensure_ascii=False, indent=2) + '\n',
                encoding='utf-8')
            if target.exists():
                shutil.rmtree(target)
            os.replace(staging, target)
        except OSError as error:
            shutil.rmtree(staging, ignore_errors=True)
            raise ModelError(f'モデルを保存できませんでした: {error}') from error
        except BaseException:
            shutil.rmtree(staging, ignore_errors=True)
            raise
    finally:
        archive.close()
    return {
        'id': model['id'],
        'name': model['name'],
        'version': model['version'],
        'path': str(target),
        'warnings': warnings,
    }


def remove_model(models_dir=None, model_id=None) -> dict:
    """インストール済みモデルを削除する（組み込みは拒否）。"""
    root = Path(models_dir) if models_dir is not None else MODELS_DIR
    model = find_model(root, model_id)
    if model is None:
        raise ModelError(f'モデルが見つかりません: {model_id}')
    if model.get('builtin'):
        raise ModelError(f'組み込みモデルは削除できません: {model.get("name") or model["id"]}')
    target = _safe_child(root, str(model.get('dirname') or model['id']))
    try:
        shutil.rmtree(target)
    except OSError as error:
        raise ModelError(f'モデルを削除できませんでした: {error}') from error
    return {'id': model['id'], 'name': model['name'], 'path': str(target)}
