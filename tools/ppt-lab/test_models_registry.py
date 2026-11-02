"""PPT Lab のモデル管理（models_registry）の検証。

    python -m unittest test_models_registry -v

ネットワークも Lab サーバも使わない（Lab の再起動・解析の実行も不要）。
ZIP をその場で組み立てて、次を確認する。

  1. 組み込みモデル（models\\fast320 = Tetrace-V1.0-PPT）の manifest と実ファイル
  2. 一覧（表示名・サイズ・builtin）と、別スキーマのフォルダを無視すること
  3. 正常なインストール（展開先・上書き禁止・replace での置き換え）
  4. 壊れた入力の拒否（サイズ超過・manifest 欠落・sha256 不一致・id 不正・
     zip-slip・必須ファイル欠落・ZIP でない・展開後サイズ超過）
  5. 削除（組み込みは拒否、未インストールは失敗）
  6. 旧プロファイル id の読み替えと、解析用パスの解決
"""
from __future__ import annotations

import hashlib
import io
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

LAB = Path(__file__).resolve().parent
sys.path.insert(0, str(LAB))

import models_registry as registry  # noqa: E402
import server as lab_server  # noqa: E402
import jobs  # noqa: E402

MODEL_ID = 'sample-model'
SCENE_NAME = 'scene.onnx'
BOARD_NAME = 'board.onnx'
NEXT_NAME = 'next.onnx'
CONTENTS = {
    SCENE_NAME: b'scene-model' * 32,
    BOARD_NAME: b'board-model' * 64,
    NEXT_NAME: b'next-model' * 16,
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def make_manifest(model_id: str = MODEL_ID, extra: dict | None = None,
                  hashes: dict | None = None, **overrides) -> dict:
    """テスト用の manifest（sha256 は CONTENTS から計算）。"""
    manifest = {
        'schema': registry.SCHEMA,
        'id': model_id,
        'name': 'サンプルモデル',
        'version': '0.1',
        'description': 'テスト用のモデル',
        'author': 'test',
        'license': 'test',
        'createdAt': '2026-11-03',
        'files': {'scene': SCENE_NAME, 'board': BOARD_NAME, 'next': NEXT_NAME},
        'sha256': dict(hashes) if hashes is not None else
        {name: sha256(data) for name, data in CONTENTS.items()},
        'builtin': False,
    }
    if extra:
        manifest.update(extra)
    manifest.update(overrides)
    return manifest


def make_package(manifest: dict | None = None,
                 files: dict[str, bytes] | None = None,
                 members: list[tuple[str, bytes]] | None = None) -> bytes:
    """`.tetracemodel`（ZIP）を組み立てる。manifest=None なら入れない。"""
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
        if manifest is not None:
            archive.writestr('manifest.json', json.dumps(manifest, ensure_ascii=False))
        body = dict(files) if files is not None else {
            name: data for name, data in CONTENTS.items()
            if manifest is None or name in (manifest.get('files') or {}).values()}
        for name, data in body.items():
            archive.writestr(name, data)
        for name, data in (members or []):
            archive.writestr(name, data)
    return buffer.getvalue()


class TempModels(unittest.TestCase):
    """空の models フォルダを1テストごとに用意する。"""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.models = self.root / 'models'
        self.models.mkdir()

    def install(self, package: bytes, replace: bool = False) -> dict:
        return registry.install_package(package, self.models, replace=replace)

    def model_dir(self, model_id: str = MODEL_ID) -> Path:
        return self.models / model_id


# ---- 1/2. 組み込みモデル ---------------------------------------------------
class BuiltinModelTest(unittest.TestCase):
    def manifest_path(self) -> Path:
        return registry.MODELS_DIR / 'fast320' / registry.MANIFEST_NAME

    def test_builtin_manifest_matches_real_files(self) -> None:
        manifest = json.loads(self.manifest_path().read_text(encoding='utf-8'))
        self.assertEqual(manifest['schema'], registry.SCHEMA)
        self.assertEqual(manifest['id'], 'tetrace-v1.0-ppt')
        self.assertEqual(manifest['name'], 'Tetrace-V1.0-PPT')
        self.assertEqual(manifest['version'], '1.0')
        self.assertTrue(manifest['builtin'])
        self.assertEqual(manifest['files'],
                         {'scene': 'scene-classifier.onnx', 'board': 'tetris.onnx',
                          'next': 'next_hold.onnx'})
        # sha256 は実ファイルと一致していること（配布物の同一性）。
        for filename, expected in manifest['sha256'].items():
            path = self.manifest_path().parent / filename
            self.assertTrue(path.is_file(), f'{filename} がありません')
            self.assertEqual(sha256(path.read_bytes()), expected, filename)
        self.assertEqual(set(manifest['sha256']), set(manifest['files'].values()))

    def test_listed_with_display_name(self) -> None:
        rows = registry.list_models(registry.MODELS_DIR)
        ids = [row['id'] for row in rows]
        self.assertIn('tetrace-v1.0-ppt', ids)
        self.assertEqual(rows[0]['id'], 'tetrace-v1.0-ppt', '組み込みが先頭に来る')
        row = next(item for item in rows if item['id'] == 'tetrace-v1.0-ppt')
        self.assertEqual(row['name'], 'Tetrace-V1.0-PPT')
        self.assertTrue(row['builtin'])
        self.assertGreater(row['size'], 0)
        for filename in row['files'].values():
            self.assertTrue((Path(row['dir']) / filename).is_file(), filename)

    def test_model_profile_paths(self) -> None:
        profile = registry.model_profile(registry.MODELS_DIR, 'tetrace-v1.0-ppt')
        self.assertIsNotNone(profile)
        folder = registry.MODELS_DIR / 'fast320'
        self.assertEqual(profile['label'], 'Tetrace-V1.0-PPT')
        self.assertEqual(Path(profile['sceneModel']),
                         folder / 'scene-classifier.onnx')
        self.assertEqual(Path(profile['boardModel']), folder / 'tetris.onnx')
        self.assertEqual(Path(profile['nextModel']), folder / 'next_hold.onnx')

    def test_other_schema_folder_is_ignored(self) -> None:
        """別スキーマ（end3）の manifest はモデル一覧に出さない。"""
        end3 = registry.MODELS_DIR / 'end3' / registry.MANIFEST_NAME
        if not end3.is_file():
            self.skipTest('end3 が無い環境')
        ids = [row['id'] for row in registry.list_models(registry.MODELS_DIR)]
        self.assertNotIn('end3', ids)

    def test_legacy_ids_map_to_builtin(self) -> None:
        self.assertEqual(registry.canonical_model_id('fast320'), 'tetrace-v1.0-ppt')
        self.assertEqual(registry.canonical_model_id('standard'), 'tetrace-v1.0-ppt')
        self.assertEqual(registry.canonical_model_id('tetrace-v1.0-ppt'),
                         'tetrace-v1.0-ppt')
        self.assertEqual(registry.canonical_model_id('other-game'), 'other-game')
        self.assertEqual(registry.canonical_model_id(''), 'tetrace-v1.0-ppt')


# ---- 3. 正常なインストール -------------------------------------------------
class InstallTest(TempModels):
    def test_install_and_list(self) -> None:
        result = self.install(make_package(make_manifest()))
        self.assertEqual(result['id'], MODEL_ID)
        self.assertEqual(result['name'], 'サンプルモデル')
        self.assertEqual(result['warnings'], [])
        target = self.model_dir()
        for name, data in CONTENTS.items():
            self.assertEqual((target / name).read_bytes(), data)
        saved = json.loads((target / 'manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(saved['id'], MODEL_ID)
        self.assertFalse(saved['builtin'], '配布パッケージは組み込みにならない')
        rows = registry.list_models(self.models)
        self.assertEqual([row['id'] for row in rows], [MODEL_ID])
        self.assertEqual(rows[0]['name'], 'サンプルモデル')
        self.assertEqual(rows[0]['size'], sum(len(data) for data in CONTENTS.values()))
        self.assertFalse(rows[0]['builtin'])

    def test_model_profile_uses_installed_files(self) -> None:
        self.install(make_package(make_manifest()))
        profile = registry.model_profile(self.models, MODEL_ID)
        self.assertIsNotNone(profile)
        folder = self.model_dir()
        self.assertEqual(Path(profile['sceneModel']), folder / SCENE_NAME)
        self.assertEqual(Path(profile['boardModel']), folder / BOARD_NAME)
        self.assertEqual(Path(profile['nextModel']), folder / NEXT_NAME)

    def test_existing_id_is_not_overwritten(self) -> None:
        self.install(make_package(make_manifest()))
        before = (self.model_dir() / BOARD_NAME).read_bytes()
        changed = dict(CONTENTS)
        changed[BOARD_NAME] = b'other-board' * 8
        manifest = make_manifest(hashes={name: sha256(data) for name, data in changed.items()})
        with self.assertRaises(registry.ModelError) as caught:
            self.install(make_package(manifest, files=changed))
        self.assertIn('すでにインストール', str(caught.exception))
        self.assertEqual((self.model_dir() / BOARD_NAME).read_bytes(), before)

    def test_replace_true_overwrites(self) -> None:
        self.install(make_package(make_manifest()))
        changed = dict(CONTENTS)
        changed[BOARD_NAME] = b'new-board' * 16
        manifest = make_manifest(name='サンプルモデル2',
                                 hashes={name: sha256(data) for name, data in changed.items()})
        result = self.install(make_package(manifest, files=changed), replace=True)
        self.assertEqual(result['name'], 'サンプルモデル2')
        self.assertEqual((self.model_dir() / BOARD_NAME).read_bytes(), changed[BOARD_NAME])
        rows = registry.list_models(self.models)
        self.assertEqual(len(rows), 1, '置き換えで二重に増えない')
        self.assertEqual(rows[0]['name'], 'サンプルモデル2')

    def test_builtin_packages_become_normal_models(self) -> None:
        """builtin: true を名乗る配布物は拒否せず、通常モデルとして入れる。"""
        result = self.install(make_package(make_manifest(builtin=True)))
        self.assertTrue(any('builtin' in warning for warning in result['warnings']))
        saved = json.loads((self.model_dir() / 'manifest.json').read_text(encoding='utf-8'))
        self.assertFalse(saved['builtin'])
        registry.remove_model(self.models, MODEL_ID)      # 削除できること

    def test_uppercase_sha256_is_accepted(self) -> None:
        """pack-model.py は sha256 を大文字で書き出す（そのまま受け入れられること）。"""
        hashes = {name: sha256(data).upper() for name, data in CONTENTS.items()}
        result = self.install(make_package(make_manifest(hashes=hashes)))
        self.assertEqual(result['id'], MODEL_ID)
        saved = json.loads((self.model_dir() / 'manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(saved['sha256'][BOARD_NAME], sha256(CONTENTS[BOARD_NAME]).lower(),
                         '展開後の manifest は小文字にそろえる')

    def test_unknown_entries_are_ignored_with_warning(self) -> None:
        package = make_package(make_manifest(),
                               files=CONTENTS,
                               members=[('readme.txt', b'hello')])
        result = self.install(package)
        self.assertTrue(any('readme.txt' in warning for warning in result['warnings']))
        self.assertFalse((self.model_dir() / 'readme.txt').exists())

    def test_install_creates_missing_models_dir(self) -> None:
        nested = self.root / 'new' / 'models'
        package = make_package(make_manifest())
        registry.install_package(package, nested)
        self.assertTrue((nested / MODEL_ID / BOARD_NAME).is_file())

    def test_install_refuses_to_clobber_broken_folder(self) -> None:
        """一覧に出ない（manifest が壊れた）フォルダは黙って消さない。"""
        broken = self.model_dir()
        broken.mkdir()
        (broken / 'memo.txt').write_text('残すこと', encoding='utf-8')
        with self.assertRaises(registry.ModelError) as caught:
            self.install(make_package(make_manifest()))
        self.assertIn('すでにあります', str(caught.exception))
        self.assertEqual((broken / 'memo.txt').read_text(encoding='utf-8'), '残すこと')

    def test_no_staging_folder_is_left(self) -> None:
        with self.assertRaises(registry.ModelError):
            self.install(make_package(make_manifest(
                hashes={SCENE_NAME: sha256(CONTENTS[SCENE_NAME]),
                        BOARD_NAME: '0' * 64,
                        NEXT_NAME: sha256(CONTENTS[NEXT_NAME])})))
        leftovers = [item.name for item in self.models.iterdir()
                     if item.name.startswith('.installing-')]
        self.assertEqual(leftovers, [])


# ---- 4. 壊れた入力の拒否 ---------------------------------------------------
class InvalidPackageTest(TempModels):
    def test_rejects_empty_and_non_zip(self) -> None:
        for package in (b'', b'not a zip file'):
            with self.subTest(package=package[:12]):
                with self.assertRaises(registry.ModelError):
                    self.install(package)

    def test_rejects_missing_manifest(self) -> None:
        package = make_package(None, files=CONTENTS)
        with self.assertRaises(registry.ModelError) as caught:
            self.install(package)
        self.assertIn('manifest.json', str(caught.exception))

    def test_rejects_broken_manifest(self) -> None:
        package = make_package(make_manifest(schema='other-schema/v1'))
        with self.assertRaises(registry.ModelError) as caught:
            self.install(package)
        self.assertIn('schema', str(caught.exception))

    def test_rejects_sha256_mismatch(self) -> None:
        hashes = {name: sha256(data) for name, data in CONTENTS.items()}
        hashes[BOARD_NAME] = '0' * 64
        package = make_package(make_manifest(hashes=hashes))
        with self.assertRaises(registry.ModelError) as caught:
            self.install(package)
        self.assertIn('sha256', str(caught.exception))
        self.assertFalse(self.model_dir().exists(), '失敗したモデルは残さない')

    def test_rejects_missing_sha256(self) -> None:
        package = make_package(make_manifest(hashes={SCENE_NAME: sha256(CONTENTS[SCENE_NAME])}))
        with self.assertRaises(registry.ModelError) as caught:
            self.install(package)
        self.assertIn('sha256', str(caught.exception))

    def test_rejects_bad_ids(self) -> None:
        for model_id in ('Bad-Id', '../evil', 'abc/def', '', '.hidden', 'a' * 80,
                         'con', 'evil..id'):
            with self.subTest(model_id=model_id):
                with self.assertRaises(registry.ModelError):
                    self.install(make_package(make_manifest(model_id=model_id)))

    def test_rejects_zip_slip_entries(self) -> None:
        for name in ('../evil.onnx', '/absolute.onnx', 'nested/scene.onnx', '..\\evil.onnx'):
            with self.subTest(name=name):
                package = make_package(make_manifest(), files=CONTENTS,
                                       members=[(name, b'evil')])
                with self.assertRaises(registry.ModelError):
                    self.install(package)
                self.assertFalse((self.root / 'evil.onnx').exists())

    def test_rejects_missing_required_kind(self) -> None:
        manifest = make_manifest(files={'scene': SCENE_NAME, 'next': NEXT_NAME})
        with self.assertRaises(registry.ModelError) as caught:
            self.install(make_package(manifest))
        self.assertIn('board', str(caught.exception))

    def test_rejects_declared_file_that_is_missing(self) -> None:
        manifest = make_manifest(files={'scene': SCENE_NAME, 'board': 'missing.onnx'})
        with self.assertRaises(registry.ModelError) as caught:
            self.install(make_package(manifest, files={SCENE_NAME: CONTENTS[SCENE_NAME]}))
        self.assertIn('missing.onnx', str(caught.exception))

    def test_rejects_non_onnx_file(self) -> None:
        manifest = make_manifest(files={'scene': SCENE_NAME, 'board': 'board.txt'})
        with self.assertRaises(registry.ModelError) as caught:
            self.install(make_package(manifest, files={SCENE_NAME: CONTENTS[SCENE_NAME],
                                                       'board.txt': b'x'}))
        self.assertIn('.onnx', str(caught.exception))

    def test_rejects_package_over_size_limit(self) -> None:
        package = make_package(make_manifest())
        with mock.patch.object(registry, 'MAX_PACKAGE_BYTES', len(package) - 1):
            with self.assertRaises(registry.ModelError) as caught:
                self.install(package)
        self.assertIn('大きすぎ', str(caught.exception))

    def test_rejects_uncompressed_size_over_limit(self) -> None:
        package = make_package(make_manifest())
        with mock.patch.object(registry, 'MAX_UNCOMPRESSED_BYTES', 16):
            with self.assertRaises(registry.ModelError) as caught:
                self.install(package)
        self.assertIn('大きすぎ', str(caught.exception))

    def test_rejects_replacing_builtin(self) -> None:
        """組み込み id を名乗るパッケージでは、組み込みを置き換えられない。"""
        package = make_package(make_manifest('tetrace-v1.0-ppt'))
        board = registry.MODELS_DIR / 'fast320' / 'tetris.onnx'
        before = board.read_bytes()
        with self.assertRaises(registry.ModelError) as caught:
            registry.install_package(package, registry.MODELS_DIR, replace=True)
        self.assertIn('組み込み', str(caught.exception))
        with self.assertRaises(registry.ModelError) as caught:
            registry.install_package(package, registry.MODELS_DIR)
        self.assertIn('組み込み', str(caught.exception))
        self.assertEqual(board.read_bytes(), before)
        self.assertFalse((registry.MODELS_DIR / 'tetrace-v1.0-ppt').exists())


# ---- 5. 削除 ---------------------------------------------------------------
class RemoveTest(TempModels):
    def test_remove_installed_model(self) -> None:
        self.install(make_package(make_manifest()))
        removed = registry.remove_model(self.models, MODEL_ID)
        self.assertEqual(removed['id'], MODEL_ID)
        self.assertFalse(self.model_dir().exists())
        self.assertEqual(registry.list_models(self.models), [])

    def test_remove_unknown_model(self) -> None:
        with self.assertRaises(registry.ModelError) as caught:
            registry.remove_model(self.models, 'no-such-model')
        self.assertIn('見つかりません', str(caught.exception))

    def test_remove_builtin_is_refused(self) -> None:
        before = registry.list_models(registry.MODELS_DIR)
        with self.assertRaises(registry.ModelError) as caught:
            registry.remove_model(registry.MODELS_DIR, 'tetrace-v1.0-ppt')
        self.assertIn('組み込み', str(caught.exception))
        self.assertEqual(registry.list_models(registry.MODELS_DIR), before)
        self.assertTrue((registry.MODELS_DIR / 'fast320' / 'tetris.onnx').is_file())


# ---- 6. 一覧の壊れた manifest ---------------------------------------------
class ListModelsTest(TempModels):
    def test_broken_manifests_are_skipped(self) -> None:
        broken = self.models / 'broken'
        broken.mkdir()
        (broken / 'manifest.json').write_text('{ this is not json', encoding='utf-8')
        no_schema = self.models / 'no-schema'
        no_schema.mkdir()
        (no_schema / 'manifest.json').write_text(
            json.dumps({'id': 'no-schema'}, ensure_ascii=False), encoding='utf-8')
        no_manifest = self.models / 'no-manifest'
        no_manifest.mkdir()
        (no_manifest / 'scene.onnx').write_bytes(b'x')
        registry.install_package(make_package(make_manifest()), self.models)
        rows = registry.list_models(self.models)
        self.assertEqual([row['id'] for row in rows], [MODEL_ID])

    def test_missing_models_dir_is_empty(self) -> None:
        self.assertEqual(registry.list_models(self.root / 'nothing'), [])


# ---- 7. API の受け口（server.py の Handler をソケット無しで呼ぶ） -----------
class FakeStore:
    """設定だけを持つ偽ストア（DB を使わない）。"""

    def __init__(self, **values) -> None:
        self.values = dict(values)

    def settings(self) -> dict:
        return dict(self.values)

    def set_setting(self, key: str, value) -> None:
        self.values[key] = value


class FakeHandler(lab_server.Handler):
    """HTTP のソケットを使わず、送受信をメモリで差し替えた Handler。"""

    def __init__(self, body: bytes = b'', headers: dict | None = None) -> None:
        self.rfile = io.BytesIO(body)
        self.wfile = io.BytesIO()
        self.headers = dict(headers or {})
        self.response_status = 0
        self.close_connection = False

    def send_response(self, code, message=None) -> None:
        self.response_status = code

    def send_header(self, key, value) -> None:
        return None

    def end_headers(self) -> None:
        return None

    def json_body(self) -> dict:
        return json.loads(self.wfile.getvalue().decode('utf-8'))


class ModelApiTest(TempModels):
    """GET /api/models、POST /api/models/install・delete の受け口。"""

    def setUp(self) -> None:
        super().setUp()
        self.store = FakeStore(analysisProfile='tetrace-v1.0-ppt')
        self.patches = [mock.patch.object(lab_server, 'STORE', self.store),
                        mock.patch.object(lab_server, 'MODELS_DIR', self.models)]
        for patch in self.patches:
            patch.start()
            self.addCleanup(patch.stop)

    def post(self, path: str, body: bytes = b'',
             headers: dict | None = None) -> FakeHandler:
        """do_POST と同じく、ValueError を 400 の JSON にして返す。"""
        handler = FakeHandler(body, headers)
        try:
            handler.api_post(path)
        except ValueError as error:
            handler.send_error_json(str(error), 400)
        return handler

    def get(self, path: str) -> FakeHandler:
        handler = FakeHandler()
        handler.api_get(path)
        return handler

    def test_list_models(self) -> None:
        registry.install_package(make_package(make_manifest()), self.models)
        handler = self.get('/api/models')
        self.assertEqual(handler.response_status, 200)
        body = handler.json_body()
        self.assertEqual([row['id'] for row in body['models']], [MODEL_ID])
        self.assertEqual(body['models'][0]['name'], 'サンプルモデル')
        self.assertEqual(body['models'][0]['size'], sum(len(v) for v in CONTENTS.values()))
        self.assertEqual(body['active'], 'tetrace-v1.0-ppt')
        # 内部パスは画面に返さない
        self.assertNotIn('dir', body['models'][0])
        self.assertNotIn('dirname', body['models'][0])

    def test_list_models_reads_legacy_active_id(self) -> None:
        self.store.values['analysisProfile'] = 'fast320'
        body = self.get('/api/models').json_body()
        self.assertEqual(body['active'], 'tetrace-v1.0-ppt')

    def test_install_endpoint(self) -> None:
        package = make_package(make_manifest())
        handler = self.post('/api/models/install', package,
                            {'Content-Length': str(len(package))})
        self.assertEqual(handler.response_status, 200)
        body = handler.json_body()
        self.assertTrue(body['ok'])
        self.assertEqual(body['id'], MODEL_ID)
        self.assertEqual(body['name'], 'サンプルモデル')
        self.assertEqual(body['warnings'], [])
        self.assertTrue((self.models / MODEL_ID / BOARD_NAME).is_file())

    def test_install_endpoint_returns_reason(self) -> None:
        hashes = {name: sha256(data) for name, data in CONTENTS.items()}
        hashes[BOARD_NAME] = '0' * 64
        package = make_package(make_manifest(hashes=hashes))
        handler = self.post('/api/models/install', package,
                            {'Content-Length': str(len(package))})
        self.assertEqual(handler.response_status, 400)
        self.assertIn('sha256', handler.json_body()['error'])
        self.assertFalse((self.models / MODEL_ID).exists())

    def test_install_endpoint_requires_content_length(self) -> None:
        package = make_package(make_manifest())
        handler = self.post('/api/models/install', package)
        self.assertEqual(handler.response_status, 400)
        self.assertIn('Content-Length', handler.json_body()['error'])

    def test_install_endpoint_rejects_too_large(self) -> None:
        package = make_package(make_manifest())
        with mock.patch.object(registry, 'MAX_PACKAGE_BYTES', len(package) - 1):
            handler = self.post('/api/models/install', package,
                                {'Content-Length': str(len(package))})
        self.assertEqual(handler.response_status, 413)
        self.assertIn('大きすぎ', handler.json_body()['error'])
        self.assertTrue(handler.close_connection, '413 のあとは接続を閉じる')

    def test_delete_endpoint(self) -> None:
        registry.install_package(make_package(make_manifest()), self.models)
        self.store.values['analysisProfile'] = MODEL_ID
        payload = json.dumps({'id': MODEL_ID}).encode('utf-8')
        handler = self.post('/api/models/delete', payload,
                            {'Content-Type': 'application/json',
                             'Content-Length': str(len(payload))})
        self.assertEqual(handler.response_status, 200)
        self.assertTrue(handler.json_body()['ok'])
        self.assertFalse((self.models / MODEL_ID).exists())
        self.assertEqual(self.store.values['analysisProfile'], 'tetrace-v1.0-ppt',
                         '選択中を消したら既定へ戻す')

    def test_delete_endpoint_refuses_builtin(self) -> None:
        payload = json.dumps({'id': 'tetrace-v1.0-ppt'}).encode('utf-8')
        headers = {'Content-Type': 'application/json',
                   'Content-Length': str(len(payload))}
        with mock.patch.object(lab_server, 'MODELS_DIR', registry.MODELS_DIR):
            handler = self.post('/api/models/delete', payload, headers)
        self.assertEqual(handler.response_status, 400)
        self.assertIn('組み込み', handler.json_body()['error'])

    def test_delete_endpoint_requires_id(self) -> None:
        handler = self.post('/api/models/delete', b'{}',
                            {'Content-Type': 'application/json',
                             'Content-Length': '2'})
        self.assertEqual(handler.response_status, 400)


# ---- 8. jobs 側の解決（モデル id → エンジンに渡すパス）と画質切替 -----------
class ModelPipelineTest(TempModels):
    """jobs.load_profile と jobs._effective_quality の確認。"""

    def test_model_id_resolves_to_installed_files(self) -> None:
        registry.install_package(make_package(make_manifest()), self.models)
        with mock.patch.object(jobs, 'MODELS_DIR', self.models):
            profile = jobs.load_profile(MODEL_ID)
        self.assertIsNotNone(profile)
        self.assertEqual(profile['label'], 'サンプルモデル')
        self.assertEqual(Path(profile['sceneModel']), self.models / MODEL_ID / SCENE_NAME)
        self.assertEqual(Path(profile['boardModel']), self.models / MODEL_ID / BOARD_NAME)

    def test_legacy_ids_use_builtin_model(self) -> None:
        for legacy in ('standard', 'fast320'):
            with self.subTest(legacy=legacy):
                profile = jobs.load_profile(legacy)
                self.assertIsNotNone(profile)
                self.assertEqual(profile['label'], 'Tetrace-V1.0-PPT')
                self.assertEqual(Path(profile['sceneModel']).name,
                                 'scene-classifier.onnx')

    def test_other_game_keeps_profiles_json_behaviour(self) -> None:
        # profiles.json の定義が空なら「従来の既定」＝None（モデルを指定しない）。
        self.assertIsNone(jobs.load_profile('other-game'))

    def test_unknown_id_falls_back_to_installed_model(self) -> None:
        registry.install_package(make_package(make_manifest()), self.models)
        with mock.patch.object(jobs, 'MODELS_DIR', self.models):
            profile = jobs.load_profile('no-such-model')
        self.assertEqual(profile['id'], MODEL_ID)

    def test_quality_switches_by_saving(self) -> None:
        store = FakeStore(downloadQualitySaved='720', downloadQualityStream='480')
        self.assertEqual(jobs._effective_quality(store, saved=True), '720')
        self.assertEqual(jobs._effective_quality(store, saved=False), '480')
        self.assertEqual(jobs._effective_quality(store), '480',
                         '保存しない（既定）は stream 側')

    def test_quality_defaults_when_keys_are_missing(self) -> None:
        # 旧キー downloadQuality しか無い DB でも落ちない（既定へ戻す）。
        legacy = FakeStore(downloadQuality='480')
        self.assertEqual(jobs._effective_quality(legacy, saved=True), '1080')
        self.assertEqual(jobs._effective_quality(legacy, saved=False), '360')
        broken = FakeStore(downloadQualitySaved='9999', downloadQualityStream=None)
        self.assertEqual(jobs._effective_quality(broken, saved=True), '1080')
        self.assertEqual(jobs._effective_quality(broken, saved=False), '360')


if __name__ == '__main__':
    unittest.main(verbosity=2)
