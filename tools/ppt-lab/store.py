"""PPT Lab の保存層（SQLite 1ファイル）。

段階1では「動画・テト譜・タグ・名前記憶・ジョブ」の器だけを用意する。
書き込みは必ずトランザクションで行い、起動時に自動バックアップを取る。
"""
from __future__ import annotations

import contextlib
import datetime as dt
import hashlib
import json
import sqlite3
import threading
import uuid
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS videos (
    id          TEXT PRIMARY KEY,
    title       TEXT NOT NULL,
    status      TEXT NOT NULL DEFAULT 'queued',
    source      TEXT NOT NULL DEFAULT '',
    created_at  TEXT NOT NULL,
    match_count INTEGER NOT NULL DEFAULT 0,
    manifest    TEXT NOT NULL DEFAULT '',
    tags_locked INTEGER NOT NULL DEFAULT 0,
    data        TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS tetofu (
    id         TEXT PRIMARY KEY,
    kind       TEXT NOT NULL,
    title      TEXT NOT NULL,
    url        TEXT NOT NULL,
    created_at TEXT NOT NULL,
    data       TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS tags (
    name       TEXT PRIMARY KEY,
    created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS tag_links (
    tag        TEXT NOT NULL,
    target     TEXT NOT NULL,
    created_at TEXT NOT NULL,
    PRIMARY KEY (tag, target)
);
CREATE INDEX IF NOT EXISTS tag_links_target ON tag_links(target);
CREATE TABLE IF NOT EXISTS name_memory (
    fingerprint TEXT PRIMARY KEY,
    name        TEXT NOT NULL,
    updated_at  TEXT NOT NULL,
    hits        INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS settings (
    key        TEXT PRIMARY KEY,
    value      TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS name_overrides (
    video_id    TEXT NOT NULL,
    fingerprint TEXT NOT NULL,
    name        TEXT NOT NULL,
    updated_at  TEXT NOT NULL,
    PRIMARY KEY (video_id, fingerprint)
);
CREATE TABLE IF NOT EXISTS jobs (
    id         TEXT PRIMARY KEY,
    kind       TEXT NOT NULL,
    status     TEXT NOT NULL,
    created_at TEXT NOT NULL,
    data       TEXT NOT NULL DEFAULT '{}'
);
"""

VIDEO_STATUSES = ('queued', 'analyzing', 'needs-names', 'ready', 'failed')


def now() -> str:
    return dt.datetime.now().isoformat(timespec='seconds')


def new_id(prefix: str) -> str:
    return prefix + '-' + uuid.uuid4().hex[:16]


class Store:
    """1ファイルの SQLite。読み書きは短いトランザクションで完結させる。"""

    def __init__(self, data_dir: Path):
        self.data = Path(data_dir)
        self.data.mkdir(parents=True, exist_ok=True)
        self.db_path = self.data / 'pptlab.sqlite3'
        self._lock = threading.Lock()
        self.backup()
        with self.connect() as db:
            db.executescript(SCHEMA)
            columns = {row['name'] for row in db.execute('PRAGMA table_info(videos)')}
            if 'manifest' not in columns:
                db.execute("ALTER TABLE videos ADD COLUMN manifest TEXT NOT NULL DEFAULT ''")
            if 'tags_locked' not in columns:
                db.execute("ALTER TABLE videos ADD COLUMN tags_locked INTEGER NOT NULL DEFAULT 0")
    # ---- 基盤 -------------------------------------------------------------
    @contextlib.contextmanager
    def connect(self):
        db = sqlite3.connect(self.db_path, timeout=15)
        db.row_factory = sqlite3.Row
        try:
            yield db
            db.commit()
        except Exception:
            db.rollback()
            raise
        finally:
            db.close()

    def backup(self, keep: int = 5) -> None:
        """起動時に1回だけ、直近のDBを退避する（壊れた時の復旧用）。"""
        if not self.db_path.is_file():
            return
        folder = self.data / 'backups'
        folder.mkdir(exist_ok=True)
        target = folder / (dt.datetime.now().strftime('%Y%m%d-%H%M%S') + '-pptlab.sqlite3')
        with contextlib.closing(sqlite3.connect(self.db_path)) as source, \
                contextlib.closing(sqlite3.connect(target)) as dest:
            source.backup(dest)
        for old in sorted(folder.glob('*-pptlab.sqlite3'))[:-keep]:
            old.unlink(missing_ok=True)

    # ---- 動画 -------------------------------------------------------------
    def videos(self) -> list[dict]:
        with self.connect() as db:
            rows = db.execute('SELECT * FROM videos ORDER BY created_at DESC').fetchall()
        return [{'id': r['id'], 'title': r['title'], 'status': r['status'],
                 'source': r['source'], 'createdAt': r['created_at'],
                 'matchCount': r['match_count'], 'manifest': r['manifest'],
                 'tags_locked': r['tags_locked']} for r in rows]

    def add_video(self, title: str, source: str, status: str = 'queued') -> dict:
        if status not in VIDEO_STATUSES:
            raise ValueError('動画の状態が不正です')
        identifier = new_id('video')
        with self.connect() as db:
            db.execute('INSERT INTO videos(id,title,status,source,created_at) VALUES(?,?,?,?,?)',
                       (identifier, title, status, source, now()))
        return {'id': identifier, 'title': title, 'status': status, 'source': source,
                'createdAt': now(), 'matchCount': 0}

    def set_video_title(self, identifier: str, title: str) -> None:
        title = (title or '').strip()
        if not title:
            raise ValueError('タイトルを入力してください')
        if len(title) > 200:
            raise ValueError('タイトルは200文字までです')
        with self.connect() as db:
            db.execute('UPDATE videos SET title=? WHERE id=?', (title, identifier))
    def delete_video(self, identifier: str) -> None:
        with self.connect() as db:
            db.execute('DELETE FROM tag_links WHERE target=?', (identifier,))
            db.execute('DELETE FROM videos WHERE id=?', (identifier,))

    def delete_tetofu(self, identifier: str) -> None:
        with self.connect() as db:
            db.execute('DELETE FROM tag_links WHERE target=?', (identifier,))
            db.execute('DELETE FROM tetofu WHERE id=?', (identifier,))
    # ---- 設定 -------------------------------------------------------------
    def settings(self) -> dict:
        with self.connect() as db:
            rows = db.execute('SELECT key, value FROM settings').fetchall()
        return {row['key']: row['value'] for row in rows}

    def set_setting(self, key: str, value: str) -> None:
        with self.connect() as db:
            db.execute('INSERT INTO settings(key, value, updated_at) VALUES(?,?,?) '
                       'ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at',
                       (key, value, now()))

    # ---- 名前の対応 -------------------------------------------------------
    def name_mappings(self) -> list[dict]:
        with self.connect() as db:
            rows = db.execute('SELECT fingerprint, name, updated_at, hits FROM name_memory '
                              'ORDER BY updated_at DESC').fetchall()
        return [{'fingerprint': row['fingerprint'], 'name': row['name'],
                 'updatedAt': row['updated_at'], 'hits': row['hits']} for row in rows]

    def forget_name(self, fingerprint: str) -> bool:
        with self.connect() as db:
            removed = db.execute('DELETE FROM name_memory WHERE fingerprint=?', (fingerprint,)).rowcount
        return bool(removed)

    def set_name_override(self, video_id: str, fingerprint: str, name: str) -> None:
        with self.connect() as db:
            db.execute('INSERT INTO name_overrides(video_id, fingerprint, name, updated_at) VALUES(?,?,?,?) '
                       'ON CONFLICT(video_id, fingerprint) DO UPDATE SET name=excluded.name, updated_at=excluded.updated_at',
                       (video_id, fingerprint, name, now()))

    def clear_name_override(self, video_id: str, fingerprint: str) -> None:
        with self.connect() as db:
            db.execute('DELETE FROM name_overrides WHERE video_id=? AND fingerprint=?', (video_id, fingerprint))

    def name_overrides_for(self, video_id: str) -> dict:
        with self.connect() as db:
            rows = db.execute('SELECT fingerprint, name FROM name_overrides WHERE video_id=?',
                              (video_id,)).fetchall()
        return {row['fingerprint']: row['name'] for row in rows}

    def reset_stuck_videos(self) -> int:
        """前回の異常終了で「解析中」のまま残った動画を失敗扱いにする。"""
        with self.connect() as db:
            cursor = db.execute(
                "UPDATE videos SET status='failed' WHERE status='analyzing'")
            return cursor.rowcount

    def set_video_status(self, identifier: str, status: str, match_count: int | None = None,
                         manifest: str | None = None) -> None:
        if status not in VIDEO_STATUSES:
            raise ValueError('動画の状態が不正です')
        with self.connect() as db:
            if match_count is not None:
                db.execute('UPDATE videos SET match_count=? WHERE id=?',
                           (int(match_count), identifier))
            if manifest is not None:
                db.execute('UPDATE videos SET manifest=? WHERE id=?', (manifest, identifier))
            db.execute('UPDATE videos SET status=? WHERE id=?', (status, identifier))

    def video(self, identifier: str) -> dict | None:
        with self.connect() as db:
            row = db.execute('SELECT * FROM videos WHERE id=?', (identifier,)).fetchone()
        if not row:
            return None
        return {'id': row['id'], 'title': row['title'], 'status': row['status'],
                'source': row['source'], 'createdAt': row['created_at'],
                'matchCount': row['match_count'], 'manifest': row['manifest']}

    # ---- テト譜 -----------------------------------------------------------
    def tetofu(self) -> list[dict]:
        with self.connect() as db:
            rows = db.execute('SELECT * FROM tetofu ORDER BY created_at DESC').fetchall()
            tags = self._tags_by_target(db)
        return [{'id': r['id'], 'kind': r['kind'], 'title': r['title'], 'url': r['url'],
                 'createdAt': r['created_at'], 'tags': tags.get(r['id'], [])} for r in rows]

    def tetofu_item(self, identifier: str) -> dict | None:
        with self.connect() as db:
            row = db.execute('SELECT * FROM tetofu WHERE id=?', (identifier,)).fetchone()
        if not row:
            return None
        return {'id': row['id'], 'kind': row['kind'], 'title': row['title'], 'url': row['url']}
    def add_tetofu(self, kind: str, title: str, url: str) -> dict:
        if kind not in ('simulator', 'editor', 'official'):
            raise ValueError('テト譜の種類が不正です')
        identifier = new_id('tetofu')
        with self.connect() as db:
            db.execute('INSERT INTO tetofu(id,kind,title,url,created_at) VALUES(?,?,?,?,?)',
                       (identifier, kind, title, url, now()))
        return {'id': identifier, 'kind': kind, 'title': title, 'url': url,
                'createdAt': now(), 'tags': []}

    # ---- タグ -------------------------------------------------------------
    def tags(self) -> list[dict]:
        with self.connect() as db:
            rows = db.execute(
                'SELECT t.name AS name, t.created_at AS created_at, COUNT(l.target) AS count '
                'FROM tags t LEFT JOIN tag_links l ON l.tag = t.name '
                'GROUP BY t.name ORDER BY t.name').fetchall()
        return [{'name': r['name'], 'count': r['count']} for r in rows]

    def add_tag(self, name: str) -> dict:
        name = (name or '').strip()
        if not name:
            raise ValueError('タグ名を入力してください')
        if len(name) > 60:
            raise ValueError('タグ名は60文字までです')
        with self.connect() as db:
            db.execute('INSERT OR IGNORE INTO tags(name,created_at) VALUES(?,?)', (name, now()))
        return {'name': name, 'count': 0}

    def delete_tag(self, name: str) -> None:
        with self.connect() as db:
            db.execute('DELETE FROM tag_links WHERE tag=?', (name,))
            db.execute('DELETE FROM tags WHERE name=?', (name,))

    def tags_for(self, target: str) -> list[str]:
        with self.connect() as db:
            rows = db.execute('SELECT tag FROM tag_links WHERE target=? ORDER BY tag', (target,)).fetchall()
        return [row['tag'] for row in rows]
    def materialize_tags(self, target: str, names: list[str]) -> list[str]:
        """自動で付くタグを保存する（ロックはしない＝あとで消せる）。"""
        clean = []
        for name in names:
            name = (name or '').strip()
            if name and name not in clean:
                clean.append(name)
        with self.connect() as db:
            for name in clean:
                db.execute('INSERT OR IGNORE INTO tags(name,created_at) VALUES(?,?)', (name, now()))
                db.execute('INSERT OR IGNORE INTO tag_links(tag,target,created_at) VALUES(?,?,?)',
                           (name, target, now()))
        return clean

    def set_tags(self, target: str, names: list[str]) -> list[str]:
        clean = []
        for name in names:
            name = (name or '').strip()
            if name and name not in clean:
                clean.append(name)
        with self.connect() as db:
            db.execute('DELETE FROM tag_links WHERE target=?', (target,))
            db.execute('UPDATE videos SET tags_locked=1 WHERE id=?', (target,))
            for name in clean:
                db.execute('INSERT OR IGNORE INTO tags(name,created_at) VALUES(?,?)', (name, now()))
                db.execute('INSERT OR IGNORE INTO tag_links(tag,target,created_at) VALUES(?,?,?)',
                           (name, target, now()))
        return clean

    @staticmethod
    def _tags_by_target(db) -> dict[str, list[str]]:
        result: dict[str, list[str]] = {}
        for row in db.execute('SELECT tag, target FROM tag_links ORDER BY tag'):
            result.setdefault(row['target'], []).append(row['tag'])
        return result

    # ---- 名前の記憶 -------------------------------------------------------
    def remember_name(self, fingerprint: str, name: str) -> None:
        with self.connect() as db:
            db.execute('INSERT INTO name_memory(fingerprint,name,updated_at,hits) VALUES(?,?,?,1) '
                       'ON CONFLICT(fingerprint) DO UPDATE SET name=excluded.name, '
                       'updated_at=excluded.updated_at, hits=hits+1',
                       (fingerprint, name, now()))

    def recall_name(self, fingerprint: str) -> str | None:
        with self.connect() as db:
            row = db.execute('SELECT name FROM name_memory WHERE fingerprint=?',
                             (fingerprint,)).fetchone()
        return row['name'] if row else None

    # ---- ジョブ -----------------------------------------------------------
    def add_job(self, kind: str, status: str, data: dict) -> str:
        identifier = new_id('job')
        with self.connect() as db:
            db.execute('INSERT INTO jobs(id,kind,status,created_at,data) VALUES(?,?,?,?,?)',
                       (identifier, kind, status, now(), json.dumps(data, ensure_ascii=False)))
        return identifier

    def set_job_status(self, identifier: str, status: str, message: str = '') -> None:
        with self.connect() as db:
            db.execute('UPDATE jobs SET status=? WHERE id=?', (status, identifier))
            row = db.execute('SELECT data FROM jobs WHERE id=?', (identifier,)).fetchone()
            if row:
                try:
                    data = json.loads(row['data'])
                except json.JSONDecodeError:
                    data = {}
                data['message'] = message
                db.execute('UPDATE jobs SET data=? WHERE id=?',
                           (json.dumps(data, ensure_ascii=False), identifier))
    def jobs(self) -> list[dict]:
        with self.connect() as db:
            rows = db.execute('SELECT * FROM jobs ORDER BY created_at DESC LIMIT 100').fetchall()
        return [{'id': r['id'], 'kind': r['kind'], 'status': r['status'],
                 'createdAt': r['created_at'], 'data': json.loads(r['data'])} for r in rows]


def fingerprint_of(path: Path) -> str:
    """名前画像の指紋（段階3で使う。今はファイル内容のSHA256）。"""
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b''):
            digest.update(chunk)
    return digest.hexdigest()[:32]
