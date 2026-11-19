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
CREATE TABLE IF NOT EXISTS opening_names (
    key        TEXT PRIMARY KEY,
    name       TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS opening_colors (
    name       TEXT PRIMARY KEY,
    color      TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS jobs (
    id         TEXT PRIMARY KEY,
    kind       TEXT NOT NULL,
    status     TEXT NOT NULL,
    created_at TEXT NOT NULL,
    data       TEXT NOT NULL DEFAULT '{}'
);
"""

TAG_KINDS = ('free', 'opening', 'midgame')

VIDEO_STATUSES = ('queued', 'analyzing', 'needs-names', 'ready', 'failed')


def now() -> str:
    return dt.datetime.now().isoformat(timespec='seconds')


def _name_key(value: str) -> str:
    """名前の照合用キー（全角半角・大文字小文字・空白の差を吸収する）。"""
    import unicodedata
    text = unicodedata.normalize('NFKC', str(value or ''))
    return ''.join(text.split()).casefold()


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
            # YouTube に上がった日（日本時間の YYYY-MM-DD）。取れなければ空
            if 'upload_date' not in columns:
                db.execute("ALTER TABLE videos ADD COLUMN upload_date TEXT NOT NULL DEFAULT ''")
            # YouTube のチャンネル名（配信者タグ・赤）。取れなければ空
            if 'channel' not in columns:
                db.execute("ALTER TABLE videos ADD COLUMN channel TEXT NOT NULL DEFAULT ''")
            # タグの種類（opening=開幕テンプレ / midgame=途中テンプレ / free=自分のタグ）。
            # プレイヤー名のタグは動画の登場プレイヤーから決まるので、ここには持たない。
            tag_columns = {row['name'] for row in db.execute('PRAGMA table_info(tags)')}
            if 'kind' not in tag_columns:
                db.execute("ALTER TABLE tags ADD COLUMN kind TEXT NOT NULL DEFAULT 'free'")
            # 手で作った・手で付けたタグ（manual=1）は、どこにも付いていなくても消さない。
            # 自動で付くプレイヤー名のタグ（manual=0）だけを整理の対象にする。
            if 'manual' not in tag_columns:
                db.execute("ALTER TABLE tags ADD COLUMN manual INTEGER NOT NULL DEFAULT 0")
                db.execute("UPDATE tags SET manual=1 WHERE kind<>'free' OR name IN "
                           "(SELECT tag FROM tag_links WHERE target NOT LIKE 'video-%')")
            # 開幕テンプレの形（7個の置き場所）も名前と一緒に持つ（書き出し・読み込み・名前ごとの一覧に使う）
            opening_columns = {row['name'] for row in db.execute('PRAGMA table_info(opening_names)')}
            if 'shape' not in opening_columns:
                db.execute("ALTER TABLE opening_names ADD COLUMN shape TEXT NOT NULL DEFAULT ''")
            self._migrate_360(db)

    def _migrate_360(self, db) -> None:
        """360p 既定への一度きりの切り替え。

        旧既定は `standard`（1080p・旧モデル）だった。明示的に別のプロファイルを
        選んでいる環境は触らず、既定のままの人だけ解析モデル（Tetrace-V1.0-PPT）へ
        移す。取得解像度は保存するかどうかで jobs.py が切り替える。
        """
        marker = db.execute(
            "SELECT value FROM settings WHERE key='migration.model360'").fetchone()
        if marker is not None:
            return
        row = db.execute(
            "SELECT value FROM settings WHERE key='analysisProfile'").fetchone()
        try:
            profile = json.loads(row['value']) if row is not None else None
        except (TypeError, json.JSONDecodeError):
            profile = None
        if profile in (None, 'standard', 'fast320'):
            db.execute(
                "INSERT INTO settings(key, value, updated_at) VALUES('analysisProfile', ?, ?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at",
                (json.dumps('tetrace-v1.0-ppt'), now()))
        db.execute(
            "INSERT INTO settings(key, value, updated_at) VALUES('migration.model360', ?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at",
            (json.dumps(1), now()))
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
                 'uploadDate': r['upload_date'] or '',
                 'channel': r['channel'] or '',
                 'matchCount': r['match_count'], 'manifest': r['manifest'],
                 'tags_locked': r['tags_locked']} for r in rows]

    def set_channel(self, identifier: str, channel: str) -> None:
        """チャンネル名（配信者タグ）を覚える。配信者タグは保存したチャンネル名から画面側で作る。
        以前に手で付けた同じ名前の通常タグは、配信者タグと重なるのでこの動画から外す。"""
        channel = (channel or '').strip()[:60]
        if not channel:
            return
        with self.connect() as db:
            db.execute('UPDATE videos SET channel=? WHERE id=?', (channel, identifier))
            db.execute('DELETE FROM tag_links WHERE target=? AND tag=?', (identifier, channel))
            db.execute("DELETE FROM tags WHERE name=? AND kind='free' "
                       "AND name NOT IN (SELECT DISTINCT tag FROM tag_links)", (channel,))

    def split_player_tags(self, players_by_video: dict[str, list[str]]) -> int:
        """以前はプレイヤー名も tag_links に入れていた。プレイヤー名は登場プレイヤーから毎回決めるので、
        tag_links には自分で付けたタグだけを残す（1回だけ実行。プレイヤー名と同じ名前の黄色タグを付けられるように）。"""
        removed = 0
        with self.connect() as db:
            for target, names in players_by_video.items():
                for name in names:
                    removed += db.execute('DELETE FROM tag_links WHERE target=? AND tag=?',
                                          (target, name)).rowcount or 0
            db.execute("DELETE FROM tag_links WHERE tag='動画解析'")
            db.execute("DELETE FROM tags WHERE manual=0 AND kind='free' "
                       "AND name NOT IN (SELECT DISTINCT tag FROM tag_links)")
        return removed

    def set_upload_date(self, identifier: str, date: str) -> None:
        with self.connect() as db:
            db.execute('UPDATE videos SET upload_date=? WHERE id=?', (date, identifier))

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
        result = {}
        for row in rows:
            try:
                result[row['key']] = json.loads(row['value'])
            except (TypeError, json.JSONDecodeError):
                result[row['key']] = row['value']
        return result

    def set_setting(self, key: str, value) -> None:
        with self.connect() as db:
            db.execute('INSERT INTO settings(key, value, updated_at) VALUES(?,?,?) '
                       'ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at',
                       (key, json.dumps(value, ensure_ascii=False), now()))

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

    def name_signature(self) -> str:
        """名前の記憶・上書きの「版」。変わったら保存済みのプレイヤー名一覧を作り直す。"""
        with self.connect() as db:
            memory = db.execute('SELECT COUNT(*), COALESCE(MAX(updated_at),""), COALESCE(SUM(LENGTH(name)),0) '
                                'FROM name_memory').fetchone()
            overrides = db.execute('SELECT COUNT(*), COALESCE(MAX(updated_at),"") FROM name_overrides').fetchone()
        return '|'.join(str(value) for value in (*memory, *overrides))

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
        """前回の終了で「解析中」「待機中」のまま残った動画とジョブを失敗扱いにする。

        ジョブはメモリにしか無いので、再起動すると続きは走らない。残しておくと
        「実行中」「待機中」のまま止まって見えるので、理由を付けて失敗にする。
        """
        reason = 'PPT Lab を再起動したため解析が中断されました（再解析できます）'
        with self.connect() as db:
            cursor = db.execute(
                "UPDATE videos SET status='failed' WHERE status IN ('analyzing','queued')")
            for row in db.execute("SELECT id, data FROM jobs WHERE status IN ('queued','running')").fetchall():
                try:
                    data = json.loads(row['data'] or '{}')
                except (TypeError, ValueError):
                    data = {}
                data.update({'message': reason, 'error': reason})
                db.execute("UPDATE jobs SET status='failed', data=? WHERE id=?",
                           (json.dumps(data, ensure_ascii=False), row['id']))
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
                'uploadDate': row['upload_date'] or '',
                'matchCount': row['match_count'], 'manifest': row['manifest']}

    # ---- テト譜 -----------------------------------------------------------
    def tetofu(self) -> list[dict]:
        with self.connect() as db:
            rows = db.execute('SELECT * FROM tetofu ORDER BY created_at DESC').fetchall()
            tags = self._tags_by_target(db)
        items = []
        for r in rows:
            try:
                extra = json.loads(r['data'] or '{}')
            except (TypeError, ValueError):
                extra = {}
            item = {'id': r['id'], 'kind': r['kind'], 'title': r['title'], 'url': r['url'],
                    'createdAt': r['created_at'], 'tags': tags.get(r['id'], [])}
            # 動画の試合から保存したもの（試合全体）は、どの試合かを持つ
            if isinstance(extra, dict) and extra.get('videoId'):
                item['videoId'] = str(extra['videoId'])
                item['matchIndex'] = int(extra.get('matchIndex') or 0)
                item['scope'] = str(extra.get('scope') or '')
                if extra.get('seconds') is not None:
                    item['seconds'] = float(extra['seconds'])
            items.append(item)
        return items

    def tetofu_item(self, identifier: str) -> dict | None:
        with self.connect() as db:
            row = db.execute('SELECT * FROM tetofu WHERE id=?', (identifier,)).fetchone()
        if not row:
            return None
        return {'id': row['id'], 'kind': row['kind'], 'title': row['title'], 'url': row['url']}
    def add_tetofu(self, kind: str, title: str, url: str, extra: dict | None = None) -> dict:
        if kind not in ('simulator', 'editor', 'official'):
            raise ValueError('テト譜の種類が不正です')
        identifier = new_id('tetofu')
        with self.connect() as db:
            db.execute('INSERT INTO tetofu(id,kind,title,url,created_at,data) VALUES(?,?,?,?,?,?)',
                       (identifier, kind, title, url, now(),
                        json.dumps(extra or {}, ensure_ascii=False)))
        item = {'id': identifier, 'kind': kind, 'title': title, 'url': url,
                'createdAt': now(), 'tags': []}
        if extra and extra.get('videoId'):
            item.update({'videoId': str(extra['videoId']),
                         'matchIndex': int(extra.get('matchIndex') or 0),
                         'scope': str(extra.get('scope') or '')})
            if extra.get('seconds') is not None:
                item['seconds'] = float(extra['seconds'])
        return item

    def set_tetofu_title(self, identifier: str, title: str) -> bool:
        title = (title or '').strip()
        if not title:
            raise ValueError('タイトルを入力してください')
        if len(title) > 200:
            raise ValueError('タイトルは200文字までです')
        with self.connect() as db:
            cursor = db.execute('UPDATE tetofu SET title=? WHERE id=?', (title, identifier))
        return cursor.rowcount > 0

    # ---- タグ -------------------------------------------------------------
    def tags(self) -> list[dict]:
        with self.connect() as db:
            rows = db.execute(
                'SELECT t.name AS name, t.created_at AS created_at, t.kind AS kind, COUNT(l.target) AS count '
                'FROM tags t LEFT JOIN tag_links l ON l.tag = t.name '
                'GROUP BY t.name ORDER BY t.name').fetchall()
        return [{'name': r['name'], 'count': r['count'], 'kind': r['kind'] or 'free'} for r in rows]

    def add_tag(self, name: str, kind: str = 'free') -> dict:
        name = (name or '').strip()
        if not name:
            raise ValueError('タグ名を入力してください')
        if len(name) > 60:
            raise ValueError('タグ名は60文字までです')
        kind = kind if kind in TAG_KINDS else 'free'
        with self.connect() as db:
            db.execute('INSERT OR IGNORE INTO tags(name,created_at,kind,manual) VALUES(?,?,?,1)', (name, now(), kind))
            db.execute('UPDATE tags SET kind=?, manual=1 WHERE name=?', (kind, name))
        return {'name': name, 'count': 0, 'kind': kind}

    def set_tag_kind(self, name: str, kind: str) -> None:
        if kind not in TAG_KINDS:
            raise ValueError('タグの種類が正しくありません')
        with self.connect() as db:
            if not db.execute('UPDATE tags SET kind=? WHERE name=?', (kind, name)).rowcount:
                raise ValueError('タグが見つかりません')

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

    def reconcile_tags(self, target: str, auto: list[str], stale_names) -> list[str]:
        """自動タグを付け直す。OCR名の候補から消えたタグは外す。"""
        clean = []
        for name in auto:
            name = (name or '').strip()
            if name and name not in clean:
                clean.append(name)
        wanted = set(clean)
        stale = set()
        for name in stale_names or []:
            name = str(name or '').strip()
            if name and name not in wanted:
                stale.add(name)
        with self.connect() as db:
            for name in stale:
                db.execute('DELETE FROM tag_links WHERE target=? AND tag=?', (target, name))
            for name in clean:
                db.execute('INSERT OR IGNORE INTO tags(name,created_at) VALUES(?,?)', (name, now()))
                db.execute('INSERT OR IGNORE INTO tag_links(tag,target,created_at) VALUES(?,?,?)',
                           (name, target, now()))
        return clean

    def rename_name(self, old: str, new: str) -> int:
        """名前の記憶を付け替える（統合にも使う）。"""
        with self.connect() as db:
            # updated_at も進める（名前の版 name_signature が文字数の同じ改名でも変わるように）
            changed = db.execute('UPDATE name_memory SET name=?, updated_at=? WHERE name=?',
                                 (new, now(), old)).rowcount
            db.execute('UPDATE name_overrides SET name=?, updated_at=? WHERE name=?', (new, now(), old))
            db.execute('INSERT OR IGNORE INTO tags(name,created_at) VALUES(?,?)', (new, now()))
            db.execute('UPDATE OR IGNORE tag_links SET tag=? WHERE tag=?', (new, old))
            db.execute('DELETE FROM tag_links WHERE tag=? AND EXISTS '
                       '(SELECT 1 FROM tag_links t2 WHERE t2.tag=? AND t2.target=tag_links.target)', (old, new))
            db.execute('DELETE FROM tags WHERE name=?', (old,))
        return int(changed or 0)

    def remove_tag_link(self, target: str, name: str) -> None:
        with self.connect() as db:
            db.execute('DELETE FROM tag_links WHERE target=? AND tag=?', (target, name))

    def prune_tags(self) -> int:
        """どこにも付いていない自動タグ（プレイヤー名）を消す。手で作ったタグとテンプレは残す。"""
        with self.connect() as db:
            removed = db.execute("DELETE FROM tags WHERE manual=0 AND kind='free' "
                                 "AND name NOT IN (SELECT DISTINCT tag FROM tag_links)").rowcount
        return int(removed or 0)

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
                db.execute('INSERT OR IGNORE INTO tags(name,created_at,manual) VALUES(?,?,1)', (name, now()))
                db.execute('UPDATE tags SET manual=1 WHERE name=?', (name,))
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

    def recall_name_by_text(self, text: str) -> str | None:
        """OCR が読んだ文字列から、過去に確定した名前を引く。

        指紋は画像の完全一致ハッシュなので、同じ人でも配信ごとに画素が変わると
        当たらない。文字のほうが安定するため、こちらを第二の手がかりに使う。
        """
        key = _name_key(text)
        if len(key) < 2:
            return None
        with self.connect() as db:
            for row in db.execute('SELECT DISTINCT name FROM name_memory'):
                if _name_key(row['name']) == key:
                    return row['name']
        return None

    def names_for_fingerprints(self, fingerprints) -> dict[str, str]:
        """指紋の一覧から {fingerprint: name} を作る（画像照合用）。"""
        values = [str(item) for item in fingerprints if item]
        result: dict[str, str] = {}
        if not values:
            return result
        with self.connect() as db:
            for start in range(0, len(values), 400):
                part = values[start:start + 400]
                marks = ','.join('?' * len(part))
                for row in db.execute(
                        f'SELECT fingerprint, name FROM name_memory '
                        f'WHERE fingerprint IN ({marks})', part):
                    result[row['fingerprint']] = row['name']
        return result

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
    def opening_names(self) -> dict[str, str]:
        """開幕の形（キー）→ 利用者が付けた名前。"""
        with self.connect() as db:
            return {row['key']: row['name'] for row in db.execute('SELECT key, name FROM opening_names')}

    def opening_templates(self) -> list[dict]:
        """名前を付けた開幕（キー・名前・形）。"""
        with self.connect() as db:
            rows = db.execute('SELECT key, name, shape FROM opening_names ORDER BY name, key').fetchall()
        items = []
        for row in rows:
            try:
                shape = json.loads(row['shape']) if row['shape'] else {}
            except (TypeError, ValueError):
                shape = {}
            items.append({'key': row['key'], 'name': row['name'], 'pieces': shape})
        return items

    def set_opening_name(self, key: str, name: str, shape: dict | None = None) -> None:
        key = (key or '').strip()
        name = (name or '').strip()
        if not key:
            raise ValueError('開幕の指定がありません')
        if len(name) > 60:
            raise ValueError('名前は60文字までです')
        with self.connect() as db:
            if name:
                db.execute('INSERT INTO opening_names(key,name,updated_at,shape) VALUES(?,?,?,?) '
                           'ON CONFLICT(key) DO UPDATE SET name=excluded.name, updated_at=excluded.updated_at, '
                           "shape=CASE WHEN excluded.shape<>'' THEN excluded.shape ELSE opening_names.shape END",
                           (key, name, now(), json.dumps(shape or {}, ensure_ascii=False) if shape else ''))
            else:
                db.execute('DELETE FROM opening_names WHERE key=?', (key,))

    def clear_opening_names(self) -> None:
        with self.connect() as db:
            db.execute('DELETE FROM opening_names')

    def opening_colors(self) -> dict[str, str]:
        with self.connect() as db:
            return {row['name']: row['color'] for row in db.execute('SELECT name, color FROM opening_colors')}

    def set_opening_color(self, name: str, color: str) -> None:
        name = (name or '').strip()
        color = (color or '').strip()
        if not name:
            raise ValueError('名前がありません')
        with self.connect() as db:
            if color:
                db.execute('INSERT INTO opening_colors(name,color,updated_at) VALUES(?,?,?) '
                           'ON CONFLICT(name) DO UPDATE SET color=excluded.color, updated_at=excluded.updated_at',
                           (name, color, now()))
            else:
                db.execute('DELETE FROM opening_colors WHERE name=?', (name,))

    def last_job_message(self, video_id: str) -> str:
        """その動画の最後のジョブの結果文（失敗理由など）。再起動後も残る。"""
        with self.connect() as db:
            rows = db.execute("SELECT data FROM jobs ORDER BY created_at DESC").fetchall()
        for row in rows:
            try:
                data = json.loads(row['data'] or '{}')
            except (TypeError, ValueError):
                continue
            if data.get('videoId') == video_id:
                return str(data.get('error') or data.get('message') or '')
        return ''

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
