"""解析結果フォルダ（scratch/regress/<run>/）を PPT Lab の動画一覧に登録する（再実行しても安全）。

- <run>/<name>/batch-results.json を1動画として登録する（manifest にそのパスを入れるだけで、ファイルは動かさない）
- 同じ配信の部分集合（lab-*）は half-* と重複するので既定では除外
- タイトルは Lab DB の動画タイトル（無ければ同じ動画の既存タイトル）。--label を付けたときだけ先頭に [<ラベル>] とタグを付ける
- 動画一覧から削除しても、PPT Lab の消去対象（batches / media フォルダ）の外なので解析結果は消えない

使い方:
  python tools/ppt-lab/register_runs.py scratch/regress/v74c-hole-lookahead-all23
"""
import argparse
import json
import re
import sqlite3
from datetime import datetime
from hashlib import sha1
from pathlib import Path

LAB_DB = Path(r'C:\Users\hirom\Documents\tetris ai\tools\video-recovery-runtime\data\lab-private\lab.sqlite3')
PPT_DB = Path.home() / 'AppData/Local/PPTLab/pptlab.sqlite3'


def lab_titles() -> dict[str, str]:
    titles: dict[str, str] = {}
    if not LAB_DB.exists():
        return titles
    db = sqlite3.connect(str(LAB_DB))
    for (body,) in db.execute('SELECT body FROM records WHERE deleted=0'):
        try:
            item = json.loads(body)
        except json.JSONDecodeError:
            continue
        if item.get('kind') == 'video' and item.get('youtubeId'):
            titles[str(item['youtubeId'])] = str(item.get('title') or '')
    db.close()
    return titles


def youtube_id(source: str) -> str:
    hit = re.search(r'\[([A-Za-z0-9_-]{11})\]', source)
    if hit:
        return hit.group(1)
    stem = Path(source).stem
    return stem if re.fullmatch(r'[A-Za-z0-9_-]{11}', stem) else ''


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('run', type=Path)
    parser.add_argument('--label', default='')
    parser.add_argument('--include-lab', action='store_true', help='lab-* も登録する')
    args = parser.parse_args()

    titles = lab_titles()
    db = sqlite3.connect(str(PPT_DB))
    now = datetime.now().isoformat(timespec='seconds')
    if args.label:
        db.execute('INSERT OR IGNORE INTO tags(name, created_at) VALUES(?,?)', (args.label, now))
    # 同じ動画が既にあればそのタイトルを使う（Lab DB にタイトルが無い動画のため）
    known: dict[str, str] = {}
    for old_title, old_source, old_manifest in db.execute(
            "SELECT title, source, manifest FROM videos WHERE id NOT LIKE 'video-run-%'"):
        vid_old = youtube_id(str(old_source)) or youtube_id(str(old_title))
        if not vid_old and old_manifest:
            try:
                vid_old = youtube_id(str(json.loads(Path(old_manifest).read_text(encoding='utf-8-sig')).get('source') or ''))
            except (OSError, json.JSONDecodeError):
                vid_old = ''
        if vid_old and old_title:
            known.setdefault(vid_old, str(old_title))
    total = 0
    for manifest in sorted(args.run.resolve().glob('*/batch-results.json')):
        name = manifest.parent.name
        if name.startswith('lab-') and not args.include_lab:
            continue
        data = json.loads(manifest.read_text(encoding='utf-8-sig'))
        source = str(data.get('source') or '')
        vid = youtube_id(source)
        title = titles.get(vid) or known.get(vid) or vid or name
        if args.label:
            title = f'[{args.label}] ' + title
        # 再生元は YouTube にする（解析用の手元の mp4 は断片化 MP4 で、ブラウザが
        # 全体を読み終えるまで再生できない）。YouTube の ID が無いときだけ手元のファイル。
        play_source = f'https://www.youtube.com/watch?v={vid}' if vid else str(Path(source))
        count = len(data.get('matches') or [])
        video_id = 'video-run-' + sha1(str(manifest).lower().encode('utf-8')).hexdigest()[:16]
        db.execute(
            '''INSERT INTO videos (id, title, status, source, created_at, match_count, manifest, tags_locked, data)
               VALUES (?, ?, 'needs-names', ?, ?, ?, ?, 0, '{}')
               ON CONFLICT(id) DO UPDATE SET title=excluded.title, source=excluded.source,
                   match_count=excluded.match_count, manifest=excluded.manifest''',
            (video_id, title, play_source, now, count, str(manifest)))
        if args.label:
            db.execute('INSERT OR IGNORE INTO tag_links(tag, target, created_at) VALUES(?,?,?)',
                       (args.label, video_id, now))
        total += count
        print(f'{video_id}  {count:3d} matches  {title[:50]}')
    db.commit()
    db.close()
    print('total matches:', total)


if __name__ == '__main__':
    main()
