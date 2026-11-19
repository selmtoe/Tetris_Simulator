"""Personal Lab: loopback-only SQLite administration and native analysis.

Public pages remain ordinary GitHub Pages files. Neither this server nor its
database belongs in the consumer package or the public Lab repository.
"""
from __future__ import annotations
import argparse, contextlib, datetime as dt, hashlib, json, mimetypes, os, re
import secrets, shutil, sqlite3, subprocess, threading, time, uuid, webbrowser, base64, math, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit, parse_qs
sys.path.insert(0,str(Path(__file__).resolve().parent))
from lab_workflows import WorkflowStoreMixin
from completed_results import CompletedResultsMixin
from score_markers import ScoreMarkersMixin
from editorial import EditorialStoreMixin
from research_collections import ResearchCollectionsMixin
from trash_operations import TrashStoreMixin
from boundary_correction import BoundaryCorrectionMixin
from preview_appearance import SCRIPT_PATHS as PREVIEW_SCRIPT_PATHS, html_variant, script_variant

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / 'tools/video-recovery-runtime'
DEFAULT_LAB = ROOT.parent / 'Lab'
DEFAULT_DATA = RUNTIME / 'data/lab-private'
KINDS = {'article', 'video', 'tetofu', 'match'}
PUBLIC_FIELDS = {'id', 'kind', 'title', 'description', 'content', 'tags', 'players', 'date',
                 'url', 'youtubeId', 'startSeconds', 'endSeconds', 'status', 'simulator', 'snapshot',
                 'videoKey', 'videoTitle', 'youtubeOffsetSeconds', 'sourceRef', 'research', 'citations', 'reviewState', 'publicPlayback', 'thumbnail',
                 'winner', 'winnerStatus', 'winnerConfidence'}
EDIT_FIELDS = PUBLIC_FIELDS | {'visibility', 'bookmarks', 'parentId', 'editorialFormat'}
EDIT_FIELDS -= {'reviewState'}
REFERENCE_FIELDS = {'recordId','title','seconds','phase','player','youtubeId','youtubeOffsetSeconds','analysisRevision','positionIndex','originId'}
VIDEO_EXTENSIONS = {'.mp4', '.mkv', '.mov', '.webm', '.m4v', '.avi'}

def stamp(): return dt.datetime.now(dt.timezone.utc).isoformat(timespec='seconds')
def encode(value): return json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
def read_json(path): return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def within(path, root):
    try: Path(path).resolve().relative_to(Path(root).resolve()); return True
    except ValueError: return False

class LabError(Exception):
    def __init__(self, message, status=400): self.status = status; super().__init__(message)

def validated_tags(tags):
    if not isinstance(tags,list) or any(not isinstance(tag,str) for tag in tags):
        raise LabError('タグをカンマで区切って入力してください。')
    for tag in tags:
        if len(tag)>100:
            preview=tag[:20]+'…'
            raise LabError(f'タグ「{preview}」は{len(tag)}文字あります。1つのタグは100文字以内にしてください。')
    unique=list(dict.fromkeys(tags))
    if len(unique)>100:
        raise LabError(f'タグが{len(unique)}個あります。1つの資料に付けられるタグは100個までです。')
    return unique

def validate_reference(value):
    if not isinstance(value,dict) or set(value)-REFERENCE_FIELDS:
        raise LabError('引用元の情報が正しくありません。')
    for key in ('recordId','title','youtubeId','player','originId'):
        if key in value and (not isinstance(value[key],str) or len(value[key])>250): raise LabError('引用元の情報が正しくありません。')
    if value.get('youtubeId') and not re.fullmatch(r'[\w-]{11}',value['youtubeId']): raise LabError('引用元のYouTube URLを確認してください。')
    for key in ('seconds','phase','youtubeOffsetSeconds','analysisRevision','positionIndex'):
        if key in value and (not isinstance(value[key],(int,float)) or not math.isfinite(value[key]) or abs(value[key])>1e8): raise LabError('引用元の時刻を確認してください。')

def validate_snapshot(data):
    if not isinstance(data,dict) or data.get('v') not in (2,3,4) or len(encode(data))>6*1024**2:
        raise LabError('局面データが大きすぎるか、対応していない形式です。')

class Store(TrashStoreMixin, ResearchCollectionsMixin, EditorialStoreMixin, ScoreMarkersMixin, CompletedResultsMixin, BoundaryCorrectionMixin, WorkflowStoreMixin):
    error = LabError
    def __init__(self, lab, data, root=ROOT):
        self.lab, self.data, self.root = Path(lab).resolve(), Path(data).resolve(), Path(root).resolve()
        self.data.mkdir(parents=True, exist_ok=True)
        self.db = self.data / 'lab.sqlite3'
        self.lock = threading.RLock()
        self.catalog = {}; self.catalog_time = 0
        self.jobs = {}; self.processes = {}; self.job_lock = threading.Lock()
        self.pending_analysis = []
        with self.connect() as db:
            db.executescript('''
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS records(id TEXT PRIMARY KEY, body TEXT NOT NULL, revision INTEGER NOT NULL, updated TEXT NOT NULL, deleted INTEGER NOT NULL DEFAULT 0);
                CREATE TABLE IF NOT EXISTS revisions(record_id TEXT NOT NULL, revision INTEGER NOT NULL, body TEXT NOT NULL, updated TEXT NOT NULL, PRIMARY KEY(record_id,revision));
                CREATE TABLE IF NOT EXISTS media(id TEXT PRIMARY KEY, path TEXT NOT NULL, name TEXT NOT NULL, bytes INTEGER NOT NULL, ready INTEGER NOT NULL DEFAULT 1);
                CREATE TABLE IF NOT EXISTS imports(origin TEXT PRIMARY KEY, record_id TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS jobs(id TEXT PRIMARY KEY, body TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY, body TEXT NOT NULL);
            ''')
            if not db.execute('SELECT 1 FROM records LIMIT 1').fetchone() and not db.execute("SELECT 1 FROM metadata WHERE key='migration:editorial-v1'").fetchone():
                for record in read_json(self.lab / 'data/library.json')['records']:
                    record.setdefault('visibility', 'public'); record.setdefault('bookmarks', [])
                    self._save(db, record, 0)
            for row in db.execute('SELECT body FROM jobs'):
                job = json.loads(row[0])
                if job['status'] in ('running', 'queued'): job['status'] = 'interrupted'
                self.jobs[job['id']] = job
        self.sync_media_links()
        self.restore_scoring_jobs()
        self.migrate_ai_candidates()
        self.initialize_editorial()
        self.ensure_analysis_articles()

    @contextlib.contextmanager
    def connect(self):
        db = sqlite3.connect(self.db, timeout=20)
        db.row_factory = sqlite3.Row
        try:
            with db: yield db
        finally: db.close()

    def _save(self, db, record, revision, deleted=0):
        record = dict(record); record['revision'] = revision + 1; record['updatedAt'] = stamp()
        db.execute('INSERT INTO records VALUES(?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET body=excluded.body,revision=excluded.revision,updated=excluded.updated,deleted=excluded.deleted',
                   (record['id'], encode(record).decode(), record['revision'], record['updatedAt'], deleted))
        db.execute('INSERT INTO revisions VALUES(?,?,?,?)', (record['id'], record['revision'], encode(record).decode(), record['updatedAt']))
        return record

    def records(self, deleted=False):
        with self.connect() as db:
            records = [json.loads(row[0]) for row in db.execute('SELECT body FROM records WHERE deleted=? ORDER BY updated DESC,id', (int(deleted),))]
        for record in records:
            record.pop('mediaUrl',None)
            if record.get('kind')=='match':
                record.setdefault('videoKey',record.get('mediaId') or hashlib.sha256(record.get('analysis',{}).get('manifest',record['id']).encode()).hexdigest()[:32])
                record.setdefault('videoTitle',record['title'].rsplit(' · 試合',1)[0])
        return records

    def get(self, identifier):
        with self.connect() as db: row = db.execute('SELECT body FROM records WHERE id=? AND deleted=0', (identifier,)).fetchone()
        if not row: raise LabError('資料が見つかりません。', 404)
        return json.loads(row[0])

    def save(self, patch):
        with self.lock, self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            return self._save_record(db, patch)

    def article_save(self, request):
        if not isinstance(request,dict): raise LabError('記事の保存内容を確認してください。')
        operation=request.get('operationId'); patch=request.get('record')
        if not isinstance(operation,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,120}',operation):
            raise LabError('記事の保存操作IDを確認してください。')
        if not isinstance(patch,dict): raise LabError('記事の保存内容を確認してください。')
        fingerprint=hashlib.sha256(json.dumps(patch,ensure_ascii=False,sort_keys=True,separators=(',',':')).encode('utf-8')).hexdigest()
        key='article-save:'+operation
        with self.lock, self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            row=db.execute('SELECT body FROM metadata WHERE key=?',(key,)).fetchone()
            if row:
                receipt=json.loads(row['body'])
                if receipt['payloadHash']!=fingerprint:
                    raise LabError('同じ保存操作IDで内容が変更されています。保存内容を確認してください。',409)
                if 'record' in receipt:return receipt['record']
                revision=db.execute('SELECT body FROM revisions WHERE record_id=? AND revision=?',(receipt['recordId'],receipt['revision'])).fetchone()
                if not revision:raise LabError('保存結果の履歴が見つかりません。',500)
                return json.loads(revision['body'])
            if request.get('draftToken') is not None:
                self._editorial_assert_draft(db,patch.get('id'),request['draftToken'])
            saved=self._save_record(db,patch,article_only=True)
            if request.get('draftToken') is not None:self._editorial_capture_saved_draft(db,saved)
            db.execute('INSERT INTO metadata VALUES(?,?)',(key,encode({'payloadHash':fingerprint,'recordId':saved['id'],'revision':saved['revision']}).decode('utf-8')))
            return saved

    def video_settings(self, request):
        if not isinstance(request,dict): raise LabError('動画の設定内容を確認してください。')
        operation=request.get('operationId'); video=request.get('video'); targets=request.get('records'); patch=request.get('patch')
        if not isinstance(operation,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,120}',operation):
            raise LabError('動画の保存操作IDを確認してください。')
        if not isinstance(video,dict) or not isinstance(targets,list) or not isinstance(patch,dict):
            raise LabError('動画と関連試合の設定内容を確認してください。')
        payload={'video':video,'records':targets,'patch':patch}
        fingerprint=hashlib.sha256(json.dumps(payload,ensure_ascii=False,sort_keys=True,separators=(',',':')).encode('utf-8')).hexdigest()
        key='video-settings:'+operation
        with self.lock, self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            row=db.execute('SELECT body FROM metadata WHERE key=?',(key,)).fetchone()
            if row:
                receipt=json.loads(row['body'])
                if receipt['payloadHash']!=fingerprint:
                    raise LabError('同じ保存操作IDで内容が変更されています。保存内容を確認してください。',409)
                revision=db.execute('SELECT body FROM revisions WHERE record_id=? AND revision=?',(receipt['recordId'],receipt['revision'])).fetchone()
                if not revision: raise LabError('保存結果の履歴が見つかりません。',500)
                return {'video':json.loads(revision['body']),'updated':receipt['updated']}
            if len(targets)>500: raise LabError('対象は0〜500試合で選んでください。')
            seen=set()
            for target in targets:
                if (not isinstance(target,dict) or set(target)!={'id','revision'}
                        or not isinstance(target['id'],str) or not re.fullmatch(r'[\w-]{1,120}',target['id'])
                        or type(target['revision']) is not int or target['revision']<1 or target['id'] in seen):
                    raise LabError('関連試合のIDと版を確認してください。')
                seen.add(target['id'])
            self._validate_match_patch(patch)
            identifier=video.get('id')
            if identifier is not None and (not isinstance(identifier,str) or not re.fullmatch(r'[\w-]{1,120}',identifier)):
                raise LabError('動画の資料IDを確認してください。')
            original=db.execute('SELECT body,deleted FROM records WHERE id=?',(identifier,)).fetchone() if identifier else None
            if original and original['deleted']:
                raise LabError('この動画はごみ箱に移されています。先に資料を戻してください。',409)
            original_kind=json.loads(original['body']).get('kind') if original else None
            if (original and original_kind!='video') or video.get('kind',original_kind)!='video':
                raise LabError('この保存操作は動画の設定にだけ使用できます。')
            # The common settings are identical on the video and its matches;
            # only matches append tags to those already saved.
            saved=self._save_record(db,{**video,**patch})
            self._update_matches(db,targets,patch)
            receipt={'payloadHash':fingerprint,'recordId':saved['id'],'revision':saved['revision'],'updated':len(targets)}
            db.execute('INSERT INTO metadata VALUES(?,?)',(key,encode(receipt).decode('utf-8')))
            return {'video':saved,'updated':len(targets)}

    def _save_record(self, db, patch, article_only=False):
        if not isinstance(patch, dict): raise LabError('資料の形式が正しくありません。')
        identifier = patch.get('id') or uuid.uuid4().hex
        if not isinstance(identifier, str) or not re.fullmatch(r'[\w-]{1,120}', identifier): raise LabError('資料IDが正しくありません。')
        row = db.execute('SELECT body,revision FROM records WHERE id=? AND deleted=0', (identifier,)).fetchone()
        value = json.loads(row['body']) if row else {'id': identifier, 'visibility': 'private', 'bookmarks': [], 'date': dt.datetime.now().date().isoformat()}
        if not row and db.execute('SELECT 1 FROM records WHERE id=? AND deleted=1',(identifier,)).fetchone():
            raise LabError('この資料はごみ箱に移されています。先に資料を戻してください。',409)
        if article_only:
            if (row and value.get('kind') not in ('article','video','tetofu')) or patch.get('kind',value.get('kind')) not in ('article','video','tetofu'):
                raise LabError('この保存操作は記事にだけ使用できます。')
            if row and patch.get('kind',value['kind'])!=value['kind']:
                raise LabError('既存記事の公開URLを維持するため、表示形式は変更できません。本文やリンクは同じ記事へ追加できます。')
            if not row and db.execute('SELECT 1 FROM records WHERE id=? AND deleted=1',(identifier,)).fetchone():
                raise LabError('この記事はごみ箱に移されています。先に資料を戻してください。',409)
        if row and patch.get('revision') != row['revision']: raise LabError('別の画面で変更されています。再読み込みしてから編集してください。', 409)
        if row and value.get('kind')=='match':
            for key in ('kind','simulator','snapshot','startSeconds','endSeconds','players'):
                if key in patch and patch[key]!=value.get(key):
                    raise LabError('解析結果はPCの配信解析で修正して、完成ファイルを取り込み直してください。')
            # Quality is supplied only by the completed-file importer.
            if 'status' in patch:patch={**patch,'status':value.get('status','failed')}
        value.update({key: val for key, val in patch.items() if key in EDIT_FIELDS}); value['id'] = identifier
        if 'editorialFormat' in value and value['editorialFormat'] not in ('text','link','video'): raise LabError('記事の形式を確認してください。')
        if article_only and not row and value.get('kind')=='video': value.setdefault('publicPlayback',{'enabled':False})
        if 'publicPlayback' in value: self.validate_public_playback(value['publicPlayback'])
        if 'youtubeId' in patch: value['youtubeLinkEdited'] = True
        if value.get('kind') not in KINDS or not isinstance(value.get('title'), str) or not value['title'].strip(): raise LabError('種類とタイトルを入力してください。')
        for key, limit in [('title',250), ('description',1000), ('content',2_000_000), ('url',4_000_000), ('youtubeId',11)]:
            if key in value and (not isinstance(value[key],str) or len(value[key]) > limit): raise LabError('入力内容が長すぎるか、形式が正しくありません。')
        if value.get('visibility') not in ('private','public'): raise LabError('公開範囲が正しくありません。')
        if value.get('url') and urlsplit(value['url']).scheme not in ('http','https'): raise LabError('リンクはhttpまたはhttpsで入力してください。')
        if 'thumbnail' in value:
            cover=value['thumbnail']
            if not isinstance(cover,str) or len(cover)>400000: raise LabError('サムネイル画像が大きすぎるか、形式が正しくありません。')
            if cover:
                match=re.fullmatch(r'data:image/(png|jpeg|webp);base64,([A-Za-z0-9+/=]+)',cover)
                if match:
                    try:
                        raw=base64.b64decode(match[2],validate=True)
                        valid=(match[1]=='png' and raw.startswith(b'\x89PNG\r\n\x1a\n') or match[1]=='jpeg' and raw.startswith(b'\xff\xd8\xff') or match[1]=='webp' and raw.startswith(b'RIFF') and raw[8:12]==b'WEBP')
                        if not valid: raise ValueError()
                    except ValueError: raise LabError('サムネイルの画像形式を確認してください。')
                elif urlsplit(cover).scheme not in ('http','https') or not urlsplit(cover).netloc: raise LabError('サムネイルの画像形式を確認してください。')
        if value.get('youtubeId') and not re.fullmatch(r'[\w-]{11}',value['youtubeId']): raise LabError('YouTubeのURLを確認してください。')
        if 'youtubeOffsetSeconds' in value and (not isinstance(value['youtubeOffsetSeconds'],(int,float)) or not -1e7 < value['youtubeOffsetSeconds'] < 1e7): raise LabError('動画の時刻補正を確認してください。')
        if value.get('analysis'):
            value['status'] = json.loads(row['body']).get('status','failed')
        value['tags'] = validated_tags(value.get('tags', []))
        if not isinstance(value.get('bookmarks'),list) or len(value['bookmarks']) > 10000: raise LabError('メモの件数が多すぎます。')
        for mark in value['bookmarks']:
            if not isinstance(mark,dict) or not isinstance(mark.get('seconds'),(int,float)) or not 0 <= mark['seconds'] < 1e8 or not isinstance(mark.get('note',''),str): raise LabError('時刻付きメモの形式が正しくありません。')
            if 'label' in mark and (not isinstance(mark['label'],str) or len(mark['label'])>250): raise LabError('時刻の見出しは250文字以内で入力してください。')
            if 'sourceRef' in mark: validate_reference(mark['sourceRef'])
        if value.get('status') == 'failed': value.pop('simulator',None)
        if 'sourceRef' in value: validate_reference(value['sourceRef'])
        if 'snapshot' in value: validate_snapshot(value['snapshot'])
        if 'research' in value:
            research=value['research']
            if not isinstance(research,dict) or set(research)-{'stage','prompt','answer','notes','ai','folderId'} or research.get('stage') not in ('candidate','selected','problem','archived'):
                raise LabError('研究の保存先を確認してください。')
            folder_id=research.get('folderId','')
            if not isinstance(folder_id,str) or len(folder_id)>120 or folder_id and not db.execute('SELECT 1 FROM metadata WHERE key=?',('research:folder:'+folder_id,)).fetchone():
                raise LabError('保存先のフォルダが見つかりません。選び直してください。')
            for key in ('prompt','answer','notes'):
                if key in research and (not isinstance(research[key],str) or len(research[key])>100000): raise LabError('問題文・解説を確認してください。')
            if 'ai' in research and (not isinstance(research['ai'],dict) or len(encode(research['ai']))>500000): raise LabError('採点結果が大きすぎます。')
        if 'citations' in value:
            if not isinstance(value['citations'],list) or len(value['citations'])>100: raise LabError('引用は100局面まで追加できます。')
            for citation in value['citations']:
                if not isinstance(citation,dict) or set(citation)-{'title','snapshot','sourceRef'} or not isinstance(citation.get('title'),str) or len(citation['title'])>250: raise LabError('引用する局面を確認してください。')
                validate_snapshot(citation.get('snapshot')); validate_reference(citation.get('sourceRef',{}))
        return self._save(db, value, row['revision'] if row else 0)

    def bulk_update(self, value):
        targets=value.get('records',[]); patch=value.get('patch',{})
        if not isinstance(targets,list) or not 1<=len(targets)<=500: raise LabError('対象は1〜500試合で選んでください。')
        self._validate_match_patch(patch)
        with self.lock, self.connect() as db:
            self._update_matches(db,targets,patch)
        return {'updated':len(targets)}

    def _validate_match_patch(self, patch):
        if not isinstance(patch,dict) or set(patch)-{'youtubeId','visibility','tags','youtubeOffsetSeconds'}: raise LabError('一括設定の項目が正しくありません。')
        if 'youtubeOffsetSeconds' in patch and (not isinstance(patch['youtubeOffsetSeconds'],(int,float)) or not -1e7 < patch['youtubeOffsetSeconds'] < 1e7): raise LabError('動画の時刻補正を確認してください。')
        if 'youtubeId' in patch and (not isinstance(patch['youtubeId'],str) or (patch['youtubeId'] and not re.fullmatch(r'[\w-]{11}',patch['youtubeId']))): raise LabError('YouTubeのURLを確認してください。')
        if 'visibility' in patch and patch['visibility'] not in ('private','public'): raise LabError('公開範囲を確認してください。')
        if 'tags' in patch: validated_tags(patch['tags'])
    def _update_matches(self, db, targets, patch):
        for target in targets:
            row=db.execute('SELECT body,revision FROM records WHERE id=? AND deleted=0',(target['id'],)).fetchone()
            if not row or row['revision']!=target['revision']: raise LabError('対象の資料が更新されました。再読み込みしてください。',409)
            record=json.loads(row['body'])
            if record['kind']!='match': raise LabError('一括設定は解析した試合に対して使えます。')
            previous=record.get('tags',[]);record.update(patch)
            if 'youtubeId' in patch:record['youtubeLinkEdited']=True
            if 'tags' in patch:record['tags']=validated_tags(previous+patch['tags'])
            self._save(db,record,row['revision'])

    def batches(self, refresh=False):
        with self.lock:
            if not refresh and time.monotonic()-self.catalog_time < 60: return list(self.catalog.values())
            runtime_data = self.root / 'tools/video-recovery-runtime/data'
            paths = []
            for directory in [runtime_data/'batches', runtime_data/'reanalysis', self.data/'analyses']:
                if directory.exists(): paths.extend(directory.rglob('batch-results.json'))
            # The most recently validated complete stream is also available without searching test runs.
            validated = runtime_data/'experiments/compatibility-browser-20260910/runs/current-final/june-full/batch-results.json'
            if validated.exists(): paths.append(validated)
            self.catalog = {}
            for path in sorted(set(paths), key=lambda p:p.stat().st_mtime, reverse=True)[:500]:
                try:
                    data = read_json(path)
                    if not isinstance(data.get('matches'), list) or not data.get('source'): continue
                    identifier = hashlib.sha256(str(path.resolve()).encode()).hexdigest()[:24]
                    self.catalog[identifier] = {'id':identifier,'title':Path(data['source']).name,'matches':len(data['matches']), 'date':dt.datetime.fromtimestamp(path.stat().st_mtime).isoformat(timespec='minutes'), 'folder':path.parent.name,'path':str(path.resolve())}
                except (OSError,ValueError,TypeError): continue
            self.catalog_time = time.monotonic()
            return list(self.catalog.values())

    def register_media(self, path):
        path = Path(path).resolve()
        if not path.is_file() or path.suffix.lower() not in VIDEO_EXTENSIONS: return None
        identifier = hashlib.sha256(str(path).encode()).hexdigest()[:32]
        with self.connect() as db: db.execute('INSERT OR IGNORE INTO media VALUES(?,?,?,?,1)',(identifier,str(path),path.name,path.stat().st_size))
        source=self.source_metadata(path)
        if source:self.link_media(identifier,source)
        return identifier

    def import_batch(self, identifier):
        self.batches()
        batch=self.catalog.get(identifier)
        if not batch:raise LabError('解析履歴を選び直してください。',404)
        preview=self.prepare_completed_path(Path(batch['path']))
        result=self.import_completed(preview['id'])
        result['excluded']=preview['excluded']
        return result

    def working_public_data(self, selected=None):
        records = []
        all_records=self.records();source_records=all_records if selected is None else selected
        public_ids={r['id'] for r in source_records if r.get('visibility')=='public'}
        if selected is not None:public_ids.update(r['id'] for r in self.public_data()['records'])
        source_by_id={r['id']:r for r in all_records}
        for record in source_records:
            if record.get('visibility') != 'public': continue
            clean = {key:value for key,value in record.items() if key in PUBLIC_FIELDS}
            if 'research' in clean: clean['research']={k:v for k,v in clean['research'].items() if k in ('stage','prompt','answer')}
            def public_reference(reference):
                clean_ref={k:v for k,v in reference.items() if k in REFERENCE_FIELDS and (k not in ('recordId','originId') or v in public_ids)}
                source=source_by_id.get(reference.get('recordId'))
                # A cited source may stay private. Only its current public-video
                # link/alignment may follow the citation, never its other fields.
                if source and 'analysisRevision' not in reference and (source.get('youtubeId') or source.get('youtubeLinkEdited')):
                    youtube=source.get('youtubeId','')
                    clean_ref.update(youtubeId=youtube,youtubeOffsetSeconds=source.get('youtubeOffsetSeconds',0) if youtube else 0)
                return clean_ref
            if 'sourceRef' in clean: clean['sourceRef']=public_reference(clean['sourceRef'])
            if 'citations' in clean: clean['citations']=[{**c,'sourceRef':public_reference(c.get('sourceRef',{}))} for c in clean['citations']]
            if record['kind']=='match':
                clean.setdefault('videoKey',record.get('mediaId') or record['id'])
                clean.setdefault('videoTitle',record['title'].rsplit(' · 試合',1)[0])
            if record.get('parentId') in public_ids:clean['parentId']=record['parentId']
            # Time notes have an independent opt-in; private notes never follow a public match.
            clean['bookmarks'] = [{**{k:m[k] for k in ('seconds','label','note','tags','player','phase') if k in m},
                                  **({'sourceRef':public_reference(m['sourceRef'])} if 'sourceRef' in m else {})}
                                  for m in record.get('bookmarks',[]) if m.get('visibility')=='public']
            if clean.get('status') == 'failed': clean.pop('simulator',None)
            records.append(clean)
        return {'schema':'tetris-lab/public-v1','records':records}

    def preview(self):
        value = self.public_data(); digest = hashlib.sha256(encode(value)).hexdigest()
        return {'digest':digest,'records':[{'id':r['id'],'title':r['title'],'kind':r['kind']} for r in value['records']],
                'privateCount':sum(r['id'] not in {p['id'] for p in value['records']} for r in self.records()), 'videoFiles':0}

    def backup(self):
        folder = self.data/'backups'; folder.mkdir(exist_ok=True)
        name = dt.datetime.now().strftime('%Y%m%d-%H%M%S-') + uuid.uuid4().hex[:8]+'.sqlite3'
        with self.connect() as source, contextlib.closing(sqlite3.connect(folder/name)) as target: source.backup(target)
        return {'file':name,'url':'/api/backups/'+name}

    def export_public(self, digest):
        with self.lock:
            if not secrets.compare_digest(str(digest),self.preview()['digest']): raise LabError('公開内容が変わっています。もう一度確認してください。',409)
            backup = self.backup(); value = self.public_data()
            for name, content in self.public_files(value).items():
                target=self.lab/name; temporary=target.with_suffix(target.suffix+'.writing')
                temporary.write_bytes(content); temporary.replace(target)
        return {'records':len(value['records']),'backup':backup,'message':'公開用ファイルを作成しました。オンラインにはまだ反映していません。'}

    def publish(self, digest):
        # Compatibility action republishes only the already published snapshot.
        # Each article's pending edits require the item-scoped preview endpoint.
        with self.publication_lock:
            if not secrets.compare_digest(str(digest),self.preview()['digest']): raise LabError('公開内容が変わっています。もう一度確認してください。',409)
            result=self._push_editorial_snapshot(self.public_data(),'snapshot-'+uuid.uuid4().hex)
            return {**result,'message':'公開済みの内容を送信しました。編集中の記事は含めていません。'}

    def media(self, identifier, ready=True):
        with self.connect() as db: row = db.execute('SELECT * FROM media WHERE id=? AND ready=1',(identifier,)).fetchone()
        if not row: raise LabError('動画が見つかりません。',404)
        return dict(row)

    def videos(self):
        with self.connect() as db: registered=[{'id':r['id'],'name':r['name']} for r in db.execute('SELECT id,name FROM media WHERE ready=1')]
        return [{**r,**(self.meta_get('media-source:'+r['id']) or {})} for r in registered]

    def select_video(self):
        # Explicitly opened by the owner's button; Windows supplies the full path.
        script = "Add-Type -AssemblyName System.Windows.Forms\n$d=New-Object System.Windows.Forms.OpenFileDialog\n$d.Filter='動画|*.mp4;*.mkv;*.mov;*.webm;*.avi;*.m4v'\n$d.Title='資料庫で解析する動画を選ぶ'\nif($d.ShowDialog() -eq 'OK'){[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;[Console]::Write($d.FileName)}"
        process=subprocess.run(['powershell','-NoProfile','-STA','-Command',script],capture_output=True,encoding='utf-8-sig',errors='replace',creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        if process.returncode: raise LabError('ファイル選択を開けませんでした。')
        if not process.stdout: return {'cancelled':True}
        identifier=self.register_media(process.stdout.strip())
        if not identifier: raise LabError('対応する動画を選んでください。')
        return {'id':identifier,'name':Path(process.stdout.strip()).name}

    def job_save(self, job):
        with self.connect() as db: db.execute('INSERT INTO jobs VALUES(?,?) ON CONFLICT(id) DO UPDATE SET body=excluded.body',(job['id'],encode(job).decode()))

    def _run_async(self, target, *args):
        # One place to start daemon work, so tests can run it synchronously.
        threading.Thread(target=target,args=args,daemon=True).start()

    def _new_analysis_job(self, media_id, title, streaming, youtube_id=None):
        job={'id':uuid.uuid4().hex,'kind':'video','title':title,'status':'running','progress':'試合を探しています','created':stamp()}
        if media_id: job['mediaId']=media_id
        if streaming: job['streaming']=True
        if youtube_id: job['youtubeId']=youtube_id
        return job

    def _video_analysis_running(self):
        # Callers hold self.job_lock. Score jobs are the only other kind and never block this.
        return any(j.get('status')=='running' and j.get('kind')=='video' for j in self.jobs.values())

    def _start_analysis(self, media_id, video, title=None, streaming=None, youtube_id=None):
        # Internal entry point for the fetch pipeline: it deliberately bypasses
        # the owner-facing analyze() gate, but still allows only one analysis.
        video=Path(video); title=title or video.name
        with self.job_lock:
            if self._video_analysis_running():
                self.pending_analysis.append((media_id,str(video),title,streaming,youtube_id))
                return None
            job=self._new_analysis_job(media_id,title,streaming,youtube_id)
            self.jobs[job['id']]=job; self.job_save(job)
        if streaming: self._run_async(self._analyze,job,video,streaming)
        else: self._run_async(self._analyze,job,video)
        return dict(job)

    def _drain_pending_analysis(self):
        # Called when a native analysis ends. A queued hand-off stays invisible
        # in the job list until it actually starts, so no new UI state is needed.
        with self.job_lock:
            if not self.pending_analysis or self._video_analysis_running(): return
            media_id, video, title, streaming, youtube_id = self.pending_analysis.pop(0)
            job=self._new_analysis_job(media_id,title,streaming,youtube_id)
            self.jobs[job['id']]=job; self.job_save(job)
        if streaming: self._run_async(self._analyze,job,Path(video),streaming)
        else: self._run_async(self._analyze,job,Path(video))

    def analyze(self, media_id):
        media = self.media(media_id); path = Path(media['path'])
        if not path.is_file(): raise LabError('元動画が移動・削除されています。')
        source = self.meta_get('media-source:'+media_id) or {}
        with self.job_lock:
            if self._video_analysis_running(): raise LabError('別の動画を解析中です。終了後に実行してください。',409)
            job=self._new_analysis_job(media_id,media['name'],None,source.get('youtubeId'))
            self.jobs[job['id']]=job; self.job_save(job)
        self._run_async(self._analyze,job,path)
        return dict(job)

    def _analyze(self, job, video, streaming=None):
        runtime=self.root/'tools/video-recovery-runtime'; output=self.data/'analyses'/job['id']; output.mkdir(parents=True,exist_ok=True)
        command=[str(runtime/'bin/TetrisVideoRecovery_stream.exe'),str(video),'--detect',str(output),str(runtime/'models/scene-classifier.onnx'),str(runtime/'models/tetris.onnx'),str(runtime/'config/tetris_recover.ini')]
        # 8GB GPU で 3 ワーカーを常時動かすための予算（実測の最適点）
        environment={**os.environ,
                     'TR_BATCH_WORKERS':'3',
                     'TR_BOARD_WORKERS':'2',
                     'TR_BATCH_WORKER_RAM_MB':'1536',
                     'TR_BATCH_RAM_RESERVE_MB':'2048',
                     'TR_BATCH_WORKER_VRAM_MB':'1024',
                     'TR_BATCH_VRAM_RESERVE_MB':'512',
                     'TR_BATCH_RELEASE_DELAY_SECONDS':'5'}
        if streaming:
            # Level 2: the engine follows the fragmented file and stops at the
            # last confirmed playable second minus this margin.
            environment.update({'TR_STREAM_READY_FILE':str(streaming['ready']),
                                'TR_STREAM_DONE_FILE':str(streaming['done']),
                                'TR_STREAM_MARGIN_SECONDS':str(streaming.get('margin',8))})
        try:
            with (output/'analysis.log').open('wb') as log:
                process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,cwd=runtime,env=environment,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                with self.job_lock:
                    self.processes[job['id']]=process
                    if job['status']=='cancelled': process.terminate()
                for line in process.stdout:
                    log.write(line)
                    text=line.decode('utf-8',errors='replace').strip()
                    match=re.match(r'match_done=(\d+)',text)
                    if match: job['progress']=f'試合 {match[1]} の処理が終了'; self.job_save(job)
                code=process.wait()
            if job['status']!='cancelled':
                if (output/'batch-results.json').exists():
                    self.batches(True); key=hashlib.sha256(str((output/'batch-results.json').resolve()).encode()).hexdigest()[:24]
                    job['import']=self.import_batch(key); job['status']='complete'; job['progress']='解析結果を資料庫に登録しました'
                else: job['status']='failed'; job['progress']='解析を完了できませんでした。動画とGPU環境を確認してください。'
            job['exitCode']=code
        except Exception as error:
            job['status']='failed'; job['progress']='解析を開始できませんでした'; job['detail']=str(error)
        finally:
            with self.job_lock: self.processes.pop(job['id'],None)
            self.job_save(job)
            self._drain_pending_analysis()

    def cancel(self, identifier):
        with self.job_lock:
            job=self.jobs.get(identifier)
            if not job or job['status']!='running': raise LabError('解析中の処理がありません。',409)
            job['status']='cancelled'; job['progress']='中止しました'
            process=self.processes.get(identifier)
            if process: process.terminate()
            self.job_save(job); return dict(job)

class Server(ThreadingHTTPServer):
    daemon_threads=True
    request_queue_size=64
    def __init__(self, port, store, tools_only=False, *, max_requests=32, connection_timeout=20):
        self.store=store; self.token=secrets.token_urlsafe(32); self.tools_only=tools_only
        self.connection_timeout=connection_timeout
        self.request_slots=threading.BoundedSemaphore(max_requests)
        super().__init__(('127.0.0.1',port),Handler)
    def get_request(self):
        request,address=super().get_request()
        # Browser preconnections and interrupted streams must eventually release their threads.
        request.settimeout(self.connection_timeout)
        return request,address
    def process_request(self, request, client_address):
        if not self.request_slots.acquire(blocking=False):
            try:
                request.sendall(b'HTTP/1.0 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
            except OSError: pass
            finally: self.shutdown_request(request)
            return
        try: super().process_request(request,client_address)
        except BaseException:
            self.request_slots.release()
            raise
    def process_request_thread(self, request, client_address):
        try: super().process_request_thread(request,client_address)
        finally: self.request_slots.release()

class Handler(BaseHTTPRequestHandler):
    server: Server
    def log_message(self, format, *args): pass
    def headers_common(self):
        self.send_header('X-Content-Type-Options','nosniff'); self.send_header('Referrer-Policy','strict-origin-when-cross-origin')
        self.send_header('Cache-Control','no-store')
        if not self.server.tools_only: self.send_header('X-Frame-Options','DENY')
    def reply(self, body, status=200):
        payload=encode(body); self.send_response(status); self.headers_common()
        self.send_header('Content-Type','application/json; charset=utf-8'); self.send_header('Content-Length',str(len(payload))); self.end_headers()
        if self.command!='HEAD': self.wfile.write(payload)
    def guard(self, write=False):
        port=self.server.server_port
        if self.headers.get('Host') not in (f'127.0.0.1:{port}',f'localhost:{port}'): raise LabError('このPCの管理画面からアクセスしてください。',403)
        if self.headers.get('Sec-Fetch-Site')=='cross-site': raise LabError('外部サイトからはアクセスできません。',403)
        origin=self.headers.get('Origin')
        if origin and origin not in (f'http://127.0.0.1:{port}',f'http://localhost:{port}'): raise LabError('外部サイトからはアクセスできません。',403)
        if write and (self.server.tools_only or not secrets.compare_digest(self.headers.get('X-Lab-Token',''),self.server.token)): raise LabError('管理画面を再読み込みしてください。',403)
    def body(self, limit=8*1024**2):
        length=int(self.headers.get('Content-Length','0'))
        if not 0 < length <= limit: raise LabError('送信内容が大きすぎるか、空です。',413)
        body=self.rfile.read(length)
        if len(body)!=length: raise LabError('送信が中断されました。')
        return body
    def file(self, path, content=None):
        path=Path(path)
        if not path.is_file(): raise LabError('ファイルが見つかりません。',404)
        size=path.stat().st_size if content is None else len(content)
        start,end,status=0,size-1,200
        range_header=self.headers.get('Range')
        if range_header:
            match=re.fullmatch(r'bytes=(\d*)-(\d*)',range_header)
            if not match or (not match[1] and not match[2]): raise LabError('指定された範囲を読み込めません。',416)
            if match[1]: start=int(match[1]); end=min(int(match[2]),end) if match[2] else end
            else: start=max(0,size-int(match[2]))
            if start>end or start>=size:
                self.send_response(416); self.send_header('Content-Range',f'bytes */{size}'); self.end_headers(); return
            status=206
        self.send_response(status); self.headers_common(); self.send_header('Accept-Ranges','bytes')
        self.send_header('Content-Type',mimetypes.guess_type(path.name)[0] or 'application/octet-stream')
        self.send_header('Content-Length',str(max(0,end-start+1)))
        if status==206: self.send_header('Content-Range',f'bytes {start}-{end}/{size}')
        self.end_headers()
        if self.command=='HEAD': return
        if content is not None: self.wfile.write(content[start:end+1]); return
        with path.open('rb') as source:
            source.seek(start); left=end-start+1
            while left>0:
                chunk=source.read(min(left,1024**2))
                if not chunk: break
                self.wfile.write(chunk); left-=len(chunk)
    def do_HEAD(self): self.do_GET()
    def do_GET(self):
        try:
            self.guard(); store=self.server.store
            url=urlsplit(self.path); path=unquote(url.path); query=parse_qs(url.query)
            if self.server.tools_only:
                if path=='/': path='/index.html'
                if path.endswith('/'): path+='index.html'
                if path=='/lab-bridge.js': self.file(store.root/'tools/tetris-lab/bridge.js'); return
                if path=='/lab-theme.css': self.file(store.root/'tools/tetris-lab/preview-theme.css'); return
                if path=='/lab-appearance.js': self.file(store.root/'tools/tetris-lab/preview-appearance.js'); return
                relative=Path(path.lstrip('/'))
                allowed=relative.parts[0] in ('simulator','F','shared','styles','assets','icons','wasm','models') or path in ('/index.html','/manifest.webmanifest','/favicon.ico')
                target=store.root/relative
                if not allowed or not within(target,store.root) or any(p.startswith('.') for p in relative.parts): raise LabError('ファイルが見つかりません。',404)
                # Private study playback does not send page visits to analytics.
                if path in ('/index.html','/F/index.html'):
                    html=target.read_text(encoding='utf-8')
                    html=re.sub(r'<script async src="https://www.googletagmanager.com/[^<]+</script>\s*<script>[\s\S]*?</script>','',html,count=1)
                    if query.get('theme')!=['original']:
                        html=html.replace('</head>','<link rel="stylesheet" href="/lab-theme.css"><script src="/lab-appearance.js"></script></head>')
                        html=html_variant(html,path)
                    html=html.replace('</body>','<script src="/lab-bridge.js"></script></body>')
                    self.file(target,html.encode()); return
                if path in PREVIEW_SCRIPT_PATHS and query.get('labTheme')==['lab']:
                    source=script_variant(path,target.read_bytes().decode('utf-8'))
                    self.file(target,source.encode('utf-8')); return
                self.file(target); return
            if path=='/api/session': self.reply({'token':self.server.token,'toolOrigin':f'http://127.0.0.1:{self.server.server_port+1}'}); return
            if path=='/api/health': self.reply({'app':'tetris-lab-personal','version':1}); return
            if path=='/api/records': self.reply({'records':store.records_with_publication(query.get('trash')==['1'])}); return
            if path=='/api/boundary-warnings': self.reply(store.boundary_warnings()); return
            if path=='/api/editorial': self.reply(store.editorial(query.get('trash')==['1'])); return
            if path=='/api/research-folders': self.reply(store.research_folders()); return
            if path=='/api/batches': self.reply({'batches':[{k:v for k,v in x.items() if k!='path'} for x in store.batches()]}); return
            if path=='/api/public-preview': self.reply(store.preview()); return
            if path=='/api/videos': self.reply({'videos':store.videos()}); return
            if path=='/api/jobs': self.reply({'jobs':store.job_list()}); return
            if path=='/api/scoring-settings': self.reply(store.scoring_settings()); return
            if path=='/api/score-markers': self.reply(store.score_markers(query.get('ids',[''])[0].split(','))); return
            if path=='/api/draft': self.reply(store.draft(query.get('id',[''])[0])); return
            if path=='/api/drafts':
                with store.connect() as db:
                    drafts=[]
                    for row in db.execute("SELECT key,body FROM metadata WHERE key LIKE 'article-draft:%'"):
                        value=json.loads(row['body'])
                        if isinstance(value,dict) and 'content' in value:
                            drafts.append({'id':row['key'][14:],'title':value.get('title',''),'savedAt':value.get('savedAt',''),'kind':value.get('kind','article'),'editorialFormat':value.get('editorialFormat','text')})
                self.reply({'drafts':drafts}); return
            if path.startswith('/api/backups/'):
                name=path.rsplit('/',1)[1]
                if not re.fullmatch(r'[\w-]+\.sqlite3',name): raise LabError('ファイルが見つかりません。',404)
                self.file(store.data/'backups'/name); return
            if path.startswith('/api/completed-files/'):
                self.file(store.completed_file(path.rsplit('/',1)[1])); return
            # Explicit static allowlist: never expose SQLite, local paths, .git or source scripts.
            if path=='/': path='/index.html'
            allowed=path in ('/index.html','/lab-enhancements.js','/lab-enhancements.css','/lab-writer.css','/lab-manager.js','/lab-manager.css','/vendor/lab-editor.js','/vendor/editor-licenses.txt','/lab-research.js','/lab-videos.js','/library.html','/library.css','/library.js','/library-tools.js','/lab-boundary.html','/layout-check.html','/queue-check.html') or path.startswith('/data/')
            target=store.lab/path.lstrip('/')
            if not allowed or not within(target,store.lab) or any(p.startswith('.') for p in Path(path).parts if p!='/'): raise LabError('ページが見つかりません。',404)
            self.file(target)
        except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError,TimeoutError): pass
        except LabError as error: self.reply({'error':str(error)},error.status)
        except Exception: self.reply({'error':'読み込みに失敗しました。'},500)
    def do_POST(self):
        try:
            self.connection.settimeout(20)
            self.guard(True)
            if not self.headers.get('Content-Type','').startswith('application/json'): raise LabError('JSON形式で送信してください。',415)
            payload=self.body(32*1024**2 if urlsplit(self.path).path=='/api/prepare-upload' else 8*1024**2)
            value=json.loads(payload); path=urlsplit(self.path).path; store=self.server.store
            if path=='/api/records': result=store.save(value)
            elif path=='/api/article-save': result=store.article_save(value)
            elif path=='/api/analysis-article': result=store.analysis_article(value)
            elif path=='/api/research-folders': result=store.save_research_folder(value)
            elif path=='/api/article-preview': result=store.article_preview(value)
            elif path=='/api/article-publish': result=store.article_publish(value)
            elif path=='/api/article-revert': result=store.article_revert(value)
            elif path=='/api/article-trash': result=store.article_trash(value)
            elif path=='/api/video-settings': result=store.video_settings(value)
            elif path=='/api/scoring-settings': result=store.scoring_settings(value)
            elif path=='/api/score': result=store.score(value)
            elif path=='/api/draft': result=store.draft(value.get('id',''),value)
            elif path=='/api/sync-sources': result=store.sync_media_links()
            elif path=='/api/open-native': result=store.open_native()
            elif path=='/api/choose-completed': result=store.choose_completed()
            elif path=='/api/prepare-upload': result=store.prepare_completed(value)
            elif path=='/api/prepare-import':
                store.batches()
                batch=store.catalog.get(value.get('id'))
                if not batch: raise LabError('解析履歴を選び直してください。',404)
                result=store.prepare_completed_path(Path(batch['path']))
            elif path=='/api/import-completed': result=store.import_completed(value.get('id'))
            elif path=='/api/bulk-update': result=store.bulk_update(value)
            elif path=='/api/trash': result=store.trash(value['id'],value['revision'],value.get('restore',False))
            elif path=='/api/trash-preview': result=store.trash_preview(value)
            elif path=='/api/bulk-trash': result=store.bulk_trash(value)
            elif path=='/api/import-batch': result=store.import_batch(value['id'])
            elif path=='/api/export-public': result=store.export_public(value.get('digest'))
            elif path=='/api/publish': result=store.publish(value.get('digest'))
            elif path=='/api/backup': result=store.backup()
            elif path=='/api/cancel': result=store.cancel(value['id'])
            elif path=='/api/boundary-correct': result=store.boundary_correct(value)
            elif path=='/api/boundary-restore': result=store.boundary_restore(value)
            else: raise LabError('操作が見つかりません。',404)
            self.reply(result)
        except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError,TimeoutError): pass
        except LabError as error: self.reply({'error':str(error)},error.status)
        except (ValueError,KeyError,TypeError): self.reply({'error':'入力内容を確認してください。'},400)
        except Exception: self.reply({'error':'保存に失敗しました。'},500)

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--lab',type=Path,default=DEFAULT_LAB); parser.add_argument('--data',type=Path,default=DEFAULT_DATA)
    parser.add_argument('--port',type=int,default=8780); parser.add_argument('--open',action='store_true'); args=parser.parse_args()
    args.data.mkdir(parents=True,exist_ok=True)
    # pythonw has no terminal streams. Keep diagnostics even when launched from the desktop.
    if sys.stdout is None: sys.stdout=(args.data/'server.log').open('a',encoding='utf-8',buffering=1)
    if sys.stderr is None: sys.stderr=(args.data/'server-error.log').open('a',encoding='utf-8',buffering=1)
    store=Store(args.lab,args.data); server=Server(args.port,store); tool_server=Server(args.port+1,store,True)
    threading.Thread(target=tool_server.serve_forever,daemon=True).start()
    url=f'http://127.0.0.1:{args.port}/index.html'
    print(url,flush=True)
    if args.open: webbrowser.open(url)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally:
        for process in list(store.processes.values()): process.terminate()
        server.server_close(); tool_server.shutdown(); tool_server.server_close()

if __name__=='__main__': main()
