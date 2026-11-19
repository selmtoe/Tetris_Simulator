"""PPT Lab のデータを、閲覧専用のブラウザ版（静的サイト）用に書き出す。

手元の PPT Lab サーバーと同じ処理（名前の組・解析状態・開幕テンプレ・プレイヤー統計）で
作るので、ブラウザ版の表示は手元の画面と一致する。

出力: tools/ppt-lab/webdata/ （tools/build-web.py が dist/pages/ppt-lab/data/ にコピーする）
    videos.json              動画一覧（YouTube ID・プレイヤー・タグ）
    matches/<動画ID>.json     試合一覧（名前・勝敗・解析状態・開幕テンプレ）
    replays/<動画ID>.json     試合ごとの盤面ビューワー用ハッシュ
    players/index.json       プレイヤー名 → 統計ファイル
    players/<n>.json         プレイヤーの統計（タグ画面の戦績）
    tags.json / settings.json / settings-schema.json / env.json / tetofu.json（空）
見る人が保存するテト譜・タグは、その人のブラウザ（IndexedDB）に入る。ここには含めない。
"""
from __future__ import annotations

import hashlib
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
import server  # noqa: E402
from namereview import NameReview  # noqa: E402
from openings import OpeningCache  # noqa: E402
from playerstats import MoveTimes  # noqa: E402
from store import Store  # noqa: E402

OUT = ROOT / 'webdata'
# 載せない配信（private-streams.json の YouTube 動画ID）。それ以外のデスクトップ版の配信はすべて書き出す
PRIVATE_LIST = ROOT / 'private-streams.json'
# 閲覧専用版で見る人が変えられる設定（表示だけ）
PUBLIC_SETTING_GROUPS = ('表示',)
MATCH_FIELDS = ('index', 'start', 'end', 'p1', 'p2', 'winner', 'winnerStatus', 'failed',
                'analysisStatus', 'analysisReason', 'p1Opening', 'p2Opening')


def youtube_of(source: str) -> str:
    source = str(source or '')
    for marker in ('watch?v=', 'youtu.be/', '/live/', '/shorts/', '/embed/'):
        if marker in source:
            tail = source.split(marker)[-1]
            candidate = ''.join(ch for ch in tail[:11] if ch.isalnum() or ch in '-_')
            if len(candidate) == 11:
                return candidate
    return ''


def write(path: Path, payload) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, separators=(',', ':')), encoding='utf-8')


def setup() -> None:
    """サーバーと同じ部品を用意する（HTTP サーバーは立てない）。"""
    data = server.DEFAULT_DATA
    server.STORE = Store(data)
    server.PLAYER_DISK_FILE = data / 'cache' / 'players.json'
    server.MATCH_SUMMARY_FILE = data / 'cache' / 'match-summary.json'
    server.MOVE_TIMES = MoveTimes(data / 'cache' / 'move-times.json')
    server.DRIFT = server.DriftCache(data / 'cache' / 'board-drift.json')
    server.OPENINGS = OpeningCache(data / 'cache' / 'openings.json')
    server.AUTO_APPLY_NAMES[0] = bool(server.STORE.settings().get('autoApplyNames', True))
    server.REVIEW = NameReview(data / 'cache', server.find_ffmpeg(), server.STORE.recall_name,
                               server.STORE.recall_name_by_text,
                               server.make_visual_name_recall(server.CROP_DIR, server.STORE),
                               crop_dir=server.CROP_DIR)


def match_rows(video: dict) -> list[dict]:
    """手元の /api/videos/<id>/matches と同じ中身から、公開に要る項目だけを残す。"""
    store, review = server.STORE, server.REVIEW
    manifest = Path(video['manifest'])
    data = review.load(str(video['id']), manifest)
    overrides = store.name_overrides_for(str(video['id']))
    if overrides:
        for row in data.get('matches', []):
            for side in ('p1', 'p2'):
                fingerprint = row.get(side + 'Fingerprint')
                if fingerprint and fingerprint in overrides:
                    row[side + 'Guess'] = overrides[fingerprint]
    data = server.add_openings(server.mark_analysis_status(dict(data), manifest), manifest)
    rows = []
    for row in data.get('matches') or []:
        item = {key: row.get(key) for key in MATCH_FIELDS if row.get(key) not in (None, '')}
        # 確認済みの名前（名前確認の組の名前）
        item['p1'] = str(row.get('p1Guess') or row.get('p1') or '')
        item['p2'] = str(row.get('p2Guess') or row.get('p2') or '')
        rows.append(item)
    return rows


def replay_hashes(manifest: Path, rows: list[dict]) -> dict[str, str]:
    hashes = {}
    for row in rows:
        index = int(row['index'])
        replay = server.replay_file(manifest, index)
        if not replay or not replay.is_file():
            continue
        try:
            payload = json.loads(replay.read_text(encoding='utf-8-sig'))
        except (OSError, json.JSONDecodeError):
            continue
        url = str((payload.get('urls') or {}).get('combined') or '')
        if '#' in url:
            hashes[str(index)] = url.split('#', 1)[1]
    return hashes


def main() -> int:
    setup()
    store = server.STORE
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)

    try:
        private = set(json.loads(PRIVATE_LIST.read_text(encoding='utf-8')).get('youtubeIds') or [])
    except (OSError, json.JSONDecodeError):
        private = set()
    print(f'載せない配信: {len(private)} 本（{PRIVATE_LIST.name}）')
    videos = []
    players: set[str] = set()
    video_tags: dict[str, int] = {}
    total_matches = 0
    for video in store.videos():
        manifest_text = str(video.get('manifest') or '')
        youtube = youtube_of(video.get('source') or '')
        if youtube in private:
            continue                  # 載せない配信
        if not manifest_text or not Path(manifest_text).is_file() or not youtube:
            print('skip (解析結果か YouTube ID が無い):', video.get('title'))
            continue
        rows = match_rows(video)
        names = [name for name in server.video_players(manifest_text, str(video['id'])) if name]
        # 通常のタグ（黄色・緑）だけ。プレイヤー（青）は players / autoTags、配信者（赤）は channel から画面側で作る
        manual = [tag for tag in store.tags_for(str(video['id'])) if tag != '動画解析']
        players.update(names)
        for tag in manual:
            video_tags[tag] = video_tags.get(tag, 0) + 1
        write(OUT / 'matches' / f"{video['id']}.json", {'matches': rows, 'warning': ''})
        hashes = replay_hashes(Path(manifest_text), rows)
        write(OUT / 'replays' / f"{video['id']}.json", hashes)
        videos.append({
            'id': video['id'], 'title': video.get('title') or '', 'status': 'ready',
            'source': f'https://www.youtube.com/watch?v={youtube}', 'youtubeId': youtube,
            'createdAt': video.get('created_at') or video.get('createdAt') or '',
            'uploadDate': video.get('uploadDate') or '',
            'channel': video.get('channel') or '',
            'matchCount': len(rows), 'players': names, 'autoTags': names,
            'tags': manual, 'progress': 100, 'message': ''
        })
        total_matches += len(rows)
        print(f"video: {str(video.get('title'))[:30]:30s} matches={len(rows)} replays={len(hashes)}")

    # プレイヤーの統計（タグ画面の戦績パネル）。公開する配信だけで数える
    # （除外した配信の試合数・対戦相手が統計に混ざらないように）
    published = {video['id'] for video in videos}
    all_videos = store.videos
    store.videos = lambda: [video for video in all_videos() if video['id'] in published]
    index = {}
    for name in sorted(players):
        stem = hashlib.sha1(name.encode('utf-8')).hexdigest()[:12]
        # 期間ごと（全体・半年・30日・7日）に書き出す。期間は書き出した日を基準にする
        for period in server.PLAYER_PERIODS:
            write(OUT / 'players' / f'{stem}-{period}.json', server.player_page(name, period))
        index[name] = stem
    write(OUT / 'players' / 'index.json', index)

    kinds = {tag['name']: tag.get('kind') or 'free' for tag in store.tags()}
    write(OUT / 'tags.json', {'tags': [{'name': name, 'count': count, 'kind': kinds.get(name, 'free')}
                                       for name, count in sorted(video_tags.items())]})
    schema = [item for item in server.SETTINGS_SCHEMA if item.get('group') in PUBLIC_SETTING_GROUPS]
    write(OUT / 'settings-schema.json', {'schema': schema})
    write(OUT / 'settings.json', {'settings': {item['key']: item['default'] for item in schema}})
    write(OUT / 'env.json', {'engine': False, 'ffmpeg': False, 'gpu': False, 'python': 'static'})
    write(OUT / 'tetofu.json', {'tetofu': []})
    size = sum(path.stat().st_size for path in OUT.rglob('*') if path.is_file())
    print(f'done: videos={len(videos)} matches={total_matches} players={len(index)} '
          f'size={size / 1024 / 1024:.1f}MB -> {OUT}')
    write(OUT / 'videos.json', {'videos': videos})
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
