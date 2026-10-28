"""公開Lab（GitHub Pages）の動画・試合・タグを PPT Lab に取り込む（再実行しても安全）。

- Lab DB の records から video / match / tetofu を読む（deleted は除外）
- 動画ごとに `imported/<videoId>/manifest.json` と `winner-detection.json` を作る
- 同じYouTube IDの動画が既にあれば作り直さずスキップ
- タグは Lab 由来のものだけを対象にし、手動タグは消さない
"""
import base64
import json
import sqlite3
import sys
from collections import defaultdict
from datetime import datetime, timezone
from hashlib import sha1
from pathlib import Path

LAB_DB = Path(r'C:\Users\hirom\Documents\tetris ai\tools\video-recovery-runtime\data\lab-private\lab.sqlite3')
PPT_DIR = Path.home() / 'AppData/Local/PPTLab'
PPT_DB = PPT_DIR / 'pptlab.sqlite3'
IMPORT_DIR = PPT_DIR / 'imported'


def load_lab_records():
    db = sqlite3.connect(str(LAB_DB))
    db.row_factory = sqlite3.Row
    records = []
    skipped = {'video': 0, 'match': 0, 'tetofu': 0}
    for row in db.execute('SELECT id, body, deleted FROM records'):
        try:
            body = json.loads(row['body'])
        except json.JSONDecodeError:
            continue
        if row['deleted']:
            kind = str(body.get('kind') or '')
            if kind in skipped:
                skipped[kind] += 1
            continue
        records.append(body)
    db.close()
    return records, skipped


def main():
    if not LAB_DB.exists():
        print('Lab DB が見つかりません:', LAB_DB)
        raise SystemExit(1)
    if not PPT_DB.exists():
        print('PPT Lab DB が見つかりません:', PPT_DB)
        raise SystemExit(1)

    IMPORT_DIR.mkdir(parents=True, exist_ok=True)
    records, skipped = load_lab_records()
    videos = [item for item in records if item.get('kind') == 'video']
    matches = [item for item in records if item.get('kind') == 'match']
    tetofu = [item for item in records if item.get('kind') == 'tetofu']

    by_video = defaultdict(list)
    for match in matches:
        key = str(match.get('youtubeId') or match.get('videoKey') or '')
        by_video[key].append(match)

    db = sqlite3.connect(str(PPT_DB))
    db.row_factory = sqlite3.Row
    now = datetime.now(timezone.utc).astimezone().isoformat(timespec='seconds')

    # 既存動画の YouTube ID を集める（source / タイトルの [ID] / マニフェストの source）
    import re
    existing_sources = {}
    for row in db.execute('SELECT id, source, title, manifest FROM videos'):
        source = str(row['source'] or '')
        title = str(row['title'] or '')
        found = ''
        if 'youtube.com/watch?v=' in source:
            found = source.split('watch?v=')[-1][:11]
        if not found:
            hit = re.search(r'\[([A-Za-z0-9_-]{11})\]', title)
            if hit:
                found = hit.group(1)
        if not found and row['manifest']:
            try:
                data = json.loads(Path(row['manifest']).read_text(encoding='utf-8-sig'))
                match = re.search(r'watch\?v=([A-Za-z0-9_-]{11})', str(data.get('source') or ''))
                if match:
                    found = match.group(1)
            except (OSError, json.JSONDecodeError):
                pass
        if found:
            existing_sources[found] = row['id']

    imported = skipped_dupes = 0
    for video in videos:
        youtube = str(video.get('youtubeId') or '').strip()
        title = str(video.get('title') or '').strip() or youtube
        if not youtube:
            continue
        rows = sorted(by_video.get(youtube, []), key=lambda item: float(item.get('startSeconds') or 0))
        if not rows:
            continue
        if youtube in existing_sources:
            print(f'skip (既にあり): {title[:36]} ({youtube}) -> {existing_sources[youtube]}')
            skipped_dupes += 1
            continue

        video_id = 'video-lab-' + sha1(youtube.encode('utf-8')).hexdigest()[:16]
        folder = IMPORT_DIR / video_id
        folder.mkdir(parents=True, exist_ok=True)

        manifest_matches = []
        winners = []
        unnamed = 0
        for index, match in enumerate(rows, start=1):
            players = [str(name or '').strip() for name in (match.get('players') or [])]
            if not any(players):
                unnamed += 1
            entry = {
                'index': index,
                'source_range': {
                    'start_seconds': float(match.get('startSeconds') or 0),
                    'end_seconds': float(match.get('endSeconds') or 0)
                },
                'player_names': {
                    'p1': {'text': players[0] if len(players) > 0 else ''},
                    'p2': {'text': players[1] if len(players) > 1 else ''}
                },
                'autosave': '',
                'labMatchId': match.get('id', ''),
                'labSimulator': match.get('simulator') or {}
            }
            manifest_matches.append(entry)
            winner = str(match.get('winner') or '').strip()
            status = str(match.get('winnerStatus') or 'ok')
            if winner in ('p1', 'p2'):
                winners.append({'match': index, 'winner': winner, 'status': status})

        manifest_path = folder / 'manifest.json'
        manifest_path.write_text(json.dumps({
            'source': f'https://www.youtube.com/watch?v={youtube}',
            'matches': manifest_matches,
            'importedFrom': 'lab'
        }, ensure_ascii=False, indent=2), encoding='utf-8')
        (folder / 'winner-detection.json').write_text(
            json.dumps({'matches': winners}, ensure_ascii=False, indent=2), encoding='utf-8')

        status = 'ready' if unnamed == 0 else 'needs-names'
        db.execute(
            '''INSERT INTO videos (id, title, status, source, created_at, match_count, manifest, tags_locked, data)
               VALUES (?, ?, ?, ?, ?, ?, ?, 0, '{}')
               ON CONFLICT(id) DO UPDATE SET title=excluded.title, status=excluded.status,
                   source=excluded.source, match_count=excluded.match_count, manifest=excluded.manifest''',
            (video_id, title, status, f'https://www.youtube.com/watch?v={youtube}', now,
             len(manifest_matches), str(manifest_path)))

        # Lab 由来のタグだけ入れる（手動タグは触らない）
        lab_tags = [str(name) for name in (video.get('tags') or []) if name]
        for match in rows:
            for name in (match.get('tags') or []):
                if name and str(name) not in lab_tags:
                    lab_tags.append(str(name))
        for name in lab_tags:
            db.execute('INSERT OR IGNORE INTO tags(name, created_at) VALUES(?,?)', (name, now))
            db.execute('INSERT OR IGNORE INTO tag_links(tag, target, created_at) VALUES(?,?,?)',
                       (name, video_id, now))
        imported += 1
        print(f'video: {title[:36]} ({youtube}) matches={len(manifest_matches)} status={status} tags={lab_tags}')

    tetofu_count = 0
    for item in tetofu:
        url = str(item.get('url') or '').strip()
        title = str(item.get('title') or '').strip() or 'テト譜'
        if not url:
            continue
        if db.execute('SELECT 1 FROM tetofu WHERE url=?', (url,)).fetchone():
            continue
        identifier = 'tetofu-lab-' + sha1(url.encode('utf-8')).hexdigest()[:16]
        db.execute('INSERT INTO tetofu (id, kind, title, url, created_at) VALUES(?,?,?,?,?)',
                   (identifier, 'simulator', title, url, now))
        for name in (item.get('tags') or []):
            db.execute('INSERT OR IGNORE INTO tags(name, created_at) VALUES(?,?)', (name, now))
            db.execute('INSERT OR IGNORE INTO tag_links(tag, target, created_at) VALUES(?,?,?)',
                       (name, identifier, now))
        tetofu_count += 1
        print(f'tetofu: {title[:36]}')

    db.commit()
    print(f'imported videos={imported} (skip={skipped_dupes}) tetofu={tetofu_count}')
    print('skipped deleted:', skipped)
    db.close()


if __name__ == '__main__':
    main()
