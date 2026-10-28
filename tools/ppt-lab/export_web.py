"""PPT Lab のデータを、閲覧専用の静的サイト用に書き出す。

出力: tools/ppt-lab/webdata/ （build-web.py が dist/pages/ppt-lab/data/ にコピーする）
"""
import json
import shutil
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
from namereview import read_player_names  # noqa: E402
import server  # noqa: E402

replay_file = server.replay_file

DB = Path.home() / 'AppData/Local/PPTLab/pptlab.sqlite3'
CROPS = Path.home() / 'AppData/Local/PPTLab/name-crops'
OUT = ROOT / 'webdata'


def youtube_of(source: str, title: str) -> str:
    source = str(source or '')
    if 'youtube.com/watch?v=' in source:
        return source.split('watch?v=')[-1][:11]
    if 'youtu.be/' in source:
        return source.split('youtu.be/')[-1][:11]
    for marker in ('/live/', '/shorts/', '/embed/'):
        if marker in source:
            tail = source.split(marker)[-1]
            candidate = ''.join(ch for ch in tail[:11] if ch.isalnum() or ch in '-_')
            if len(candidate) == 11:
                return candidate
    for token in str(title or '').replace(']', '[').split('['):
        token = token.strip()
        if len(token) == 11 and all(ch.isalnum() or ch in '-_' for ch in token):
            return token
    return ''


def main() -> int:
    if not DB.exists():
        print('DB がありません:', DB)
        return 1
    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / 'matches').mkdir(parents=True)
    (OUT / 'name-crops').mkdir(parents=True)
    (OUT / 'replays').mkdir(parents=True)

    db = sqlite3.connect(str(DB))
    db.row_factory = sqlite3.Row

    tags = [{'name': row['name'], 'count': db.execute(
        'SELECT COUNT(*) FROM tag_links WHERE tag=?', (row['name'],)).fetchone()[0]}
        for row in db.execute('SELECT name FROM tags ORDER BY name')]

    links = {}
    for row in db.execute('SELECT tag, target FROM tag_links'):
        links.setdefault(row['target'], []).append(row['tag'])

    videos = []
    for row in db.execute('SELECT id, title, status, source, created_at, match_count, manifest FROM videos '
                          'ORDER BY created_at DESC'):
        youtube = youtube_of(row['source'], row['title'])
        matches = []
        manifest = Path(row['manifest']) if row['manifest'] else None
        if manifest and manifest.is_file():
            try:
                data = json.loads(manifest.read_text(encoding='utf-8-sig'))
            except (OSError, json.JSONDecodeError):
                data = {}
            winners = {}
            for name in ('winner-detection.json', 'winners.json'):
                candidate = manifest.parent / name
                if not candidate.is_file():
                    continue
                try:
                    payload = json.loads(candidate.read_text(encoding='utf-8-sig'))
                except (OSError, json.JSONDecodeError):
                    continue
                for entry in payload.get('matches') or []:
                    winners[int(entry.get('match', 0))] = entry
                break
            for item in data.get('matches') or []:
                index = int(item.get('index', 0))
                names = item.get('player_names') or {}
                p1 = str((names.get('p1') or {}).get('text') or '')
                p2 = str((names.get('p2') or {}).get('text') or '')
                # 名前を確認済みなら autosave の確定名を優先する
                autosave = Path(str(item.get('autosave') or ''))
                if not autosave.is_absolute():
                    autosave = manifest.parent / autosave
                if autosave.is_file():
                    try:
                        saved = read_player_names(autosave)
                        p1 = str(saved.get('p1') or p1)
                        p2 = str(saved.get('p2') or p2)
                    except Exception:
                        pass
                winner = winners.get(index) or {}
                matches.append({
                    'index': index,
                    'start': float((item.get('source_range') or {}).get('start_seconds') or 0),
                    'end': float((item.get('source_range') or {}).get('end_seconds') or 0),
                    'p1': p1, 'p2': p2,
                    'winner': str(winner.get('winner') or ''),
                    'winnerStatus': str(winner.get('status') or ''),
                    'p1Image': '', 'p2Image': '',
                    'p1Group': 0, 'p2Group': 0,
                    'p1Guess': '', 'p2Guess': '',
                    'p1Fingerprint': '', 'p2Fingerprint': ''
                })
        players = []
        for match in matches:
            for name in (match['p1'], match['p2']):
                if name and name not in players:
                    players.append(name)
        (OUT / 'matches' / f'{row["id"]}.json').write_text(
            json.dumps({'matches': matches, 'warning': ''}, ensure_ascii=False),
            encoding='utf-8')
        # 盤面ビューワー用: 試合ごとの状態ハッシュ（F/ のURLの # 以降）
        hashes = {}
        for match in matches:
            index = match['index']
            url = ''
            replay = replay_file(manifest, index) if manifest else None
            if replay and replay.is_file():
                try:
                    payload = json.loads(replay.read_text(encoding='utf-8-sig'))
                    url = str((payload.get('urls') or {}).get('combined') or '')
                except (OSError, json.JSONDecodeError):
                    url = ''
            if not url:
                simulator = (data.get('matches') or [])
                for item in simulator:
                    if int(item.get('index', 0)) == index:
                        lab = item.get('labSimulator') or {}
                        # p1 / p2 は片側だけの1Pコレクション。両者の盤面が
                        # 入っている combined を優先する（P1だけになる不具合の修正）。
                        url = str(lab.get('combined') or lab.get('p1') or lab.get('p2') or '')
                        break
            if '#' in url:
                hashes[str(index)] = url.split('#', 1)[1]
        if hashes:
            (OUT / 'replays' / f'{row["id"]}.json').write_text(
                json.dumps(hashes, ensure_ascii=False), encoding='utf-8')

        videos.append({
            'id': row['id'], 'title': row['title'], 'status': row['status'],
            'source': f'https://www.youtube.com/watch?v={youtube}' if youtube else '',
            'youtubeId': youtube, 'createdAt': row['created_at'],
            'matchCount': row['match_count'], 'players': players,
            'tags': links.get(row['id'], []), 'autoTags': [],
            'progress': 100, 'message': ''
        })
        print(f'video: {row["title"][:36]} matches={len(matches)}')

    tetofu = [{'id': row['id'], 'kind': row['kind'], 'title': row['title'], 'url': row['url'],
               'createdAt': row['created_at'], 'tags': links.get(row['id'], [])}
              for row in db.execute('SELECT * FROM tetofu ORDER BY created_at DESC')]

    mappings = {}
    for row in db.execute('SELECT fingerprint, name, updated_at, hits FROM name_memory'):
        entry = mappings.setdefault(row['name'], {'name': row['name'], 'fingerprints': [],
                                                  'hits': 0, 'updatedAt': row['updated_at'], 'image': ''})
        entry['fingerprints'].append(row['fingerprint'])
        entry['hits'] += int(row['hits'] or 0)
        crop = CROPS / f'{row["fingerprint"]}.png'
        if crop.is_file():
            shutil.copyfile(crop, OUT / 'name-crops' / f'{row["fingerprint"]}.png')
            if not entry['image']:
                entry['image'] = f'data/name-crops/{row["fingerprint"]}.png'

    (OUT / 'videos.json').write_text(json.dumps({'videos': videos}, ensure_ascii=False), encoding='utf-8')
    (OUT / 'tags.json').write_text(json.dumps({'tags': tags}, ensure_ascii=False), encoding='utf-8')
    (OUT / 'tetofu.json').write_text(json.dumps({'tetofu': tetofu}, ensure_ascii=False), encoding='utf-8')
    (OUT / 'name-mappings.json').write_text(
        json.dumps({'mappings': sorted(mappings.values(), key=lambda item: -item['hits'])}, ensure_ascii=False),
        encoding='utf-8')
    (OUT / 'settings.json').write_text(json.dumps({'settings': {}}, ensure_ascii=False), encoding='utf-8')
    (OUT / 'env.json').write_text(json.dumps({'engine': False, 'ffmpeg': False, 'gpu': False,
                                              'python': 'static'}, ensure_ascii=False), encoding='utf-8')
    db.close()
    print(f'done: videos={len(videos)} tetofu={len(tetofu)} tags={len(tags)} mappings={len(mappings)}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
