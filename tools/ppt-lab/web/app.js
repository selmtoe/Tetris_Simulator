/* PPT Lab の画面。見た目は Lab（base.css / lab-enhancements.css）の
   クラスだけで組み立て、独自の色や角丸は増やさない。 */
(function () {
    'use strict';

    const mainView = document.getElementById('main-view');
    const statusLine = document.getElementById('lab-status');
    const dialog = document.getElementById('entry-dialog');
    const dialogTitle = document.getElementById('entry-dialog-title');
    const dialogNote = document.getElementById('entry-dialog-note');
    const entryTitle = document.getElementById('entry-title');
    const entryUrl = document.getElementById('entry-url');
    const entrySubmit = document.getElementById('entry-submit');
    const entrySaveField = document.getElementById('entry-save-field');
    const entrySaveVideo = document.getElementById('entry-save-video');

    const state = {
        view: 'videos',
        videos: [],
        tetofu: [],
        tags: [],
        query: '',
        pollTimer: null,
        lastVideosJson: '',
        entryMode: 'youtube',
        noticeTimer: null,
        mode: 'videos',
        page: 0,
        filters: { query: '', player: '', opponent: '', tag: '', result: '', sort: 'newest' },
        // 動画詳細（試合のレール）の絞り込み。動画一覧の filters とは別に持つ
        matchFilters: { query: '', player: '', opponent: '' },
        matchCache: new Map(),
        matchLoading: false,
        matchFailed: new Set(),
        pendingMatch: 0,
        pendingSeek: null,
        featureList: [],
        feature: { segments: [], index: -1 },
        featureIndex: -1,
        featurePending: null,
        lastFeatureSeekAt: 0,
        searchTimer: null,
        tetofuQuery: '',
        tagQuery: '',
        tagSelected: new Set(),
        tetofuTag: '',
        pages: [],
        pageIndex: -1,
        currentUrls: null,
        syncTimer: null,
        bridge: null,
        // ビューワーを postMessage 連携で動かしているか（false のときだけ URL を開き直す）
        viewerFallback: false,
        // postMessage の準備を待つ期限（この間は読み込み直さずに待つ）
        viewerReadyBy: 0,
        lastSeekAt: 0,
        preferred: null,
        pendingSeek: null,
        pendingSeekAt: 0,
        pendingSeekFrom: NaN,
        pendingSeekPaused: false,
        syncing: false,
        follow: true,
        ytPlayer: null,
        ytReady: false,
        videoId: '',
        videoTitle: '',
        tetofuId: '',
        matches: [],
        // サーバーが名前画像の指紋でまとめた「1組 = 1エントリ」の一覧
        matchGroups: [],
        replays: new Map(),
        edits: new Map(),
        editsVideoId: '',
        nameGroups: new Map(),
        settings: {},
        settingsTab: '一般',
        settingsSchema: [],
        // インストール済みの解析モデル（/api/models の結果）
        models: [],
        modelsActive: '',
        modelsError: '',
        groups: new Set()
    };

    const STATUS_LABEL = {
        queued: '待機中',
        analyzing: '解析中',
        'needs-names': '解析済み',
        ready: '登録済み',
        failed: '失敗'
    };

    // ---- 小さな道具 -------------------------------------------------------
    function node(tag, text, className) {
        const element = document.createElement(tag);
        if (text !== undefined && text !== null) element.textContent = String(text);
        if (className) element.className = className;
        return element;
    }

    // 全角/半角・大文字小文字・前後の空白を無視して比べる（Lab の normalize と同じ）
    function normalizeText(value) {
        return String(value === undefined || value === null ? '' : value)
            .normalize('NFKC')
            .trim()
            .replace(/\s+/g, ' ')
            .toLocaleLowerCase('ja');
    }

    // 画面遷移で破棄されても詳細を開き直せるようにする
    function ensureViewerFrame() {
        let frame = document.getElementById('detail-viewer');
        if (frame) return frame;
        frame = node('iframe', null, 'lab-replay-viewer');
        frame.id = 'detail-viewer';
        frame.title = '盤面ビューワー';
        frame.dataset.status = 'loading';
        document.body.append(frame);
        return frame;
    }

    function formatDate(value) {
        if (!value) return '';
        const date = new Date(value);
        if (Number.isNaN(date.getTime())) return String(value).slice(0, 10);
        const year = date.getFullYear();
        const month = String(date.getMonth() + 1).padStart(2, '0');
        const day = String(date.getDate()).padStart(2, '0');
        return year + '-' + month + '-' + day;
    }

    // CC BY の動画の出典表示（YouTube 動画ID → 作者・元のタイトル）。
    // 動画ページの一番下に1行で出す。ライセンスは YouTube の「クリエイティブ・コモンズ
    // 著作権表示必須ライセンス」= CC BY 4.0。
    const CC_BY_CREDITS = {
        'FMhM5akSQWk': { author: 'たいようあめみや', title: '【VS宇宙最強テトラー】しゃけごはん！レート連戦【ぷよぷよテトリス2】【puyopuyotetris2】' },
        'J-upNUb6kwE': { author: 'たいようあめみや', title: '【VS最上位テトラー】いんたい！テトリス30先【ぷよぷよテトリス2】【puyopuyotetris2】' },
        'UEKoqg7LZkM': { author: 'たいようあめみや', title: '【VS最強格テトラー】ゆうくん！テトリス15先2回【ぷよぷよテトリス2】【puyopuyotetris2】' },
        'YUMAUsgN66s': { author: 'たいようあめみや', title: '【VS最強格テトラー】ふれあ！テトリス15先×2回【ぷよぷよテトリス2】【puyopuyotetris2】' },
    };

    function creditLine(video) {
        const id = youtubeId(video && video.source);
        const credit = CC_BY_CREDITS[id];
        if (!credit) return null;
        const line = node('p', null, 'lab-credit');
        const link = (text, href) => {
            const a = node('a', text);
            a.href = href;
            a.target = '_blank';
            a.rel = 'noopener';
            return a;
        };
        line.append('映像：' + credit.author + '「', link(credit.title, 'https://www.youtube.com/watch?v=' + id),
            '」（YouTube）／', link('CC BY 4.0', 'https://creativecommons.org/licenses/by/4.0/deed.ja'));
        return line;
    }

    function youtubeId(url) {
        const match = String(url || '').match(
            /(?:youtube\.com\/(?:watch\?v=|shorts\/|live\/)|youtu\.be\/)([\w-]{11})/);
        return match ? match[1] : '';
    }

    function thumbnailFor(video) {
        const id = youtubeId(video.source);
        if (id) return 'https://i.ytimg.com/vi/' + id + '/mqdefault.jpg';
        // ローカル動画は Lab の既定サムネイルと同じ、灰色の板にする。
        return 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(
            '<svg xmlns="http://www.w3.org/2000/svg" width="160" height="90">' +
            '<rect width="160" height="90" fill="#e8ecea"/>' +
            '<rect x="62" y="33" width="36" height="24" rx="4" fill="#b9c6c1"/>' +
            '<path d="M74 40l12 7-12 7z" fill="#f9f9f9"/></svg>');
    }

    function showStatus(text) {
        if (statusLine) statusLine.textContent = text || '';
    }

    // 画面の右から出る通知
    function notify(text, kind) {
        let stack = document.getElementById('ppt-toasts');
        if (!stack) {
            stack = node('div', null, 'ppt-toasts');
            stack.id = 'ppt-toasts';
            document.body.append(stack);
        }
        const item = node('div', text, 'ppt-toast' + (kind ? ' ppt-toast-' + kind : ''));
        stack.append(item);
        while (stack.children.length > 4) stack.firstElementChild.remove();
        requestAnimationFrame(() => item.classList.add('in'));
        setTimeout(() => {
            item.classList.remove('in');
            setTimeout(() => item.remove(), 260);
        }, 3600);
    }

    function toast(text) {
        notify(text);
        showStatus(text);
        clearTimeout(state.noticeTimer);
        state.noticeTimer = setTimeout(() => showStatus(''), 6000);
    }

    async function api(path, options) {
        const response = await fetch(path, options);
        let body = null;
        try {
            body = await response.json();
        } catch (error) {
            body = null;
        }
        if (!response.ok) {
            throw new Error((body && body.error) || '通信に失敗しました');
        }
        return body || {};
    }

    // ---- 検索の条件式 -----------------------------------------------------
    // 空白は AND、and / or / && / || / ( ) が使える。語はタグ名かタイトルに当たる。
    function tokenizeQuery(text) {
        const tokens = [];
        const pattern = /&&|\|\||\(|\)|\b(and|or)\b|[^\s()&|]+/gi;
        let match;
        while ((match = pattern.exec(text))) {
            const raw = match[0];
            const lower = raw.toLowerCase();
            if (raw === '(' || raw === ')') tokens.push(raw);
            else if (lower === 'and' || raw === '&&') tokens.push('AND');
            else if (lower === 'or' || raw === '||') tokens.push('OR');
            else tokens.push({ term: raw.toLowerCase() });
        }
        return tokens;
    }

    function parseQuery(text) {
        const tokens = tokenizeQuery(text || '');
        if (!tokens.length) return null;
        let index = 0;
        const peek = () => tokens[index];
        const parseAtom = () => {
            const token = peek();
            if (token === '(') {
                index += 1;
                const inner = parseOr();
                if (peek() === ')') index += 1;
                return inner;
            }
            if (token && token.term) {
                index += 1;
                return { term: token.term };
            }
            index += 1;
            return null;
        };
        const parseAnd = () => {
            let left = parseAtom();
            while (true) {
                const token = peek();
                if (token === 'AND') {
                    index += 1;
                    left = { op: 'and', left, right: parseAtom() };
                    continue;
                }
                if (token && token !== ')' && token !== 'OR') {
                    left = { op: 'and', left, right: parseAtom() };
                    continue;
                }
                break;
            }
            return left;
        };
        const parseOr = () => {
            let left = parseAnd();
            while (peek() === 'OR') {
                index += 1;
                left = { op: 'or', left, right: parseAnd() };
            }
            return left;
        };
        const ast = parseOr();
        if (!ast && tokens.length) return { invalid: true };
        return ast;
    }

    function matchQuery(ast, tags, text) {
        if (ast && ast.invalid) return false;
        if (!ast) return true;
        if (ast.term) {
            const term = ast.term;
            if ((tags || []).some(tag => String(tag).toLowerCase().includes(term))) return true;
            return String(text || '').toLowerCase().includes(term);
        }
        if (ast.op === 'and') return matchQuery(ast.left, tags, text) && matchQuery(ast.right, tags, text);
        if (ast.op === 'or') return matchQuery(ast.left, tags, text) || matchQuery(ast.right, tags, text);
        return true;
    }

    function selectedTags() {
        return Array.from(state.tagSelected || []).map(normalizeTagKey);
    }

    function hasAllTags(tags, wanted) {
        if (!wanted.length) return true;
        const list = (tags || []).map(tag => String(tag));
        return wanted.every(tag => list.includes(tag));
    }

    // ---- カード -----------------------------------------------------------
    function cardThumbnail(video) {
        const image = node('img', null, 'card-thumbnail card-video-thumb');
        image.src = thumbnailFor(video);
        image.width = 160;
        image.height = 90;
        image.alt = '';
        image.loading = 'lazy';
        image.onerror = function () {
            image.onerror = null;
            image.src = thumbnailFor({ source: '' });
        };
        return image;
    }

    // タグはリンクとして動かす（押すとタグ画面へ移動して絞り込む）
    // タグの種類: 配信者（YouTube のチャンネル名・赤）／通常（自分で付けるタグ・黄色）／
    // 開幕テンプレ・途中テンプレ（緑）／プレイヤー（動画の登場プレイヤー・青）
    // 配信者とプレイヤーは自動で付き、同じ名前の通常タグとは別物として扱う。
    // そのため中では「streamer:名前」「player:名前」というキーで区別する（通常タグは名前のまま）。
    const TAG_KIND_LABELS = { streamer: '配信者', free: 'タグ', opening: '開幕テンプレ', midgame: '途中テンプレ', player: 'プレイヤー' };
    const TAG_KIND_ORDER = ['streamer', 'free', 'opening', 'midgame', 'player'];
    const AUTO_TAG_KINDS = ['streamer', 'player'];
    function playerNameSet() {
        return new Set((state.videos || []).flatMap(video => video.players || []));
    }
    function channelNameSet() {
        return new Set((state.videos || []).map(video => video.channel).filter(Boolean));
    }
    function autoTagKey(kind, name) {
        return kind + ':' + name;
    }
    function tagName(key) {
        const text = String(key);
        const kind = AUTO_TAG_KINDS.find(item => text.startsWith(item + ':'));
        return kind ? text.slice(kind.length + 1) : text;
    }
    function tagKind(key) {
        const text = String(key);
        const auto = AUTO_TAG_KINDS.find(item => text.startsWith(item + ':'));
        if (auto) return auto;
        const tag = (state.tags || []).find(item => item.name === text);
        return (tag && tag.kind) || 'free';
    }
    // 動画のタグ（表示順: 配信者 → 通常 → プレイヤー）
    function videoTagKeys(video) {
        if (!video) return [];
        return [
            ...(video.channel ? [autoTagKey('streamer', video.channel)] : []),
            ...(video.tags || []),
            ...(video.autoTags || []).map(name => autoTagKey('player', name))
        ];
    }
    // 検索用（種類は問わず名前だけ）
    function videoTagNames(video) {
        return videoTagKeys(video).map(tagName);
    }
    // 古いリンク（#tags/名前）でプレイヤー名・配信者名を指していたものを新しいキーに直す
    function normalizeTagKey(key) {
        const text = String(key);
        if (AUTO_TAG_KINDS.some(kind => text.startsWith(kind + ':'))) return text;
        if ((state.tags || []).some(tag => tag.name === text)) return text;
        // 以前のリンクはプレイヤー名のものなので、プレイヤーを先に見る
        if (playerNameSet().has(text)) return autoTagKey('player', text);
        if (channelNameSet().has(text)) return autoTagKey('streamer', text);
        return text;
    }

    function tagLink(key) {
        const link = node('a', tagName(key), 'tag tag-kind-' + tagKind(key));
        link.href = '#tags/' + encodeURIComponent(key);
        link.addEventListener('click', event => {
            event.stopPropagation();
            event.preventDefault();
            location.hash = '#tags/' + encodeURIComponent(key);
        });
        return link;
    }

    function crownMark(review) {
        const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
        svg.setAttribute('viewBox', '0 0 24 24');
        svg.setAttribute('class', 'lab-crown' + (review ? ' lab-crown-review' : ''));
        svg.setAttribute('role', 'img');
        svg.setAttribute('aria-label', '勝者');
        const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
        path.setAttribute('d', 'M2.5 19.5v-12l5 3.6L12 4.6l4.5 6.5 5-3.6v12z');
        svg.append(path);
        return svg;
    }

    // 画面に出す名前。名前確認で覚えた名前（p1Guess）があればそれ、無ければ読み取った名前。
    // 読み取りの誤り（例: uyeshota → uveshota）で同じ人が2人に分かれないようにする。
    // 名前を直す画面は読み取った名前（row.p1）との差で動くので、そちらはこれを使わない。
    function shownName(row, side) {
        return String((row && (row[side + 'Guess'] || row[side])) || '');
    }

    function playerNames(match) {
        const wrap = node('span', null, 'lab-match-players');
        const names = [shownName(match, 'p1'), shownName(match, 'p2')].filter(Boolean);
        names.forEach((name, index) => {
            if (index) wrap.append(document.createTextNode(' / '));
            if (match.winner === (index === 0 ? 'p1' : 'p2')) {
                wrap.append(crownMark(match.winnerStatus !== 'ok'));
            }
            wrap.append(document.createTextNode(name));
        });
        if (match.winner === 'draw') {
            wrap.append(node('span', '引き分け', 'lab-draw-badge'));
        }
        return wrap;
    }
    const DEFAULT_THUMB = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(
        '<svg xmlns="http://www.w3.org/2000/svg" width="640" height="360" viewBox="0 0 640 360">' +
        '<rect width="640" height="360" fill="white"/>' +
        '<text x="320" y="198" text-anchor="middle" font-family="Arial,sans-serif" font-size="72" ' +
        'font-weight="700" letter-spacing="9" fill="#79877f">LAB</text></svg>');

    function labThumbnail(video) {
        const id = youtubeId(video.source);
        if (id) return 'https://i.ytimg.com/vi/' + id + '/mqdefault.jpg';
        // ローカル動画は、サーバーが動画から切り出した1コマを使う（無ければ既定の画像）
        if (video.source && video.id) return '/api/videos/' + encodeURIComponent(video.id) + '/thumb';
        return DEFAULT_THUMB;
    }

    // 動画もテト譜と同じ「正方形タイル」で並べる
    // タグ画面でプレイヤーのタグを1つ選んでいるなら、その名前（動画を開いたときの1つ目のプレイヤーに入れる）
    function selectedPlayerForVideo() {
        if (state.view !== 'tags') return '';
        const wanted = selectedTags();
        return wanted.length === 1 && tagKind(wanted[0]) === 'player' ? wanted[0] : '';
    }

    function renderLabCard(container, video, matchCount) {
        const card = node('article', null, 'card ppt-tile ppt-video-tile');
        card.dataset.videoId = video.id;
        const thumb = node('div', null, 'ppt-tile-preview');
        const image = node('img', null, 'ppt-tile-thumb');
        image.src = labThumbnail(video);
        image.alt = '';
        image.loading = 'lazy';
        image.decoding = 'async';
        image.onerror = () => { image.onerror = null; image.src = DEFAULT_THUMB; };
        thumb.append(image);
        if (video.status === 'queued') {
            thumb.append(node('span', '解析待ち', 'ppt-tile-badge'));
        } else if (video.status === 'analyzing') {
            thumb.append(node('span', '解析中 ' + Math.round(video.progress || 0) + '%', 'ppt-tile-badge'));
        } else if (video.status === 'failed') {
            // 押すと同じ入力でもう一度解析する。理由はマウスを乗せると出る
            const badge = node('button', '解析失敗・再解析', 'ppt-tile-badge ppt-tile-badge-error');
            badge.type = 'button';
            badge.title = (video.error || video.message || '解析に失敗しました') + '\n押すともう一度解析します';
            badge.addEventListener('click', event => {
                event.preventDefault();
                event.stopPropagation();
                retryVideo(video);
            });
            thumb.append(badge);
        } else if (video.status === 'needs-names') {
            // 名前の確認がまだ。押すと名前の確認画面へ直接行く
            const badge = node('button', '名前未確認', 'ppt-tile-badge ppt-tile-badge-names');
            badge.type = 'button';
            badge.title = '名前の確認画面を開く';
            badge.addEventListener('click', event => {
                event.preventDefault();
                event.stopPropagation();
                if (video.id) location.hash = '#names/' + video.id;
            });
            thumb.append(badge);
        }
        card.append(thumb);

        const title = node('h3', null, 'card-title');
        const link = node('a', video.title, 'card-title-action');
        link.href = '#video/' + video.id;
        // ブラウザ既定の遷移に任せず、このカードのIDで明示的に開く
        link.addEventListener('click', event => {
            event.preventDefault();
            event.stopPropagation();
            if (!video.id) return;
            state.pendingMatch = 0;
            state.pendingPlayerFilter = selectedPlayerForVideo();
            location.hash = '#video/' + video.id;
        });
        title.append(link);
        card.append(title);

        const players = (video.players || []).slice(0, 3).join(' / ');
        card.append(node('p', players || 'プレイヤー未登録', 'ppt-tile-players'));

        const tagRow = node('div', null, 'lab-video-tags');
        const allTags = videoTagKeys(video);
        allTags.slice(0, 8).forEach(tag => tagRow.append(tagLink(tag)));
        if (allTags.length > 8) tagRow.append(node('span', '+' + (allTags.length - 8), 'ppt-tile-more'));
        card.append(tagRow);

        const shown = Number.isFinite(matchCount) ? matchCount : (video.matchCount || 0);
        const note = video.status === 'failed'
            ? '解析失敗' + (video.error ? '：' + String(video.error).slice(0, 80) : '')
            : video.status === 'queued' ? '解析待ち'
            : shown + '試合';
        const noteNode = node('div', note, 'lab-analysis-count');
        if (video.status === 'failed' && video.error) noteNode.title = String(video.error);
        card.append(noteNode);

        card.addEventListener('click', event => {
            if (event.target.closest('a,button,input,select,textarea')) return;
            if (window.getSelection()?.toString()) return;
            const videoId = card.dataset.videoId;
            state.pendingPlayerFilter = selectedPlayerForVideo();
            if (videoId) location.hash = '#video/' + videoId;
        });
        container.append(card);
    }
    function filterVideos(videos) {
        const f = state.filters;
        const ast = parseQuery(f.query);
        const wanted = state.view === 'tags' ? selectedTags() : [];
        return videos.filter(video => {
            const players = video.players || [];
            const tags = video.tags || [];
            const names = videoTagNames(video);
            const text = [video.title, ...names, ...players].join(' ');
            if (!matchQuery(ast, names, text)) return false;
            if (!hasAllTags(videoTagKeys(video), wanted)) return false;
            if (f.player && !players.includes(f.player)) return false;
            if (f.opponent && !players.includes(f.opponent)) return false;
            // 名前が動画に居るだけでは足りない。その2人が同じ試合で対戦している動画だけ残す。
            if ((f.player || f.opponent) && !videoHasPairMatch(video)) return false;
            if (f.tag && !tags.includes(f.tag)) return false;
            return true;
        });
    }

    // 読み込み前は残しておき、読み込んだら試合単位で判定し直す
    function videoHasPairMatch(video) {
        const cached = state.matchCache.get(video.id);
        if (!cached) return true;
        return cached.some(row => pairOk(row));
    }

    // 動画カードに出す試合数。プレイヤーで絞っているときは条件に合う試合だけ数える
    function videoMatchCount(video, pairWanted) {
        if (!pairWanted) return video.matchCount || 0;
        const cached = state.matchCache.get(video.id);
        if (!cached) return video.matchCount || 0;   // 読み込み前は暫定値
        return cached.filter(row => matchFilterOk(row)).length;
    }

    // 設定「既定の並び順」（new / old / matches / title）を一覧の値に直す
    function defaultSort() {
        const value = String(setting('sortOrder') || 'new');
        return { new: 'newest', old: 'oldest', matches: 'matches', title: 'title',
                 newest: 'newest', oldest: 'oldest' }[value] || 'newest';
    }

    function sortEntries(entries) {
        const mode = state.filters.sort;
        return entries.slice().sort((a, b) => {
            if (mode === 'title') return String(a.title).localeCompare(String(b.title), 'ja');
            if (mode === 'oldest') return videoDate(a).localeCompare(videoDate(b));
            if (mode === 'matches') {
                const diff = (b.matchCount || 0) - (a.matchCount || 0);
                if (diff) return diff;
            }
            return videoDate(b).localeCompare(videoDate(a));
        });
    }

    // 動画の日付: YouTube に上がった日（取れていなければ PPT Lab に追加した日）
    function videoDate(video) {
        return String((video && (video.uploadDate || video.createdAt)) || '');
    }

    function circled(n) {
        const table = '①②③④⑤⑥⑦⑧⑨⑩⑪⑫⑬⑭⑮⑯⑰⑱⑲⑳';
        return n >= 1 && n <= 20 ? table[n - 1] : String(n);
    }

    // 「プレイヤー1」「プレイヤー2」は、その2人が同じ試合に居るときだけ通す。
    // P1/P2 の左右は問わない（同じ名前を両方に選んだときだけ両側一致を要求）。
    function pairOk(row) {
        const one = state.filters.player;
        const two = state.filters.opponent;
        if (!one && !two) return true;
        if (one && two && one === two) return shownName(row, 'p1') === one && shownName(row, 'p2') === two;
        const names = [shownName(row, 'p1'), shownName(row, 'p2')].filter(Boolean);
        if (one && !names.includes(one)) return false;
        if (two && !names.includes(two)) return false;
        return true;
    }

    // Lab と同じ勝敗判定: プレイヤー未指定なら素通し、未確定は winner 無し or status !== ok
    function resultOk(row) {
        const want = state.filters.result;
        const player = state.filters.player;
        if (!want) return true;
        if (want === 'draw') return row.winner === 'draw';
        if (!player) return true;
        const names = [shownName(row, 'p1'), shownName(row, 'p2')].filter(Boolean);
        const side = names[0] === player ? 'p1' : names[1] === player ? 'p2' : '';
        if (!side) return false;
        if (want === 'review') return !row.winner || row.winnerStatus !== 'ok';
        if (row.winner === 'draw') return false;
        const won = row.winner === side;
        return want === 'win' ? won : !won;
    }

    // 試合一覧と連続再生で同じ絞り込みを使う（件数がずれないように）
    function matchFilterOk(row) {
        return pairOk(row) && resultOk(row);
    }

    // ---- 動画詳細（試合のレール）の絞り込み --------------------------------
    // Lab の動画詳細と同じで、プレイヤーを2つ選んだときは
    // 「その2人が同じ試合で対戦している」ものだけを通す（片方だけでもよい）。
    function matchPlayerOk(row) {
        const one = normalizeText(state.matchFilters.player);
        const two = normalizeText(state.matchFilters.opponent);
        if (!one && !two) return true;
        const names = [shownName(row, 'p1'), shownName(row, 'p2')].map(normalizeText).filter(Boolean);
        if (one && two) {
            return names.some((name, index) => name === one
                && names.some((other, otherIndex) => otherIndex !== index && other === two));
        }
        return (!one || names.includes(one)) && (!two || names.includes(two));
    }

    // 検索語は空白で区切ると AND（部分一致・大文字小文字は無視）
    function matchTextOk(row) {
        const words = normalizeText(state.matchFilters.query).split(' ').filter(Boolean);
        if (!words.length) return true;
        const text = normalizeText([shownName(row, 'p1'), shownName(row, 'p2')].filter(Boolean).join(' '));
        return words.every(word => text.includes(word));
    }

    // 試合カード・前後の移動・件数で同じ絞り込みを使う（件数がずれないように）
    function detailMatchFilterOk(row) {
        return matchPlayerOk(row) && matchTextOk(row);
    }

    function detailMatches(matches) {
        return (matches || []).filter(row => detailMatchFilterOk(row));
    }

    // その動画の試合に出てくるプレイヤー（表記ゆれは1つにまとめて名前順）
    function matchPlayerChoices(matches, video) {
        const labels = new Map();
        const add = value => {
            const label = String(value === undefined || value === null ? '' : value).trim();
            const key = normalizeText(label);
            if (!key || key.includes('?')) return;
            if (!labels.has(key)) labels.set(key, label);
        };
        (matches || []).forEach(row => { add(shownName(row, 'p1')); add(shownName(row, 'p2')); });
        ((video && video.players) || []).forEach(add);
        return [...labels.values()].sort((a, b) => a.localeCompare(b, 'ja'));
    }

    function renderMatchCard(container, video, row, order) {
        const card = node('article', null, 'card lab-video-match-card');
        const meta = node('div', null, 'card-meta');
        const badge = node('span', '動画' + circled(order), 'lab-source-badge');
        badge.title = '出典: ' + video.title;
        meta.append(badge, node('span', video.title), node('span', formatDate(videoDate(video))));
        card.append(meta);

        const title = node('h3', null, 'card-title');
        const link = node('a', null, 'card-title-action');
        link.href = '#video/' + video.id;
        link.append(document.createTextNode('試合 ' + row.index + ' · '));
        const names = [shownName(row, 'p1'), shownName(row, 'p2')].filter(Boolean);
        if (!names.length) link.append(document.createTextNode('プレイヤー未登録'));
        names.forEach((name, index) => {
            if (index) link.append(document.createTextNode(' / '));
            if (row.winner === (index === 0 ? 'p1' : 'p2')) {
                link.append(crownMark(row.winnerStatus !== 'ok'));
            }
            link.append(document.createTextNode(name));
        });
        if (row.winner === 'draw') {
            link.append(node('span', '引き分け', 'lab-draw-badge'));
        }
        link.addEventListener('click', event => {
            event.preventDefault();
            event.stopPropagation();
            location.hash = '#video/' + video.id;
            state.pendingMatch = row.index;
        });
        title.append(link);
        card.append(title);

        const time = node('p', null, 'lab-video-match-time');
        time.textContent = timeText(row.start) + '〜' + timeText(row.end);
        card.append(time);

        const tagRow = node('div', null, 'lab-video-match-tags');
        videoTagKeys(video).filter(key => tagKind(key) !== 'player')
            .slice(0, 6)
            .forEach(tag => tagRow.append(tagLink(tag)));
        if (tagRow.childElementCount) card.append(tagRow);

        card.addEventListener('click', event => {
            if (event.target.closest('a,button')) return;
            if (window.getSelection()?.toString()) return;
            location.hash = '#video/' + video.id;
            state.pendingMatch = row.index;
        });
        container.append(card);
    }

    function renderVideos() {
        const root = node('section', null, 'lab-video-library');
        mainView.replaceChildren(root);

        const header = node('div', null, 'lab-video-library-heading');
        // 動画の追加は Lab の「動画の管理」と同じ位置（見出しの右）に置く
        const addBar = node('div', null, 'lab-inline-actions ppt-server-only');
        const youtube = node('button', 'YouTubeから追加', 'btn-outline');
        youtube.type = 'button';
        youtube.addEventListener('click', () => openEntry('youtube'));
        const local = node('button', 'ファイルから追加', 'btn-outline');
        local.type = 'button';
        local.addEventListener('click', () => addFromFile(local));
        addBar.append(youtube, local);
        header.append(addBar);
        root.append(header);

        const tabs = node('div', null, 'lab-video-tabs');
        const modes = [['videos', '動画一覧'], ['matches', '試合一覧']].map(([value, label]) => {
            const control = node('button', label, 'btn-outline');
            control.type = 'button';
            control.dataset.view = value;
            control.addEventListener('click', () => {
                state.mode = value;
                state.page = 0;
                location.hash = value === 'matches' ? '#videos/matches' : '#videos';
                renderVideos();
            });
            tabs.append(control);
            return control;
        });
        root.append(tabs);

        const players = [...new Set(state.videos.flatMap(video => video.players || []))]
            .filter(name => name && name.length >= 2 && !name.includes('?'))
            .sort();
        const tagChoices = [...new Set(state.videos.flatMap(video => video.tags || []))]
            .filter(tag => tag !== '動画解析').sort();

        if (!state.sortChosen) state.filters.sort = defaultSort();
        const controls = node('div', null, 'lab-video-filters');
        const bind = (label, key, values, type) => {
            const wrap = node('label', label);
            const input = type === 'search' ? node('input') : node('select');
            if (type === 'search') {
                input.type = 'search';
                input.placeholder = '動画名・プレイヤー・タグ（and / or / () 可）';
                input.value = state.filters[key];
            } else {
                values.forEach(([value, text]) => {
                    const option = node('option', text);
                    option.value = value;
                    input.append(option);
                });
                input.value = state.filters[key];
            }
            input.setAttribute('aria-label', label);
            input.addEventListener(type === 'search' ? 'input' : 'change', () => {
                if (type === 'search') {
                    // 1文字ごとに作り直すとフォーカスが飛ぶので、少し待ってから描き直す
                    const value = input.value;
                    clearTimeout(state.searchTimer);
                    state.searchTimer = setTimeout(() => {
                        state.filters[key] = value;
                        state.page = 0;
                        renderVideos();
                        const next = document.querySelector('.lab-video-filters input[type=search]');
                        if (next) {
                            next.focus();
                            next.setSelectionRange(next.value.length, next.value.length);
                        }
                    }, 260);
                    return;
                }
                state.filters[key] = input.value;
                if (key === 'sort') state.sortChosen = true;
                state.page = 0;
                renderVideos();
            });
            wrap.append(input);
            controls.append(wrap);
            return input;
        };
        bind('検索', 'query', null, 'search');
        bind('プレイヤー1', 'player', [['', 'すべて'], ...players.map(name => [name, name])]);
        bind('プレイヤー2', 'opponent', [['', 'すべて'], ...players.map(name => [name, name])]);
        bind('タグ', 'tag', [['', 'すべて'], ...tagChoices.map(name => [name, name])]);
        bind('勝敗', 'result', [['', 'すべて'], ['win', '勝ち'], ['lost', '負け'], ['draw', '引き分け'], ['review', '未確定']]);
        const sortOptions = [['newest', '新しい順'], ['oldest', '古い順'], ['title', 'タイトル順'], ['matches', '試合数順']];
        bind('並び順', 'sort', sortOptions);

        const clear = node('button', '絞り込みを解除', 'btn-outline');
        clear.type = 'button';
        clear.addEventListener('click', () => {
            state.filters = { query: '', player: '', opponent: '', tag: '', result: '', sort: defaultSort() };
            state.page = 0;
            renderVideos();
        });
        controls.append(clear);

        const feature = node('button', '連続再生', 'btn-outline');
        feature.type = 'button';
        feature.title = '絞り込み結果を先頭から続けて再生します';
        feature.addEventListener('click', () => playSequentially());
        controls.append(feature);
        root.append(controls);

        const entries = sortEntries(filterVideos(state.videos));
        // プレイヤーで絞るときは、動画一覧でも試合データが要る
        const pairWanted = Boolean(state.filters.player || state.filters.opponent);

        // 試合一覧は全動画を横断してフラットに並べる
        let rows;
        if (state.mode === 'matches') {
            rows = [];
            // 試合一覧の検索は試合ごとに判定する（動画に名前が居るだけの試合は出さない）
            const rowAst = parseQuery(state.filters.query);
            entries.forEach(video => {
                const cached = state.matchCache.get(video.id);
                if (!cached) return;
                const videoTags = videoTagKeys(video).filter(key => tagKind(key) !== 'player').map(tagName);
                cached.forEach(row => {
                    if (!matchFilterOk(row)) return;
                    if (rowAst) {
                        const rowTags = [...videoTags, shownName(row, 'p1'), shownName(row, 'p2')].filter(Boolean);
                        if (!matchQuery(rowAst, rowTags, [video.title, ...rowTags].join(' '))) return;
                    }
                    rows.push({ video, row });
                });
            });
        } else {
            rows = entries.map(video => ({ video }));
        }

        const matchTotal = state.mode === 'matches'
            ? rows.length
            : entries.reduce((sum, video) => sum + videoMatchCount(video, pairWanted), 0);
        root.append(node('p', entries.length + '動画 · ' + matchTotal + '試合', 'lab-video-count'));

        const list = node('div', null, 'ppt-tile-grid');
        root.append(list);

        const pageSize = Math.max(5, Number(setting('pageSize')) || 40);
        const pageCount = Math.max(1, Math.ceil(rows.length / pageSize));
        state.page = Math.min(state.page, pageCount - 1);
        const visible = rows.slice(state.page * pageSize, (state.page + 1) * pageSize);

        // 試合一覧・プレイヤー絞り込みのどちらでも、足りない試合データを読む
        const need = (state.mode === 'matches' || pairWanted)
            ? entries.filter(video => video.matchCount && !state.matchCache.has(video.id))
            : [];
        if (need.length && !state.matchLoading) {
            state.matchLoading = true;
            list.append(node('p', state.mode === 'matches'
                ? '試合一覧を読み込んでいます…'
                : 'プレイヤーで絞り込んでいます…', 'lab-help-text'));
            (async () => {
                for (const video of need) {
                    try {
                        const body = await api('/api/videos/' + video.id + '/matches?light=1');
                        state.matchCache.set(video.id, body.matches || []);
                    } catch (error) {
                        state.matchFailed = state.matchFailed || new Set();
                        state.matchFailed.add(video.id);
                        state.matchCache.set(video.id, []);
                    }
                }
                state.matchLoading = false;
                if (state.view === 'videos') renderVideos();
            })();
        }

        if (state.mode === 'matches') {
            if (!rows.length && !state.matchLoading && !need.length) {
                list.append(node('p', entries.length
                    ? '条件に合う試合はありません。'
                    : '動画はまだありません。', 'lab-help-text'));
            }
            const order = new Map();
            visible.forEach(({ video }) => {
                if (!order.has(video.id)) order.set(video.id, order.size + 1);
            });
            visible.forEach(({ video, row }) => renderMatchCard(list, video, row, order.get(video.id)));
        } else {
            if (!entries.length && !state.matchLoading) {
                list.append(node('p', state.videos.length
                    ? '条件に合う動画はありません。'
                    : (document.documentElement.dataset.localSaves === 'true'
                        ? 'まだ公開している配信はありません。配信者の許可をいただいた配信から順に追加していきます。'
                        : 'まだ動画がありません。上のボタンから追加してください。'), 'lab-help-text'));
            }
            visible.forEach(({ video }) => renderLabCard(list, video, videoMatchCount(video, pairWanted)));
        }

        if (pageCount > 1) {
            const pager = node('nav', null, 'lab-video-pagination');
            pager.setAttribute('aria-label', '一覧のページ');
            const prev = node('button', '前へ', 'btn-outline');
            prev.type = 'button';
            prev.disabled = state.page === 0;
            prev.addEventListener('click', () => {
                state.page -= 1;
                renderVideos();
                root.scrollIntoView({ block: 'start' });
            });
            const label = node('span', (state.page + 1) + ' / ' + pageCount);
            const next = node('button', '次へ', 'btn-outline');
            next.type = 'button';
            next.disabled = state.page >= pageCount - 1;
            next.addEventListener('click', () => {
                state.page += 1;
                renderVideos();
                root.scrollIntoView({ block: 'start' });
            });
            pager.append(prev, label, next);
            root.append(pager);
        }

        for (const tab of modes) {
            const active = tab.dataset.view === state.mode;
            tab.setAttribute('aria-pressed', String(active));
            tab.classList.toggle('lab-primary', active);
        }
    }

    // 連続再生: 絞り込み結果の先頭の試合から再生し、あとは同期に任せる
    async function buildFeatureSegments(videos) {
        const segments = [];
        for (const video of videos) {
            let rows = [];
            try {
                const body = await api('/api/videos/' + video.id + '/matches?light=1');
                rows = body.matches || [];
                state.matchCache.set(video.id, rows);
            } catch (error) {
                rows = [];
            }
            // 動画をまたいで start で並べ替えると、別の動画の試合が混ざる。
            // 動画の順は絞り込み結果のまま、同じ動画の中だけ試合番号順にする。
            // 試合一覧と同じ条件（プレイヤー1/2・勝敗）で絞るので件数も一致する。
            rows.slice().sort((a, b) => (Number(a.index) || 0) - (Number(b.index) || 0))
                .forEach(row => {
                    // 盤面復元に失敗した試合はビューワーが空になるので、連続再生では飛ばす
                    if (row.failed) return;
                    if (!matchFilterOk(row)) return;
                    const start = Number(row.start) || 0;
                    const end = Number(row.end) || (start + 60);
                    if (end > start + 1) {
                        segments.push({ videoId: video.id, matchIndex: row.index, start, end });
                    }
                });
        }
        return segments;
    }

    // 連続再生: 絞り込み結果の試合だけを、特集動画のように続けて再生する
    async function playSequentially() {
        const videos = sortEntries(filterVideos(state.videos)).filter(video => (video.matchCount || 0) > 0);
        if (!videos.length) {
            toast('再生できる試合がありません。');
            return;
        }
        toast('再生リストを作っています…');
        const segments = await buildFeatureSegments(videos);
        if (!segments.length) {
            toast('再生できる試合がありません。');
            return;
        }
        state.feature = { segments, index: 0 };
        state.featureList = videos.map(video => video.id);
        state.featureIndex = -1;
        state.featurePending = null;
        state.lastFeatureSeekAt = 0;
        const first = segments[0];
        state.videoId = first.videoId;
        state.pendingMatch = first.matchIndex;
        state.follow = true;
        // 連続再生は動画一覧の絞り込みで作る。動画詳細側の絞り込みは解除して始める
        state.matchFilters = { query: '', player: '', opponent: '' };
        if (location.hash === '#video/' + first.videoId) route();
        else location.hash = '#video/' + first.videoId;
        toast('絞り込み結果の' + segments.length + '試合を続けて再生します。');
    }

    // 連続再生をやめる（手動で試合を選び直したときなど）
    function stopFeature() {
        state.feature = { segments: [], index: -1 };
        state.featureList = [];
        state.featureIndex = -1;
        state.featurePending = null;
        state.lastFeatureSeekAt = 0;
    }

    // 現在の試合が終わったら次の試合へ（試合の間は飛ばす）
    function advanceFeature() {
        if (!state.feature || !state.feature.segments.length) return false;
        const next = state.feature.index + 1;
        if (next >= state.feature.segments.length) {
            toast('連続再生が終わりました。');
            stopFeature();
            return true;
        }
        const segment = state.feature.segments[next];
        state.feature.index = next;
        state.lastFeatureSeekAt = 0;
        state.featurePending = { videoId: segment.videoId, matchIndex: segment.matchIndex };
        if (segment.videoId === state.videoId) {
            const row = (state.matches || []).find(item => item.index === segment.matchIndex);
            if (row) {
                showMatch(row, { fromSync: true })
                    .catch(() => {
                        state.featurePending = null;
                    });
                seekVideoTo(segment.start, { pause: false });
                return true;
            }
            // 解析結果から試合が消えている → 次のセグメントへ送る
            return advanceFeature();
        }
        state.pendingMatch = segment.matchIndex;
        state.videoId = segment.videoId;
        location.hash = '#video/' + segment.videoId;
        return true;
    }

    function featureTick() {
        if (state.featurePending) return;          // 次の試合・動画の読み込み待ち
        const segment = state.feature.segments[state.feature.index];
        if (!segment || segment.videoId !== state.videoId) return;
        if (!state.pages.length) return;
        if (!state.currentMatch || state.currentMatch.index !== segment.matchIndex) return;
        const seconds = videoTime();
        if (!Number.isFinite(seconds)) return;
        if (seconds >= segment.end - 0.25) {
            advanceFeature();
            return;
        }
        if (seconds < segment.start - 0.5) {
            // シークが効かないときに 250ms ごと投げ直さないよう間隔をあける
            if (Date.now() - (state.lastFeatureSeekAt || 0) > 2000) {
                state.lastFeatureSeekAt = Date.now();
                seekVideoTo(segment.start, { pause: false });
            }
            return;
        }
        const page = pageIndexAt(state.pages, seconds - segment.start);
        if (page >= 0 && page !== state.pageIndex) showPage(page, { seek: false });
    }
    // ---- テト譜のプレビュー -------------------------------------------------
    function tetofuBoardText(value) {
        if (Array.isArray(value)) {
            return value.map(row => (Array.isArray(row) ? row.join('') : String(row || ''))).join('');
        }
        return String(value || '');
    }

    const TETOFU_DECODER = new TextDecoder();

    // 保存したURLを、カードに出す1ページぶんだけ復号する。
    // ページ列を丸ごと抱えると重いので、盤面とNEXTだけ残す。
    function tetofuPageData(item) {
        const hash = String(item.url || '').split('#')[1];
        if (!hash) return null;
        try {
            const binary = atob(hash.replace(/-/g, '+').replace(/_/g, '/'));
            const json = JSON.parse(TETOFU_DECODER.decode(Uint8Array.from(binary, ch => ch.charCodeAt(0))));
            if (window.TetrisEventCodec && window.TetrisEventCodec.isEventReplay(json)) {
                const decoded = window.TetrisEventCodec.decodeCollection(json, { compactBoards: true });
                const target = decoded.cases[decoded.currentCase || 0];
                const pages = (target && target.pages) || [];
                if (!pages.length) return null;
                const requested = Number((String(item.url).match(/[?&]page=(\d+)/) || [])[1] || 1);
                // 試合全体は1ページ目が空の盤面なので、プレビューは試合の中ほどを出す
                const index = item.scope === 'match'
                    ? Math.floor((pages.length - 1) / 2)
                    : Math.max(0, Math.min(requested - 1, pages.length - 1));
                const page = pages[index];
                const pick = player => (player ? {
                    board: String(player.board || ''),
                    next: String(player.next || '')
                } : null);
                return { p1: pick(page.p1), p2: page.p2 ? pick(page.p2) : null };
            }
            // 旧形式（v2のスナップショット）は盤面とNEXTがそのまま入っている
            const snapshot = panel => {
                if (!panel || typeof panel !== 'object') return null;
                const board = panel.b !== undefined ? panel.b : panel.board;
                if (board === undefined) return null;
                const next = panel.n !== undefined ? panel.n : panel.next;
                return { board: tetofuBoardText(board), next: String(next || '') };
            };
            const p1 = snapshot(json.p1);
            const p2 = snapshot(json.p2);
            return p1 || p2 ? { p1, p2 } : null;
        } catch (error) {
            return null;
        }
    }

    // 今のシミュレータ（白いページ＋濃紺の盤面＋パステルのミノ）に合わせる。
    // 形・色・NEXT見出しの並びは runtime-config.js と 30-viewer.js から持ってくる。
    const TETOFU_BLOCK = 28;
    const TETOFU_PREVIEW_WIDTH = 460;
    const TETOFU_PREVIEW_HEIGHT = 600;
    const TETOFU_BOARD_X = 10;
    const TETOFU_BOARD_Y = 20;
    const TETOFU_BOARD_WIDTH = 10 * TETOFU_BLOCK;
    const TETOFU_BOARD_HEIGHT = 20 * TETOFU_BLOCK;
    const TETOFU_NEXT_X = 340;
    const TETOFU_NEXT_TOP = 95;
    const TETOFU_NEXT_SLOT = 80;
    const TETOFU_BOARD_BG = '#1b1b1f';
    const TETOFU_GRID = '#444';
    const TETOFU_NEXT_INK = '#6f6f6f';
    const TETOFU_COLORS = {
        I: '#4ec7cd', O: '#eac44a', T: '#b56fc7', L: '#e0a15a',
        J: '#587aca', S: '#82c964', Z: '#e97d7d', G: '#b0b0b0'
    };
    const TETOFU_TETROMINOS = {
        I: { shape: [[0, 0], [1, 0], [2, 0], [3, 0]], center: [1.5, 0.5] },
        O: { shape: [[0, 0], [1, 0], [0, -1], [1, -1]], center: [0.5, -0.5] },
        T: { shape: [[0, 0], [-1, 0], [0, -1], [1, 0]], center: [0, 0] },
        L: { shape: [[-1, 0], [0, 0], [1, 0], [1, -1]], center: [0, 0] },
        J: { shape: [[0, 0], [-1, 0], [1, 0], [-1, -1]], center: [0, 0] },
        S: { shape: [[1, -1], [-1, 0], [0, 0], [0, -1]], center: [0, 0] },
        Z: { shape: [[0, 0], [1, 0], [0, -1], [-1, -1]], center: [0, 0] }
    };
    const tetofuPreviewCache = new Map();

    function tetofuPreviewData(item) {
        const key = String(item.id || '') + '|' + String(item.url || '');
        if (tetofuPreviewCache.has(key)) return tetofuPreviewCache.get(key);
        const data = tetofuPageData(item);
        if (tetofuPreviewCache.size > 300) tetofuPreviewCache.clear();
        tetofuPreviewCache.set(key, data);
        return data;
    }

    function drawTetofuBlock(ctx, piece, x, y, size) {
        ctx.fillStyle = TETOFU_COLORS[piece] || '#b0b0b0';
        ctx.fillRect(x, y, size, size);
        ctx.strokeStyle = '#444';
        ctx.lineWidth = 0.5;
        ctx.strokeRect(x, y, size, size);
    }

    function drawTetofuGrid(ctx, x, y, width, height) {
        ctx.strokeStyle = TETOFU_GRID;
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (let col = 1; col < 10; col += 1) {
            ctx.moveTo(x + col * TETOFU_BLOCK + 0.5, y);
            ctx.lineTo(x + col * TETOFU_BLOCK + 0.5, y + height);
        }
        for (let row = 1; row < 20; row += 1) {
            ctx.moveTo(x, y + row * TETOFU_BLOCK + 0.5);
            ctx.lineTo(x + width, y + row * TETOFU_BLOCK + 0.5);
        }
        ctx.stroke();
    }

    function drawTetofuBoard(ctx, board) {
        const width = 10;
        const visible = 20;
        const offset = 20 * width; // 40行のうち下20行
        const x = TETOFU_BOARD_X;
        const y = TETOFU_BOARD_Y;
        const size = TETOFU_BLOCK;
        ctx.fillStyle = TETOFU_BOARD_BG;
        ctx.fillRect(x, y, TETOFU_BOARD_WIDTH, TETOFU_BOARD_HEIGHT);
        drawTetofuGrid(ctx, x, y, TETOFU_BOARD_WIDTH, TETOFU_BOARD_HEIGHT);
        for (let row = 0; row < visible; row += 1) {
            for (let col = 0; col < width; col += 1) {
                const value = String(board || '')[offset + row * width + col] || '';
                if (!value || value === '_' || value === '.' || value === ' ') continue;
                drawTetofuBlock(ctx, value, x + col * size, y + row * size, size);
            }
        }
    }

    function drawTetofuNext(ctx, sequence) {
        const queue = String(sequence || '').split('');
        const pieces = [];
        for (let i = 0; i < Math.min(queue.length, 10); i += 1) {
            const piece = queue[i];
            if (piece && TETOFU_TETROMINOS[piece]) pieces.push(piece);
        }
        ctx.textAlign = 'left';
        ctx.fillStyle = TETOFU_NEXT_INK;
        ctx.font = '600 26px -apple-system, "Segoe UI", "Noto Sans JP", sans-serif';
        ctx.fillText('NEXT', TETOFU_NEXT_X, 60);
        pieces.forEach((piece, index) => {
            const entry = TETOFU_TETROMINOS[piece];
            const minX = Math.min(...entry.shape.map(cell => cell[0]));
            const minY = Math.min(...entry.shape.map(cell => cell[1]));
            const top = TETOFU_NEXT_TOP + index * TETOFU_NEXT_SLOT;
            entry.shape.forEach(cell => {
                drawTetofuBlock(ctx, piece,
                    TETOFU_NEXT_X + (cell[0] - minX) * TETOFU_BLOCK,
                    top + (cell[1] - minY) * TETOFU_BLOCK,
                    TETOFU_BLOCK);
            });
        });
    }

    function drawTetofuPlayer(ctx, player) {
        drawTetofuBoard(ctx, player.board);
        drawTetofuNext(ctx, player.next);
    }

    function drawTetofuPlaceholder(ctx, width, height) {
        ctx.fillStyle = '#ffffff';
        ctx.fillRect(0, 0, width, height);
        ctx.fillStyle = TETOFU_BOARD_BG;
        ctx.fillRect(TETOFU_BOARD_X, TETOFU_BOARD_Y,
            TETOFU_BOARD_WIDTH, TETOFU_BOARD_HEIGHT);
        drawTetofuGrid(ctx, TETOFU_BOARD_X, TETOFU_BOARD_Y,
            TETOFU_BOARD_WIDTH, TETOFU_BOARD_HEIGHT);
        ctx.fillStyle = '#8a8a8a';
        ctx.font = '14px system-ui, sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText('プレビューなし', width / 2, height / 2);
    }

    function drawTetofuPreview(canvas, item) {
        const dpr = Math.min(1.5, window.devicePixelRatio || 1);
        const width = TETOFU_PREVIEW_WIDTH;
        const height = TETOFU_PREVIEW_HEIGHT;
        if (canvas.width !== width * dpr || canvas.height !== height * dpr) {
            canvas.width = width * dpr;
            canvas.height = height * dpr;
        }
        const ctx = canvas.getContext('2d');
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, width, height);
        ctx.fillStyle = '#ffffff';
        ctx.fillRect(0, 0, width, height);
        const data = tetofuPreviewData(item);
        const panels = data ? [data.p1, data.p2].filter(Boolean) : [];
        if (!panels.length) {
            drawTetofuPlaceholder(ctx, width, height);
            canvas.dataset.ready = '1';
            return;
        }
        if (panels.length > 1) {
            // 2Pは左右に半分の縮尺で並べる
            const scale = 0.5;
            const top = (height - height * scale) / 2;
            panels.slice(0, 2).forEach((player, index) => {
                ctx.save();
                ctx.translate(index * width * scale, top);
                ctx.scale(scale, scale);
                drawTetofuPlayer(ctx, player);
                ctx.restore();
            });
        } else {
            drawTetofuPlayer(ctx, panels[0]);
        }
        canvas.dataset.ready = '1';
    }

    // 画面に入ったカードだけ描く（スクロールの先のカードは復号しない）
    let tetofuObserver = null;
    function observeTetofuPreview(frame, item) {
        const draw = () => {
            const canvas = frame.querySelector('canvas');
            if (canvas) drawTetofuPreview(canvas, item);
        };
        if (!('IntersectionObserver' in window)) {
            requestAnimationFrame(draw);
            return;
        }
        if (!tetofuObserver) {
            tetofuObserver = new IntersectionObserver(entries => {
                entries.forEach(entry => {
                    if (!entry.isIntersecting) return;
                    tetofuObserver.unobserve(entry.target);
                    const canvas = entry.target.querySelector('canvas');
                    if (canvas) drawTetofuPreview(canvas, entry.target.__tetofuItem);
                });
            }, { rootMargin: '240px' });
        }
        frame.__tetofuItem = item;
        tetofuObserver.observe(frame);
    }

    function closeTetofuMenu() {
        document.querySelectorAll('.ppt-card-menu.open').forEach(menu => {
            menu.classList.remove('open');
            const trigger = menu.querySelector('.ppt-card-edit');
            if (trigger) trigger.setAttribute('aria-expanded', 'false');
        });
    }

    function openTetofuMenu(card) {
        const menu = card.querySelector('.ppt-card-menu');
        if (!menu) return;
        const open = !menu.classList.contains('open');
        closeTetofuMenu();
        menu.classList.toggle('open', open);
        const trigger = menu.querySelector('.ppt-card-edit');
        if (trigger) {
            trigger.setAttribute('aria-expanded', String(open));
            if (open) trigger.focus();
        }
    }

    function openTetofuRename(item) {
        state.entryMode = 'rename-tetofu';
        state.tetofuId = item.id;
        dialogTitle.textContent = '名前を変更';
        dialogNote.textContent = 'テト譜の表示名です。';
        entryTitle.value = item.title || '';
        entryUrl.value = '';
        entryTitle.parentElement.hidden = false;
        entryUrl.parentElement.hidden = true;
        entryTitle.required = true;
        entrySubmit.textContent = '保存する';
        dialog.showModal();
        entryTitle.focus();
        entryTitle.select();
    }

    // カード右上の鉛筆。名前変更・タグ・削除はここにまとめる。
    function tetofuEditMenu(item) {
        const menu = node('div', null, 'ppt-card-menu');
        const trigger = node('button', null, 'ppt-icon-button ppt-card-edit');
        trigger.type = 'button';
        trigger.title = '編集';
        trigger.setAttribute('aria-label', '編集');
        trigger.setAttribute('aria-haspopup', 'true');
        trigger.setAttribute('aria-expanded', 'false');
        trigger.innerHTML = '<svg viewBox="0 0 24 24" width="15" height="15" aria-hidden="true"><path d="M4 20h4l10-10-4-4L4 16v4z" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/><path d="M13.6 5.6l4 4" fill="none" stroke="currentColor" stroke-width="1.8"/></svg>';

        const actions = node('div', null, 'ppt-card-actions');
        const rename = node('button', '名前を変更');
        rename.type = 'button';
        rename.addEventListener('click', () => {
            closeTetofuMenu();
            openTetofuRename(item);
        });
        const tags = node('button', 'タグを編集');
        tags.type = 'button';
        tags.addEventListener('click', () => {
            closeTetofuMenu();
            openTagDialog({ id: item.id, kind: 'tetofu', title: item.title, tags: item.tags || [], autoTags: [] });
        });
        const remove = node('button', '削除', 'ppt-danger');
        remove.type = 'button';
        remove.addEventListener('click', async () => {
            closeTetofuMenu();
            if (!window.confirm('「' + item.title + '」を削除しますか？')) return;
            try {
                await api('/api/tetofu/' + item.id, { method: 'DELETE' });
                await loadTetofu();
                render();
                notify('テト譜を削除しました。');
            } catch (error) {
                toast(error.message);
            }
        });
        actions.append(rename, tags, remove);

        trigger.addEventListener('click', event => {
            event.preventDefault();
            event.stopPropagation();
            const open = !menu.classList.contains('open');
            closeTetofuMenu();
            menu.classList.toggle('open', open);
            trigger.setAttribute('aria-expanded', String(open));
        });
        menu.append(trigger, actions);
        return menu;
    }

    function renderTetofuCard(container, item) {
        const card = node('article', null, 'card ppt-tetofu-card');
        const frame = node('div', null, 'ppt-tetofu-frame');

        // 盤面＋NEXTだけの軽量プレビュー（iframeは使わない）
        const link = node('a', null, 'ppt-tetofu-open');
        link.href = item.url;
        link.target = '_blank';
        link.rel = 'noopener';
        link.setAttribute('aria-label', (item.title || 'テト譜') + ' を開く');
        // 設定が「このウィンドウ内」なら、Lab の上に重ねて開く
        link.addEventListener('click', event => {
            openLinkInLab(event, item.url, item.title || 'テト譜');
        });
        const canvas = node('canvas', null, 'ppt-tetofu-canvas');
        canvas.width = TETOFU_PREVIEW_WIDTH;
        canvas.height = TETOFU_PREVIEW_HEIGHT;
        canvas.dataset.ready = '0';
        link.append(canvas);
        // タイトルはプレビュー下部に重ねて常時表示（盤面に被ってよい）
        link.append(node('span', item.title || '', 'ppt-tetofu-title'));
        frame.append(link);

        if (document.documentElement.dataset.readonly !== 'true' || document.documentElement.dataset.localSaves === 'true') {
            // 鉛筆はプレビュー右下に固定
            frame.append(tetofuEditMenu(item));
            card.addEventListener('contextmenu', event => {
                if (event.target.closest('.ppt-tetofu-tags, .ppt-card-menu')) return;
                event.preventDefault();
                openTetofuMenu(card);
            });
        }
        card.append(frame);

        const tagRow = node('div', null, 'lab-video-tags ppt-tetofu-tags');
        (item.tags || []).forEach(tag => tagRow.append(tagLink(tag)));
        card.append(tagRow);

        // 動画から保存したものは、元の動画のその時刻へ飛べる
        if (item.videoId) {
            const video = (state.videos || []).find(entry => entry.id === item.videoId);
            const seconds = Number(item.seconds);
            const jump = node('a', '▶ 元の動画 ' + (Number.isFinite(seconds) ? timeText(seconds) : '')
                + (item.matchIndex ? '（試合 ' + item.matchIndex + '）' : ''), 'ppt-tetofu-source');
            jump.href = '#video/' + item.videoId;
            if (video && video.title) jump.title = video.title;
            jump.addEventListener('click', event => {
                event.preventDefault();
                state.pendingMatch = item.matchIndex || 0;
                state.pendingSeek = Number.isFinite(seconds) ? seconds : null;
                location.hash = '#video/' + item.videoId;
            });
            card.append(jump);
        }

        container.append(card);
        observeTetofuPreview(frame, item);
    }
    // テト譜の種類（絞り込み用）。動画から保存したものは scope、手で登録したものは「手動登録」
    const TETOFU_KINDS = [['', 'すべて'], ['match', '試合全体'], ['all', '決着盤面'],
        ['p1', 'P1の局面'], ['p2', 'P2の局面'], ['manual', '手動登録']];

    function tetofuKindOf(item) {
        if (!item.videoId) return 'manual';
        return ['match', 'all', 'p1', 'p2'].includes(item.scope) ? item.scope : 'manual';
    }

    function renderTetofu() {
        const root = node('section', null, 'lab-video-library');
        mainView.replaceChildren(root);

        const header = node('div', null, 'lab-video-library-heading');
        const addBar = node('div', null, 'lab-inline-actions');
        for (const [label, mode] of [['シミュレータリンクを登録', 'simulator'],
            ['エディタリンクを登録', 'editor'], ['本家テト譜リンクを登録', 'official']]) {
            const button = node('button', label, 'btn-outline');
            button.type = 'button';
            button.addEventListener('click', () => openEntry(mode));
            addBar.append(button);
        }
        header.append(addBar);
        root.append(header);

        // 動画一覧と同じ絞り込み（検索・タグ）
        const controls = node('div', null, 'lab-video-filters');
        const searchWrap = node('label', '検索');
        const search = node('input');
        search.type = 'search';
        search.placeholder = 'タイトル・タグ';
        search.value = state.tetofuQuery || '';
        search.setAttribute('aria-label', 'テト譜を検索');
        search.addEventListener('input', () => {
            // 1文字ごとに作り直すと入力欄が作り直されて打てなくなるので、少し待ってから描き直す
            const value = search.value;
            clearTimeout(state.tetofuSearchTimer);
            state.tetofuSearchTimer = setTimeout(() => {
                state.tetofuQuery = value;
                renderTetofu();
                const next = document.querySelector('.lab-video-filters input[type=search]');
                if (next) {
                    next.focus();
                    next.setSelectionRange(next.value.length, next.value.length);
                }
            }, 260);
        });
        searchWrap.append(search);
        controls.append(searchWrap);

        const select = (label, key, choices) => {
            const wrap = node('label', label);
            const input = node('select');
            input.setAttribute('aria-label', label);
            choices.forEach(([value, text]) => {
                const option = node('option', text);
                option.value = value;
                input.append(option);
            });
            input.value = state[key] || choices[0][0];
            input.addEventListener('change', () => {
                state[key] = input.value;
                renderTetofu();
            });
            wrap.append(input);
            controls.append(wrap);
        };
        select('種類', 'tetofuKind', TETOFU_KINDS.map(([value, text]) => [value, text]));
        const videoIds = [...new Set(state.tetofu.map(item => item.videoId).filter(Boolean))];
        const videoTitle = id => ((state.videos || []).find(video => video.id === id) || {}).title || '（削除された動画）';
        select('動画', 'tetofuVideo', [['', 'すべて'], ...videoIds
            .map(id => [id, videoTitle(id)])
            .sort((a, b) => a[1].localeCompare(b[1], 'ja'))]);
        select('並び順', 'tetofuSort', [['new', '新しい順'], ['old', '古い順'], ['title', '名前順'], ['video', '動画の時刻順']]);

        const tagChoices = [...new Set(state.tetofu.flatMap(item => item.tags || []))].sort();
        const tagWrap = node('label', 'タグ');
        const tagSelect = node('select');
        tagSelect.setAttribute('aria-label', 'タグ');
        [['', 'すべて'], ...tagChoices.map(name => [name, name])].forEach(([value, text]) => {
            const option = node('option', text);
            option.value = value;
            tagSelect.append(option);
        });
        tagSelect.value = state.tetofuTag || '';
        tagSelect.addEventListener('change', () => {
            state.tetofuTag = tagSelect.value;
            renderTetofu();
        });
        tagWrap.append(tagSelect);
        controls.append(tagWrap);

        const clear = node('button', '絞り込みを解除', 'btn-outline');
        clear.type = 'button';
        clear.addEventListener('click', () => {
            state.tetofuQuery = '';
            state.tetofuTag = '';
            state.tetofuKind = '';
            state.tetofuVideo = '';
            state.tetofuSort = 'new';
            renderTetofu();
        });
        controls.append(clear);
        root.append(controls);

        const ast = parseQuery(state.tetofuQuery);
        const wanted = state.view === 'tags' ? selectedTags() : [];
        const items = state.tetofu.filter(item => {
            const tags = item.tags || [];
            if (state.tetofuTag && !tags.includes(state.tetofuTag)) return false;
            if (state.tetofuKind && tetofuKindOf(item) !== state.tetofuKind) return false;
            if (state.tetofuVideo && item.videoId !== state.tetofuVideo) return false;
            if (!hasAllTags(tags, wanted)) return false;
            return matchQuery(ast, tags, item.title + ' ' + item.url);
        });
        const sort = state.tetofuSort || 'new';
        const created = item => String(item.createdAt || '');
        items.sort((a, b) => {
            if (sort === 'old') return created(a).localeCompare(created(b));
            if (sort === 'title') return String(a.title || '').localeCompare(String(b.title || ''), 'ja');
            if (sort === 'video') {
                // 動画から保存したものを動画ごと・時刻順に。手動登録は後ろ
                const va = a.videoId || '￿', vb = b.videoId || '￿';
                if (va !== vb) return va.localeCompare(vb);
                return (Number(a.seconds) || 0) - (Number(b.seconds) || 0);
            }
            return created(b).localeCompare(created(a));
        });
        root.append(node('p', items.length + 'テト譜', 'lab-video-count'));

        const list = node('div', null, 'ppt-tile-grid ppt-grid-tetofu');
        if (!items.length) {
            list.append(node('p', state.tetofu.length
                ? '条件に合うテト譜はありません。'
                : 'まだテト譜がありません。右上のボタンから登録できます。', 'lab-help-text'));
        }
        items.forEach(item => renderTetofuCard(list, item));
        root.append(list);
    }
    function renderTags() {
        const root = node('section', null, 'lab-video-library');
        mainView.replaceChildren(root);

        const header = node('div', null, 'lab-video-library-heading');
        root.append(header);

        // 追加だけは Lab の公開画面にないので、見出しの下に小さく置く
        const form = node('form', null, 'lab-inline-actions');
        const input = node('input');
        input.type = 'search';
        input.placeholder = '新しいタグ';
        input.setAttribute('aria-label', '新しいタグ');
        input.className = 'ppt-tag-input';
        const submit = node('button', 'タグを追加', 'btn-outline');
        submit.type = 'submit';
        form.append(input, submit);
        form.addEventListener('submit', async event => {
            event.preventDefault();
            const name = input.value.trim();
            if (!name) return;
            try {
                await api('/api/tags', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ name })
                });
                input.value = '';
                await loadTags();
                render();
            } catch (error) {
                toast(error.message);
            }
        });
        root.append(form);

        // タグの検索（画面上部の検索欄をやめたので、ここに置く）
        const searchWrap = node('label', '検索', 'ppt-tag-search-wrap');
        const search = node('input', null, 'ppt-tag-search');
        search.type = 'search';
        search.placeholder = 'タグを検索（and / or / () 可）';
        search.value = state.tagQuery || '';
        search.setAttribute('aria-label', 'タグを検索');
        search.addEventListener('input', () => {
            const value = search.value;
            clearTimeout(state.tagSearchTimer);
            state.tagSearchTimer = setTimeout(() => {
                state.tagQuery = value;
                render();
                const next = document.querySelector('.ppt-tag-search');
                if (next) {
                    next.focus();
                    next.setSelectionRange(next.value.length, next.value.length);
                }
            }, 260);
        });
        searchWrap.append(search);
        root.append(searchWrap);

        const selected = state.tagSelected || new Set();
        const query = (state.tagQuery || '').trim().toLowerCase();
        // 検索欄が空なら全タグを出す
        const simple = query && !/[()]|\band\b|\bor\b|&&|\|\|/.test(query) ? query : '';
        // 通常のタグ（state.tags）に、動画から作る配信者・プレイヤーのタグを足して並べる
        const autoCounts = new Map();
        (state.videos || []).forEach(video => {
            new Set(videoTagKeys(video).filter(key => AUTO_TAG_KINDS.includes(tagKind(key))))
                .forEach(key => autoCounts.set(key, (autoCounts.get(key) || 0) + 1));
        });
        const allTags = [
            ...state.tags.map(tag => ({ key: tag.name, name: tag.name, count: tag.count || 0 })),
            ...Array.from(autoCounts, ([key, count]) => ({ key, name: tagName(key), count }))
                .sort((a, b) => a.name.localeCompare(b.name, 'ja'))
        ];
        const tags = allTags.filter(tag => !simple || tag.name.toLowerCase().includes(simple));
        const list = node('div', null, 'ppt-tag-cloud');
        if (!tags.length) {
            list.append(node('p', allTags.length
                ? '条件に合うタグはありません。'
                : 'タグがありません。動画やテト譜に付けるラベルを作れます。', 'lab-help-text'));
        }
        const groups = { streamer: [], free: [], opening: [], midgame: [], player: [] };
        tags.forEach(tag => groups[tagKind(tag.key)].push(tag));
        const groupBoxes = {};
        TAG_KIND_ORDER.forEach(kind => {
            if (!groups[kind].length) return;
            const box = node('div', null, 'ppt-tag-group');
            box.append(node('p', TAG_KIND_LABELS[kind] + '（' + groups[kind].length + '）', 'ppt-tag-group-title'));
            groupBoxes[kind] = box;
            list.append(box);
        });
        const selectedKeys = new Set(selectedTags());
        tags.forEach(tag => {
            const active = selectedKeys.has(tag.key);
            const kind = tagKind(tag.key);
            const pill = node('button', tag.name + ' (' + (tag.count || 0) + ')',
                'tag ppt-tag-pill tag-kind-' + kind + (active ? ' active' : ''));
            pill.type = 'button';
            pill.title = 'クリックで絞り込み（複数選択でAND）';
            pill.addEventListener('click', () => {
                const selected = new Set(selectedTags());
                if (selected.has(tag.key)) selected.delete(tag.key);
                else selected.add(tag.key);
                state.tagSelected = selected;
                const names = Array.from(selected);
                history.replaceState(null, '', '#tags' + (names.length ? '/' + names.map(encodeURIComponent).join('+') : ''));
                render();
            });
            groupBoxes[kind].append(pill);
        });
        root.append(list);

        if (!selected.size && !query) return;

        // 選んだタグ（AND）と条件式に合う項目を並べる
        const ast = parseQuery(state.tagQuery);
        const wanted = state.view === 'tags' ? selectedTags() : [];
        const videos = state.videos.filter(video => {
            const names = videoTagNames(video);
            const text = [video.title, ...(video.players || [])].join(' ');
            return hasAllTags(videoTagKeys(video), wanted) && matchQuery(ast, names, text);
        });
        const tetofu = state.tetofu.filter(item => {
            const tags = item.tags || [];
            return hasAllTags(tags, wanted) && matchQuery(ast, tags, item.title + ' ' + item.url);
        });
        const total = videos.length + tetofu.length;
        const clear = node('button', '絞り込みを解除', 'btn-outline');
        clear.type = 'button';
        clear.addEventListener('click', () => {
            state.tagSelected = new Set();
            state.tagQuery = '';
            render();
        });
        const bar = node('div', null, 'lab-inline-actions');
        bar.append(node('span', '該当 ' + total + ' 件', 'lab-video-count'), clear);
        // 1つだけ選んだタグが自分で作ったタグなら、種類を変えられる
        if (wanted.length === 1 && !AUTO_TAG_KINDS.includes(tagKind(wanted[0]))
            && (state.tags || []).some(tag => tag.name === wanted[0])) {
            const label = node('label', '種類', 'ppt-tag-search-wrap');
            const select = node('select', null, 'ppt-tag-kind-select');
            [['free', 'その他'], ['opening', '開幕テンプレ'], ['midgame', '途中テンプレ']].forEach(([value, text]) => {
                const option = node('option', text);
                option.value = value;
                select.append(option);
            });
            select.value = tagKind(wanted[0]);
            select.addEventListener('change', async () => {
                try {
                    await api('/api/tags/kind', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ name: wanted[0], kind: select.value })
                    });
                    await loadTags();
                    render();
                } catch (error) {
                    toast(error.message);
                }
            });
            label.append(select);
            bar.append(label);
            // タグそのものを消す（全部の動画・テト譜から外れる）。動画のメニューからここへ移した
            const remove = node('button', 'このタグを削除', 'btn-outline ppt-danger');
            remove.type = 'button';
            remove.addEventListener('click', async () => {
                const name = wanted[0];
                const count = ((state.tags || []).find(tag => tag.name === name) || {}).count || 0;
                if (!window.confirm('タグ「' + name + '」を削除しますか？\n付いている ' + count
                    + ' 件（動画・テト譜）からすべて外れます。元に戻せません。')) return;
                try {
                    await api('/api/tags', {
                        method: 'DELETE',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ name })
                    });
                    await Promise.all([loadTags(), loadTetofu(), loadVideos()]);
                    location.hash = '#tags';
                    notify('タグ「' + name + '」を削除しました。');
                } catch (error) {
                    toast(error.message);
                }
            });
            bar.append(remove);
        }
        root.append(bar);
        // プレイヤーのタグを1人だけ選んだら、その人の戦績を出す
        if (wanted.length === 1 && !query && tagKind(wanted[0]) === 'player') {
            renderPlayerPanel(root, tagName(wanted[0]));
        }

        const items = node('div', null, 'ppt-tile-grid');
        if (!total) {
            items.append(node('p', '条件に合う項目はありません。', 'lab-help-text'));
        }
        videos.forEach(video => renderLabCard(items, video));
        tetofu.forEach(item => renderTetofuCard(items, item));
        root.append(items);
    }

    // ---- プレイヤーの戦績 ----------------------------------------------------
    function percentText(value) {
        return value === null || value === undefined ? '—' : Math.round(value * 100) + '%';
    }

    // テンプレの割合の円グラフ（items: [{name, count}]。無ければ灰色の輪だけ）
    const PIE_COLORS = ['#7fb8e6', '#8fd08a', '#f0c96a', '#e79a9a', '#b7a3e3', '#7fd1c4', '#c9c9c9'];
    function templatePie(items) {
        const ns = 'http://www.w3.org/2000/svg';
        const svg = document.createElementNS(ns, 'svg');
        svg.setAttribute('viewBox', '-1 -1 2 2');
        svg.setAttribute('aria-hidden', 'true');
        const total = items.reduce((sum, item) => sum + item.count, 0);
        if (!total) {
            const ring = document.createElementNS(ns, 'circle');
            ring.setAttribute('r', '0.8');
            ring.setAttribute('fill', 'none');
            ring.setAttribute('stroke', 'rgba(127,127,127,.25)');
            ring.setAttribute('stroke-width', '0.3');
            svg.append(ring);
            return svg;
        }
        let angle = -Math.PI / 2;
        items.forEach((item, index) => {
            const share = item.count / total;
            const end = angle + share * Math.PI * 2;
            const shape = document.createElementNS(ns, share >= 0.9999 ? 'circle' : 'path');
            if (share >= 0.9999) {
                shape.setAttribute('r', '1');
            } else {
                const large = share > 0.5 ? 1 : 0;
                shape.setAttribute('d', 'M 0 0 L ' + Math.cos(angle) + ' ' + Math.sin(angle)
                    + ' A 1 1 0 ' + large + ' 1 ' + Math.cos(end) + ' ' + Math.sin(end) + ' Z');
            }
            shape.setAttribute('fill', item.color || PIE_COLORS[index % PIE_COLORS.length]);
            svg.append(shape);
            angle = end;
        });
        return svg;
    }

    function renderPlayerPanel(root, name) {
        const panel = node('section', null, 'ppt-player-panel');
        root.append(panel);
        state.playerStats = state.playerStats || new Map();
        const period = state.playerPeriod || 'all';
        const cacheKey = name + '\u0000' + period;
        const cached = state.playerStats.get(cacheKey);
        const left = node('div');
        const titleRow = node('div', null, 'ppt-player-title');
        titleRow.append(node('h3', name));
        // 期間はその動画が YouTube に上がった日で数える
        const periods = node('div', null, 'ppt-player-periods');
        [['all', '全体'], ['half', '半年'], ['30d', '30日'], ['7d', '7日']].forEach(([value, label]) => {
            const button = node('button', label, 'btn-outline' + (value === period ? ' active' : ''));
            button.type = 'button';
            button.setAttribute('aria-pressed', String(value === period));
            button.addEventListener('click', () => {
                if (state.playerPeriod === value) return;
                state.playerPeriod = value;
                render();
            });
            periods.append(button);
        });
        titleRow.append(periods);
        left.append(titleRow);
        panel.append(left);
        if (!cached) {
            left.append(node('p', '集計中…', 'ppt-player-empty'));
            api('/api/players/stats?name=' + encodeURIComponent(name) + '&period=' + period).then(body => {
                state.playerStats.set(cacheKey, body);
                // 選択中のキーは「player:名前」なので名前で比べる
                if (state.view === 'tags' && selectedTags().length === 1
                    && selectedTags()[0] === autoTagKey('player', name)) render();
            }).catch(error => {
                left.append(node('p', error.message, 'ppt-player-empty'));
            });
            return;
        }
        const totals = cached.totals || {};
        const pps = cached.pps || {};
        // 左半分: 数字（APM はあとで追加）
        const stats = node('div', null, 'ppt-player-stats');
        const stat = (label, value) => {
            const box = node('div', null, 'ppt-player-stat');
            box.append(node('span', label), node('strong', value));
            stats.append(box);
        };
        // 1行目に試合数、その下は左の列に勝率・勝敗、右の列に PPS・APM
        stat('試合', String(totals.matches || 0));
        stats.lastChild.classList.add('ppt-player-stat-wide');
        stat('勝率', percentText(totals.winRate));
        // 数えた手が少ない（200 手未満）ときは参考値として括弧で囲む
        const ppsText = pps.value ? pps.value.toFixed(2) : '—';
        stat('PPS', pps.value && pps.reliable === false ? '(' + ppsText + ')' : ppsText);
        if (pps.value) {
            stats.lastChild.title = '数えた手 ' + (pps.pieces || 0) + ' 手'
                + '（開幕30手・ライン消去・3秒超の待ちを除く）'
                + (pps.reliable === false ? '。' + (pps.minPieces || 200) + ' 手未満なので参考値' : '');
        }
        stat('勝敗', (totals.wins || 0) + ' - ' + (totals.losses || 0) + (totals.draws ? ' - ' + totals.draws : ''));
        stat('APM', '—');
        left.append(stats);

        // 右半分: 上に相手ごとの戦績（スクロール）、下にテンプレの円グラフ
        const right = node('div', null, 'ppt-player-right');
        const versus = node('div', null, 'ppt-player-versus');
        versus.append(node('h4', '戦績'));
        const scroll = node('div', null, 'ppt-player-scroll');
        const table = node('table', null, 'ppt-player-table');
        const head = node('tr');
        ['相手', '試合', '勝-負', '勝率'].forEach(text => head.append(node('th', text)));
        table.append(head);
        (cached.opponents || []).forEach(row => {
            const line = node('tr');
            const who = node('td');
            if (row.name && !row.name.startsWith('（')) who.append(tagLink(autoTagKey('player', row.name)));
            else who.textContent = row.name || '';
            const rate = node('td', percentText(row.winRate));
            if (row.winRate !== null && row.winRate !== undefined) {
                const bar = node('span', null, 'ppt-player-bar');
                const fill = node('i');
                fill.style.width = Math.round(row.winRate * 100) + '%';
                bar.append(fill);
                rate.append(bar);
            }
            line.append(who, node('td', String(row.matches)), node('td', row.wins + '-' + row.losses), rate);
            table.append(line);
        });
        scroll.append(table);
        versus.append(scroll);

        const templates = node('div', null, 'ppt-player-templates');
        templates.append(node('h4', 'テンプレ'));
        const pie = node('div', null, 'ppt-player-pie');
        const raw = (cached.openings || []).map(item => ({ name: item.name, count: Number(item.count) || 0, color: item.color }))
            .filter(item => item.count > 0);
        const rawTotal = raw.reduce((sum, item) => sum + item.count, 0) || 1;
        // 0% になるものと未登録は「その他」（灰色）にまとめて最後に置く
        const other = { name: 'その他', count: 0, color: '#b8b8b8' };
        const items = [];
        raw.forEach(item => {
            if (item.name === '未登録' || item.name === 'その他' || Math.round(item.count * 100 / rawTotal) === 0) other.count += item.count;
            else items.push(item);
        });
        if (other.count) items.push(other);
        pie.append(templatePie(items));
        if (items.length) {
            const total = items.reduce((sum, item) => sum + item.count, 0);
            const legend = node('ul', null, 'ppt-player-legend');
            items.forEach((item, index) => {
                const entry = node('li');
                const swatch = node('i');
                swatch.style.background = item.color || PIE_COLORS[index % PIE_COLORS.length];
                entry.append(swatch, document.createTextNode(item.name + ' ' + Math.round(item.count / total * 100) + '%'));
                legend.append(entry);
            });
            pie.append(legend);
        }
        templates.append(pie);
        right.append(versus, templates);
        panel.append(right);
    }

    // ---- 設定 -------------------------------------------------------------
    const SETTING_DEFAULTS = {
        theme: 'light',
        simulatorBase: 'https://selmtoe.github.io/Tetris_Simulator/',
        openLinks: 'newTab',
        analysisWorkers: '1',
        downloadVideo: false
    };

    // 今の試合・今の局面をシミュレータで開く（キーボードの S）
    async function openSimulatorHere() {
        if (!state.currentMatch) return;
        try {
            const urls = await replayUrls(state.currentMatch.index);
            const url = simulatorLink(urls.combined, (state.pageIndex || 0) + 1);
            if (!url) {
                toast('この試合はシミュレータで開けません（盤面データがありません）。');
                return;
            }
            // 設定が「このウィンドウ内」なら、Lab の上に重ねて開く
            if (opensInLab()) openInlineFrame(url, { title: 'シミュレータ' });
            else window.open(url, '_blank', 'noopener');
        } catch (error) {
            toast(error.message);
        }
    }

    // 再生速度を、ローカル動画・YouTube のどちらにも反映する
    function applyPlaybackRate() {
        const rate = Number(state.playbackRate) || 1;
        const media = document.getElementById('detail-video');
        if (media) media.playbackRate = rate;
        if (state.ytReady && state.ytPlayer?.setPlaybackRate) {
            try { state.ytPlayer.setPlaybackRate(rate); } catch (error) { /* 速度を変えられなくても再生はできる */ }
        }
    }

    function settingOn(key) {
        const value = setting(key);
        return !(value === false || value === 'false' || value === 0 || value === '0');
    }

    function setting(key) {
        const value = state.settings ? state.settings[key] : undefined;
        if (value !== undefined && value !== null && value !== '') return value;
        return SETTING_DEFAULTS[key] ?? '';
    }

    async function loadSettings() {
        try {
            const body = await api('/api/settings');
            state.settings = Object.assign({}, SETTING_DEFAULTS, body.settings || {});
        } catch (error) {
            state.settings = Object.assign({}, SETTING_DEFAULTS);
        }
        applyTheme();
    }

    function applyTheme() {
        document.documentElement.dataset.theme = setting('theme') === 'dark' ? 'dark' : 'light';
    }

    async function saveSetting(key, value) {
        const previous = state.settings ? state.settings[key] : undefined;
        state.settings = Object.assign({}, state.settings, { [key]: value });
        if (key === 'theme') applyTheme();
        try {
            await api('/api/settings', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ [key]: value })
            });
            notify('設定を保存しました。');
        } catch (error) {
            state.settings = Object.assign({}, state.settings, { [key]: previous });
            if (key === 'theme') applyTheme();
            toast(error.message);
        }
    }

    // 「テンプレ判定」は判定エンジン（名前を付けた開幕の形）を作る人向け。閲覧専用版では出さない
    const SETTINGS_TABS = document.documentElement.dataset.localSaves === 'true'
        ? ['表示', '自分のデータ']
        : document.documentElement.dataset.readonly === 'true'
            ? ['一般', '表示', '解析', '名前とタグ', 'データ管理']
            : ['一般', '表示', '解析', '名前とタグ', 'データ管理', 'テンプレ判定'];

    function settingsTab() {
        return state.settingsTab && SETTINGS_TABS.includes(state.settingsTab) ? state.settingsTab : SETTINGS_TABS[0];
    }

    function ramDiskText(info) {
        if (!info) return '';
        const gb = value => ((Number(value) || 0) / 1024).toFixed(1);
        if (info.available) {
            return (info.drive || 'RAM') + ' 使用中（空き ' + gb(info.freeMB) + 'GB / ' + gb(info.totalMB) + 'GB）';
        }
        if (info.busy) return '作成しています…（管理者の確認が出たら「はい」を選んでください）';
        if (info.error) return info.error;
        return '未作成 — オンにすると管理者の確認（UAC）が1回出ます';
    }

    function watchRamDisk(row) {
        const status = node('span', '', 'ppt-setting-note');
        row.append(status);
        let timer = null;
        const tick = async () => {
            if (!status.isConnected) {
                if (timer) clearInterval(timer);
                return;
            }
            try {
                status.textContent = ramDiskText(await api('/api/ramdisk'));
            } catch (error) {
                // 表示だけの情報なので、取れなくても何もしない
            }
        };
        tick();
        timer = setInterval(tick, 2000);
    }

    function settingControl(item) {
        const value = setting(item.key);
        if (item.type === 'bool') {
            const box = node('input');
            box.type = 'checkbox';
            box.checked = Boolean(value);
            box.addEventListener('change', () => saveSetting(item.key, box.checked));
            return box;
        }
        if (item.options && item.options.length) {
            const select = node('select');
            item.options.forEach(option => {
                const element = node('option', option[1]);
                element.value = String(option[0]);
                select.append(element);
            });
            select.value = String(value);
            select.addEventListener('change', async () => {
                const next = item.type === 'int' ? Number(select.value) : select.value;
                await saveSetting(item.key, next);
                // 「このウィンドウ内」を選んだ直後に、どう変わるかを伝える
                if (item.key === 'openLinks' && String(setting('openLinks')) === String(next)) {
                    notify(next === 'sameWindow'
                        ? 'これからは Lab の中で開きます（画面共有でも見えます）。'
                        : 'これからは新しいタブで開きます。');
                }
            });
            return select;
        }
        const input = node('input');
        input.type = 'text';
        input.value = value == null ? '' : String(value);
        input.placeholder = '（既定）';
        input.addEventListener('change', () => saveSetting(item.key, input.value.trim()));
        return input;
    }

    // ---- 解析モデル（インストール済みモデルの一覧と追加・削除） -------------
    // 旧プロファイル名（standard / fast320）は Tetrace-V1.0-PPT と同じ扱い。
    const LEGACY_MODEL_IDS = ['standard', 'fast320'];

    async function loadModels() {
        try {
            const body = await api('/api/models');
            state.models = Array.isArray(body.models) ? body.models : [];
            state.modelsActive = String(body.active || '');
            state.modelsError = '';
        } catch (error) {
            // 旧版のサーバーで起動中（/api/models が無い）ときはここに来る。
            state.models = [];
            state.modelsActive = '';
            state.modelsError = error.message || 'モデル一覧を取得できませんでした';
        }
        return state.models;
    }

    function formatModelSize(bytes) {
        const value = Number(bytes) || 0;
        if (value >= 1024 * 1024) return (value / (1024 * 1024)).toFixed(1) + 'MB';
        if (value >= 1024) return Math.round(value / 1024) + 'KB';
        return value + 'B';
    }

    function modelIdForSelect() {
        const current = String(setting('analysisProfile') || '');
        const known = (state.models || []).map(model => model.id);
        // 一覧が取れないときは保存されている値をそのまま使う（旧版サーバー対策）。
        if (!known.length) return current;
        if (known.indexOf(current) >= 0) return current;
        if (current === 'other-game') return current;
        if (LEGACY_MODEL_IDS.indexOf(current) >= 0 && known.indexOf('tetrace-v1.0-ppt') >= 0) {
            return 'tetrace-v1.0-ppt';
        }
        if (state.modelsActive && known.indexOf(state.modelsActive) >= 0) return state.modelsActive;
        return String(known[0]);
    }

    // 解析モデルの選択。選択肢はサーバーの一覧（/api/models）から作る。
    function modelSettingControl(item) {
        const wrapper = node('div', null, 'ppt-model-controls');
        const select = node('select');
        const models = state.models || [];
        // 一覧が取れないときは、設定スキーマの選択肢で描く（画面を壊さない）。
        const entries = models.length
            ? models.map(model => [String(model.id), model.name])
            : (item.options || []).filter(option => String(option[0]) !== 'other-game');
        if (!entries.length) entries.push(['', 'インストール済みのモデルがありません']);
        entries.forEach(entry => {
            const option = node('option', entry[1]);
            option.value = String(entry[0]);
            select.append(option);
        });
        const values = Array.from(select.options).map(option => option.value);
        const preferred = modelIdForSelect();
        select.value = values.indexOf(preferred) >= 0 ? preferred : (values[0] || '');
        select.addEventListener('change', () => saveSetting('analysisProfile', select.value));
        wrapper.append(select);

        const addButton = node('button', 'モデルを追加', 'btn-outline');
        addButton.type = 'button';
        addButton.disabled = Boolean(state.modelsError);
        const picker = node('input');
        picker.type = 'file';
        picker.accept = '.tetracemodel,.zip';
        picker.hidden = true;
        addButton.addEventListener('click', () => picker.click());
        picker.addEventListener('change', () => {
            const file = picker.files && picker.files[0];
            picker.value = '';
            if (file) uploadModel(file);
        });
        wrapper.append(addButton);
        wrapper.append(picker);
        if (state.modelsError) {
            wrapper.append(node('span', 'モデル一覧を取得できませんでした（Lab を再起動してください）。',
                'ppt-setting-note'));
        }
        return wrapper;
    }

    // インストール済みモデルの一覧（組み込み以外は削除できる）。
    function modelManager() {
        const list = node('div', null, 'ppt-model-list');
        const models = state.models || [];
        if (!models.length) {
            list.append(node('span', state.modelsError
                ? 'モデル一覧を取得できませんでした（Lab を再起動してください）。'
                : 'インストール済みのモデルはありません。', 'ppt-setting-note'));
            return list;
        }
        models.forEach(model => {
            const row = node('div', null, 'ppt-model-item');
            const version = model.version ? ' (v' + model.version + ')' : '';
            row.append(node('span', model.name + version + ' — ' + formatModelSize(model.size),
                'ppt-setting-value'));
            if (model.builtin) {
                row.append(node('span', '組み込み', 'ppt-setting-note'));
            } else {
                const remove = node('button', '削除', 'btn-outline');
                remove.type = 'button';
                remove.addEventListener('click', () => removeModel(model));
                row.append(remove);
            }
            list.append(row);
        });
        return list;
    }

    // 選んだ .tetracemodel（ZIP）をそのままサーバーへ送る。
    async function uploadModel(file) {
        try {
            const response = await fetch('/api/models/install', {
                method: 'POST',
                headers: { 'Content-Type': 'application/zip' },
                body: file
            });
            let body = null;
            try {
                body = await response.json();
            } catch (error) {
                body = null;
            }
            if (!response.ok) {
                throw new Error((body && body.error) || 'モデルを追加できませんでした');
            }
            const name = (body && body.name) || file.name;
            notify(name + ' を追加しました。');
            if (body && body.warnings && body.warnings.length) {
                notify('注意: ' + body.warnings.join(' / '));
            }
            state.models = [];
            await loadModels();
            renderSettings();
        } catch (error) {
            toast('モデルを追加できませんでした: ' + error.message);
        }
    }

    async function removeModel(model) {
        if (!window.confirm('「' + model.name + '」を削除します。よろしいですか？')) return;
        try {
            await api('/api/models/delete', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ id: model.id })
            });
            notify('「' + model.name + '」を削除しました。');
            state.models = [];
            await loadSettings();
            await loadModels();
            renderSettings();
        } catch (error) {
            toast(error.message);
        }
    }

    function renderSettings() {
        const root = node('section', null, 'lab-video-library');
        mainView.replaceChildren(root);
        const header = node('div', null, 'lab-video-library-heading');
        root.append(header);

        const tabs = node('div', null, 'lab-video-tabs');
        SETTINGS_TABS.forEach(name => {
            const button = node('button', name, 'btn-outline');
            button.type = 'button';
            button.setAttribute('aria-pressed', String(name === settingsTab()));
            button.addEventListener('click', () => { state.settingsTab = name; renderSettings(); });
            tabs.append(button);
        });
        root.append(tabs);

        const body = node('div', null, 'ppt-settings-body');
        root.append(body);

        if (settingsTab() === 'データ管理') {
            renderDataTab(body);
            return;
        }
        if (settingsTab() === '自分のデータ') {
            renderViewerDataTab(body);
            return;
        }
        if (settingsTab() === 'テンプレ判定') {
            body.append(node('p', '読み込み中…', 'lab-help-text'));
            loadOpenings().then(() => {
                if (state.view === 'settings' && settingsTab() === 'テンプレ判定') renderOpenings(body);
            }).catch(error => toast(error.message));
            return;
        }

        (async () => {
            let schema = state.settingsSchema || [];
            if (!schema.length) {
                try {
                    const result = await api('/api/settings/schema');
                    schema = result.schema || [];
                    state.settingsSchema = schema;
                } catch (error) {
                    schema = [];
                }
            }
            const group = settingsTab();
            if (group === '解析' && !(state.models || []).length) await loadModels();
            schema.filter(item => item.group === group).forEach(item => {
                if (item.key === 'analysisProfile') {
                    // 「解析モデル」だけは、選択肢をサーバーの一覧（/api/models）から作り、
                    // 追加ボタンと削除ボタンを添える。
                    const modelRow = node('div', null, 'ppt-setting-row');
                    modelRow.append(node('span', item.label, 'ppt-setting-label'));
                    modelRow.append(modelSettingControl(item));
                    if (item.note) modelRow.append(node('span', item.note, 'ppt-setting-note'));
                    body.append(modelRow);
                    body.append(modelManager());
                    return;
                }
                const row = node('label', null, 'ppt-setting-row');
                row.append(node('span', item.label, 'ppt-setting-label'));
                row.append(settingControl(item));
                if (item.note) row.append(node('span', item.note, 'ppt-setting-note'));
                if (item.key === 'useRamDisk') watchRamDisk(row);
                body.append(row);
            });
        })();
    }

    function renderDataTab(body) {
        const card = (title) => {
            const box = node('div', null, 'card ppt-setting-card');
            box.append(node('h3', title, 'card-title'));
            body.append(box);
            return box;
        };

        // --- GitHub Pages（公開Lab）と同期（デスクトップ版だけ）---
        const publish = card('GitHub Pages と同期');
        publish.append(node('p', '解析データとテンプレ判定のうち、変わったものを公開Lab（GitHub Pages）に反映します。'
            + '載せない配信は tools/ppt-lab/private-streams.json に書きます。', 'lab-help-text'));
        const publishButton = node('button', 'GitHub Pages と同期', 'btn-outline');
        publishButton.type = 'button';
        const publishStatus = node('p', '', 'ppt-setting-value');
        const publishLog = node('pre', '', 'ppt-publish-log');
        publish.append(publishButton, publishStatus, publishLog);
        const showPublish = state => {
            publishButton.disabled = Boolean(state.running);
            publishButton.textContent = state.running ? '同期中…' : 'GitHub Pages と同期';
            publishStatus.textContent = state.running
                ? (state.step ? state.step + '…' : '準備中…')
                : (state.message || (state.finishedAt ? '' : 'まだ同期していません。'))
                    + (state.finishedAt && !state.running ? '（' + String(state.finishedAt).replace('T', ' ').slice(0, 16) + '）' : '');
            publishStatus.dataset.ok = state.ok === false ? 'false' : 'true';
            publishLog.textContent = (state.log || []).join('\n');
            publishLog.hidden = !(state.log || []).length;
        };
        const pollPublish = async () => {
            if (!publish.isConnected) return;
            try {
                const state = await api('/api/publish');
                showPublish(state);
                if (state.running) setTimeout(pollPublish, 1500);
                else if (state.ok === true && state.commit) notify('GitHub Pages と同期しました。');
                else if (state.ok === false) toast('同期に失敗しました：' + state.message);
            } catch (error) {
                publishStatus.textContent = error.message;
            }
        };
        publishButton.addEventListener('click', async () => {
            try {
                showPublish(await api('/api/publish', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' }));
                setTimeout(pollPublish, 1000);
            } catch (error) {
                toast(error.message);
            }
        });
        api('/api/publish').then(state => { showPublish(state); if (state.running) setTimeout(pollPublish, 1500); })
            .catch(() => {});

        // --- プレイヤーネームの対応 ---
        const names = card('プレイヤーネームの対応');
        const search = node('input');
        search.type = 'search';
        search.placeholder = '名前で検索';
        search.addEventListener('input', () => renderRows(search.value.trim()));
        names.append(search);
        const table = node('table', null, 'ppt-name-table ppt-mapping-table');
        const head = node('thead');
        const headRow = node('tr');
        ['画像', '名前', '使用', '更新', ''].forEach(label => headRow.append(node('th', label)));
        head.append(headRow);
        table.append(head);
        const tbody = node('tbody');
        table.append(tbody);
        names.append(table);

        let mappings = [];
        const renderRows = (query) => {
            tbody.replaceChildren();
            const filtered = mappings.filter(item => !query || String(item.name).includes(query));
            if (!filtered.length) {
                const row = node('tr');
                const cell = node('td', '対応がありません。', 'lab-help-text');
                cell.colSpan = 5;
                row.append(cell);
                tbody.append(row);
                return;
            }
            filtered.forEach(item => {
                const row = node('tr');
                const imageCell = node('td');
                if (item.image) {
                    const image = node('img', null, 'ppt-mapping-image');
                    image.src = item.image;
                    image.alt = '';
                    image.loading = 'lazy';
                    imageCell.append(image);
                } else {
                    imageCell.append(node('span', '—', 'ppt-muted'));
                }
                row.append(imageCell);
                row.append(node('td', item.name));
                row.append(node('td', String(item.hits || 0) + ' 回 / ' + (item.fingerprints ? item.fingerprints.length : 1) + ' 画像'));
                row.append(node('td', String(item.updatedAt || '').slice(0, 16).replace('T', ' ')));
                const actions = node('td', null, 'ppt-row-actions');
                const rename = node('button', '名前を変更', 'btn-outline');
                rename.type = 'button';
                rename.addEventListener('click', async () => {
                    const next = window.prompt('新しい名前', item.name);
                    if (!next || next === item.name) return;
                    try {
                        await api('/api/names/rename', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({ from: item.name, to: next }) });
                        notify('名前を変更しました。');
                        load();
                    } catch (error) { toast(error.message); }
                });
                const merge = node('button', '統合', 'btn-outline');
                merge.type = 'button';
                merge.addEventListener('click', async () => {
                    const target = window.prompt('統合先の名前', item.name);
                    if (!target || target === item.name) return;
                    try {
                        await api('/api/names/merge', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({ from: item.name, to: target }) });
                        notify('統合しました。');
                        load();
                    } catch (error) { toast(error.message); }
                });
                const unlink = node('button', '解除', 'btn-outline');
                unlink.type = 'button';
                unlink.addEventListener('click', async () => {
                    if (!window.confirm('「' + item.name + '」の紐づけを解除しますか？')) return;
                    try {
                        await api('/api/names/mappings', { method: 'DELETE', headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({ name: item.name }) });
                        notify('紐づけを解除しました。');
                        load();
                    } catch (error) { toast(error.message); }
                });
                actions.append(rename, merge, unlink);
                row.append(actions);
                tbody.append(row);
            });
        };
        const load = async () => {
            try {
                const result = await api('/api/names/mappings');
                mappings = result.mappings || [];
            } catch (error) {
                mappings = [];
            }
            renderRows(search.value.trim());
        };
        load();

        // --- 動画の保存 ---
        const storage = card('動画の保存');
        (async () => {
            let schema = state.settingsSchema || [];
            if (!schema.length) {
                try {
                    const result = await api('/api/settings/schema');
                    schema = result.schema || [];
                    state.settingsSchema = schema;
                } catch (error) { schema = []; }
            }
            schema.filter(item => item.group === 'データ管理').forEach(item => {
                const row = node('label', null, 'ppt-setting-row');
                row.append(node('span', item.label, 'ppt-setting-label'));
                row.append(settingControl(item));
                storage.append(row);
            });
            const info = await api('/api/data/info').catch(() => null);
            if (info) {
                const row = node('div', null, 'ppt-setting-row');
                row.append(node('span', '動画の合計サイズ', 'ppt-setting-label'));
                row.append(node('span', (info.mediaSize / 1024 / 1024 / 1024).toFixed(1) + ' GB', 'ppt-setting-value'));
                storage.append(row);
            }
        })();

        // --- キャッシュ ---
        const cache = card('キャッシュ');
        const cacheRow = (label, kind, sizeText) => {
            const row = node('div', null, 'ppt-setting-row');
            row.append(node('span', label, 'ppt-setting-label'));
            row.append(node('span', sizeText, 'ppt-setting-value'));
            const button = node('button', '削除', 'btn-outline');
            button.type = 'button';
            button.addEventListener('click', async () => {
                if (!window.confirm(label + 'を削除しますか？')) return;
                try {
                    const result = await api('/api/data/cache', { method: 'DELETE', headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ kind }) });
                    notify((result.freed / 1024 / 1024).toFixed(1) + ' MB 削除しました。');
                    renderSettings();
                } catch (error) { toast(error.message); }
            });
            row.append(button);
            cache.append(row);
        };
        (async () => {
            const info = await api('/api/data/info').catch(() => null);
            cacheRow('名前画像', 'crops', info ? (info.cropSize / 1024).toFixed(0) + ' KB' : '—');
            cacheRow('解析キャッシュ', 'analysis', info ? (info.analysisCacheSize / 1024 / 1024).toFixed(1) + ' MB（' + info.analysisCacheCount + '件）' : '—');
        })();

        // --- このアプリについて ---
        const about = card('このアプリについて');
        (async () => {
            const info = await api('/api/data/info').catch(() => null);
            if (!info) return;
            const line = (label, value) => {
                const row = node('div', null, 'ppt-setting-row');
                row.append(node('span', label, 'ppt-setting-label'));
                row.append(node('span', value, 'ppt-setting-value'));
                about.append(row);
            };
            line('バージョン', info.version);
            line('データの場所', info.dataDir);
            line('DBサイズ', (info.dbSize / 1024 / 1024).toFixed(1) + ' MB');
            line('動画 / 試合', info.videos + '本 / ' + info.matches + '試合');
        })();
    }

    function render() {
        document.querySelectorAll('.nav-item[data-view]').forEach(link => {
            const active = link.dataset.view === state.view;
            link.classList.toggle('active', active);
            if (active) link.setAttribute('aria-current', 'page');
            else link.removeAttribute('aria-current');
        });
        if (state.view !== 'video') {
            stopSync();
            stopFeature();
        }
        if (state.view === 'video') renderVideoDetail();
        else if (state.view === 'names') renderNames();
        else if (state.view === 'tetofu') renderTetofu();
        else if (state.view === 'tags') renderTags();
        else if (state.view === 'settings') {
            const parts = String(location.hash || '').replace(/^#/, '').split('/');
            if (parts[1]) state.settingsTab = decodeURIComponent(parts[1]);
            renderSettings();
        }
        else renderVideos();
        try { localStorage.setItem('pptLastView', String(location.hash || '#videos')); } catch (error) { /* 保存できなくてもよい */ }
        syncPolling();
    }

    // ---- タグ付け ---------------------------------------------------------
    // タグ編集（動画・テト譜の両方で使う）
    async function openTagDialog(target, options) {
        // 既存タグを選べるように、開く前に読み込んでおく
        if (!state.tags.length) {
            try { await loadTags(); } catch (error) { /* 取れなくても続行 */ }
        }
        const dialog = document.getElementById('tag-dialog');
        const opts = options || {};
        const deleting = opts.mode === 'delete';
        const picker = document.getElementById('tag-picker');
        const note = document.getElementById('tag-dialog-note');
        const fresh = document.getElementById('tag-new');
        note.textContent = deleting
            ? '削除するタグを選んでください（すべての動画・テト譜から外れます）'
            : (target.title || '');
        fresh.value = '';
        const chosen = new Set(target.tags || []);
        const freshRow = fresh.parentElement;
        if (freshRow) freshRow.hidden = deleting;
        const saveButton = document.getElementById('tag-save');
        if (opts.onSave) saveButton.textContent = '保存する';
        if (saveButton) saveButton.textContent = deleting ? '閉じる' : '保存する';
        // 配信者・プレイヤーのタグは自動で付くので、ここには出さない（同じ名前の通常タグを付けられる）
        const auto = new Set();
        picker.replaceChildren();
        // 既存タグ＋この対象が今持っているタグ（プレイヤー名など）をまとめて出す
        const names = new Map();
        state.tags.forEach(tag => names.set(tag.name, tag.count || 0));
        chosen.forEach(name => { if (!names.has(name)) names.set(name, 0); });
        Array.from(names.keys()).sort().forEach(name => {
            const chip = node('button', name, 'tag tag-kind-' + tagKind(name) + (chosen.has(name) ? ' active' : ''));
            chip.type = 'button';
            if (auto.has(name) && !deleting) {
                // 自動で付くタグ（動画解析・プレイヤー名）は外せない
                chip.classList.add('ppt-tag-locked');
                chip.title = '自動で付くタグ（変更できません）';
                chip.disabled = true;
                chip.textContent = name + ' · 自動';
                picker.append(chip);
                return;
            }
            chip.title = auto.has(name) ? '自動で付いたタグ' : '';
            if (deleting && auto.has(name)) {
                chip.classList.add('ppt-tag-locked');
                chip.textContent = name + ' · 自動';
                chip.title = '自動で付くタグは削除できません';
                chip.disabled = true;
                picker.append(chip);
                return;
            }
            if (deleting) {
                chip.addEventListener('click', async () => {
                    if (!window.confirm('「' + name + '」をすべての動画・テト譜から削除しますか？')) return;
                    try {
                        await api('/api/tags', {
                            method: 'DELETE',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({ name })
                        });
                        dialog.close();
                        await loadTags();
                        await loadVideos();
                        await loadTetofu();
                        if (state.view === 'video') renderVideoDetail();
                        else render();
                        toast('「' + name + '」を削除しました。');
                    } catch (error) {
                        toast(error.message);
                    }
                });
                picker.append(chip);
                return;
            }
            chip.addEventListener('click', () => {
                if (chosen.has(name)) chosen.delete(name);
                else chosen.add(name);
                chip.classList.toggle('active', chosen.has(name));
            });
            picker.append(chip);
        });
        if (!names.size) picker.append(node('span', 'タグがまだありません。下の欄で追加できます。', 'ppt-muted'));
        dialog.showModal();
        const save = document.getElementById('tag-save');
        save.onclick = async event => {
            if (opts.onSave) {
                event.preventDefault();
                const picked = Array.from(chosen);
                fresh.value.split(',').map(value => value.trim()).filter(Boolean).forEach(name => picked.push(name));
                try {
                    await opts.onSave(Array.from(new Set(picked)));
                    dialog.close();
                } catch (error) {
                    toast(error.message);
                }
                return;
            }
            event.preventDefault();
            if (deleting && auto.has(name)) {
                chip.classList.add('ppt-tag-locked');
                chip.textContent = name + ' · 自動';
                chip.title = '自動で付くタグは削除できません';
                chip.disabled = true;
                picker.append(chip);
                return;
            }
            if (deleting) {
                dialog.close();
                return;
            }
            const extra = fresh.value.split(',').map(value => value.trim()).filter(Boolean);
            extra.forEach(name => chosen.add(name));
            try {
                await api('/api/tags/apply', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ target: target.id, tags: Array.from(chosen) })
                });
                dialog.close();
                await loadTags();
                if (target.kind === 'video') {
                    await loadVideos();
                    renderVideoDetail();
                } else {
                    await loadTetofu();
                    render();
                }
                toast('タグを保存しました。');
            } catch (error) {
                toast(error.message);
            }
        };
    }
    // ---- 動画詳細（再生と試合一覧） ---------------------------------------
    function seekPlayer(seconds) {
        const player = document.getElementById('detail-video');
        if (player) {
            try {
                player.currentTime = Math.max(0, seconds);
                player.play?.();
            } catch (error) {
                /* シークできない形式でも一覧は使える */
            }
            return;
        }
        const frame = document.getElementById('detail-frame');
        if (frame) {
            const base = frame.dataset.base || frame.src.split('&start=')[0].split('?start=')[0];
            frame.dataset.base = base;
            frame.src = base + (base.includes('?') ? '&' : '?') + 'start=' + Math.round(seconds) + '&autoplay=1';
        }
    }

    // 画面は <サイト>/ppt-lab/ に置き、盤面ビューワー（F/）とシミュレータは
    // 同じサイトのものを使う。ローカル版はネットに取りに行かず、
    // リポジトリ直下の本体をそのまま開く。
    // ブラウザ版（公開Lab）では window.PPT_SITE_BASE でシミュレータ・ビューワーのあるサイトを指す
    const SITE_BASE = new URL(window.PPT_SITE_BASE || '../', location.href).href;
    const SIMULATOR_BASE = SITE_BASE;
    const VIEWER_BASE = SITE_BASE + 'F/';
    const PUBLIC_BASE = 'https://selmtoe.github.io/Tetris_Simulator/';
    // ビューワーとは postMessage でやり取りする（?labViewer=1 で有効）
    // 応答が無い環境では自動で URL 再読み込み方式に切り替える。
    const VIEWER_ORIGIN = location.origin;
    const VIEWER_MESSAGE = 'labViewerRequest';

    // 保存してある公開URLを、いま開いているサイトの同じ場所へ置き換える
    function siteUrl(url) {
        const value = String(url || '');
        if (!value || !value.startsWith(PUBLIC_BASE)) return value;
        return SITE_BASE + value.slice(PUBLIC_BASE.length);
    }

    // ---- Lab の中で開く（設定「リンクの開き方」= このウィンドウ内） --------
    // 画面共有で Lab だけを見せていると、シミュレータやビューワーが別タブ・
    // 別ウィンドウで開いたときに共有相手に見えない。そういうときは同じ
    // ウィンドウの上に固定パネルを重ね、その中の iframe で開く。
    // 盤面ビューワー（#detail-viewer）はこれまでどおりページ内に置いたまま。
    const INLINE_BLOCKED_MESSAGE =
        'このページはLabの中では開けません。新しいタブで開いてください。';
    const INLINE_OUTSIDE_HINT =
        'Lab の外のページです。表示されないときは「新しいタブで開く」から開いてください。';
    // iframe の読み込みを待つ上限（過ぎたら「開けない」案内に切り替える）
    const INLINE_LOAD_TIMEOUT = 12000;

    // 設定が「このウィンドウ内」か（既定は新しいタブ）
    function opensInLab() {
        return setting('openLinks') === 'sameWindow';
    }

    // iframe に渡す URL を決める。公開サイト（PUBLIC_BASE）の URL は、
    // いま開いているサイトの同じ場所（SITE_BASE）へ置き換える。
    // 同一オリジンのページは X-Frame-Options に邪魔されず、中身も確認できる。
    function inlineUrl(url) {
        const value = String(url || '').trim();
        if (!value || value === '#') return { src: '', sameOrigin: false };
        let src = value;
        if (value.startsWith(PUBLIC_BASE)) src = SITE_BASE + value.slice(PUBLIC_BASE.length);
        let sameOrigin = false;
        try {
            const parsed = new URL(src, location.href);
            sameOrigin = parsed.origin === location.origin;
            src = parsed.href;
        } catch (error) {
            sameOrigin = false;
        }
        return { src, sameOrigin };
    }

    // 「このページは開けません」の案内を出す（iframe の上に重ねる）
    function showInlineBlocked(overlay, message) {
        const blocked = overlay.querySelector('.ppt-inline-blocked');
        if (!blocked) return;
        const text = blocked.querySelector('.ppt-inline-blocked-text');
        if (text) text.textContent = message;
        blocked.hidden = false;
    }

    // オーバーレイは1枚だけ作って使い回す（毎回作り直すと iframe が増える）
    function inlineOverlay() {
        let overlay = document.getElementById('ppt-inline-frame');
        if (overlay) return overlay;

        overlay = node('div', null, 'ppt-inline-frame');
        overlay.id = 'ppt-inline-frame';
        overlay.hidden = true;
        overlay.setAttribute('role', 'dialog');
        overlay.setAttribute('aria-modal', 'true');
        overlay.setAttribute('aria-labelledby', 'ppt-inline-title');

        const panel = node('div', null, 'ppt-inline-panel');

        // ヘッダー: タイトル / URL / 新しいタブで開く / 閉じる
        const head = node('div', null, 'ppt-inline-head');
        const heading = node('div', null, 'ppt-inline-heading');
        const title = node('h2', '', 'ppt-inline-title');
        title.id = 'ppt-inline-title';
        const urlLine = node('p', '', 'ppt-inline-url');
        heading.append(title, urlLine);
        head.append(heading);

        const actions = node('div', null, 'ppt-inline-actions');
        const hint = node('span', INLINE_OUTSIDE_HINT, 'ppt-inline-hint');
        hint.hidden = true;
        const external = node('a', '新しいタブで開く', 'btn-outline ppt-inline-external');
        external.target = '_blank';
        external.rel = 'noopener';
        const close = node('button', '閉じる', 'btn-outline ppt-inline-close');
        close.type = 'button';
        actions.append(hint, external, close);
        head.append(actions);
        panel.append(head);

        // 本体: iframe と、開けなかったときの案内
        const body = node('div', null, 'ppt-inline-body');
        const frame = node('iframe', null, 'ppt-inline-iframe');
        // シミュレータの再生（全画面・自動再生）をそのまま使えるようにする
        frame.setAttribute('allow', 'fullscreen; autoplay');
        frame.setAttribute('referrerpolicy', 'no-referrer-when-downgrade');
        const blocked = node('div', null, 'ppt-inline-blocked');
        blocked.hidden = true;
        const blockedText = node('p', INLINE_BLOCKED_MESSAGE, 'ppt-inline-blocked-text');
        const blockedLink = node('a', '新しいタブで開く', 'btn-outline');
        blockedLink.target = '_blank';
        blockedLink.rel = 'noopener';
        blocked.append(blockedText, blockedLink);
        body.append(frame, blocked);
        panel.append(body);

        overlay.append(panel);
        document.body.append(overlay);

        // 背景（パネルの外）のクリックでは閉じない。誤って閉じるのを防ぐ。
        close.addEventListener('click', () => closeInlineFrame());

        frame.addEventListener('load', () => {
            if (!overlay.__open) return;
            clearTimeout(overlay.__timer);
            overlay.__timer = null;
            // 待ち時間切れで案内を出していても、実際に読み込めたら引っ込める
            const blocked = overlay.querySelector('.ppt-inline-blocked');
            if (blocked) blocked.hidden = true;
            // 別オリジンのページは中身を覗けない（X-Frame-Options で表示を
            // 断られても検知できない）。その場合はヘッダーの案内に任せる。
            if (!overlay.__sameOrigin) return;
            let doc = null;
            try { doc = frame.contentDocument; } catch (error) { doc = null; }
            if (!doc || !doc.body) showInlineBlocked(overlay, INLINE_BLOCKED_MESSAGE);
        });

        return overlay;
    }

    // Lab の上に重ねて開く（設定が「このウィンドウ内」のときだけ呼ぶ）
    function openInlineFrame(rawUrl, options) {
        const opts = options || {};
        const resolved = inlineUrl(rawUrl);
        if (!resolved.src) {
            toast('開けるリンクがありません。');
            return false;
        }
        const overlay = inlineOverlay();
        const frame = overlay.querySelector('.ppt-inline-iframe');
        const title = opts.title || 'Lab の中で開いています';

        overlay.querySelector('.ppt-inline-title').textContent = title;
        const urlLine = overlay.querySelector('.ppt-inline-url');
        urlLine.textContent = resolved.src;
        urlLine.title = resolved.src;
        overlay.querySelector('.ppt-inline-external').href = resolved.src;
        overlay.querySelector('.ppt-inline-blocked a').href = resolved.src;
        overlay.querySelector('.ppt-inline-blocked').hidden = true;
        // 同一オリジンに無いページは、表示できないときに自分で気づけるようにする
        overlay.querySelector('.ppt-inline-hint').hidden = resolved.sameOrigin;
        frame.title = title;

        overlay.__sameOrigin = resolved.sameOrigin;
        overlay.__open = true;
        clearTimeout(overlay.__timer);
        overlay.__timer = setTimeout(() => {
            overlay.__timer = null;
            if (overlay.__open) showInlineBlocked(overlay, INLINE_BLOCKED_MESSAGE);
        }, INLINE_LOAD_TIMEOUT);

        if (overlay.hidden) {
            // 開いている間は背後のスクロールを止める
            overlay.__previousOverflow = document.body.style.overflow;
            overlay.__returnFocus = document.activeElement;
        }
        overlay.hidden = false;
        document.body.style.overflow = 'hidden';

        // 同じ URL を開き直したときも読み込み直す
        frame.removeAttribute('src');
        frame.src = resolved.src;

        // Esc は iframe の中に焦点があると親に届かない。開いたら閉じるボタンへ
        // 焦点を移しておく（iframe を触ったあとは「閉じる」で閉じる）。
        setTimeout(() => {
            try {
                overlay.querySelector('.ppt-inline-close').focus({ preventScroll: true });
            } catch (error) {
                /* 焦点を移せなくても表示は続ける */
            }
        }, 0);
        return true;
    }

    function closeInlineFrame() {
        const overlay = document.getElementById('ppt-inline-frame');
        if (!overlay || overlay.hidden) return;
        overlay.__open = false;
        clearTimeout(overlay.__timer);
        overlay.__timer = null;
        overlay.hidden = true;
        const frame = overlay.querySelector('.ppt-inline-iframe');
        // 音や再生を止めるため、閉じたら中身を捨てる
        if (frame) frame.src = 'about:blank';
        document.body.style.overflow = overlay.__previousOverflow || '';
        overlay.__previousOverflow = '';
        const back = overlay.__returnFocus;
        overlay.__returnFocus = null;
        if (back && back.isConnected && typeof back.focus === 'function') {
            try { back.focus({ preventScroll: true }); } catch (error) { /* 焦点は戻せなくてもよい */ }
        }
    }

    // クリックを横取りして Lab の中で開く（設定が newTab のときは何もしない）。
    // 開ける URL がまだ無いときは横取りせず、今までどおりの動きに任せる。
    function openLinkInLab(event, url, title) {
        if (!opensInLab()) return false;
        const resolved = inlineUrl(url);
        if (!resolved.src) return false;
        event.preventDefault();
        openInlineFrame(resolved.src, { title });
        return true;
    }

    function createViewerBridge(frame) {
        let ready = false;
        let latest = null;
        let sentKey = null;
        let lastIndex = -1;
        let pinger = null;
        const send = (action, data) => {
            try {
                frame.contentWindow?.postMessage({ type: VIEWER_MESSAGE, action, ...data }, VIEWER_ORIGIN);
            } catch (error) {
                /* 送れないときは次の機会に */
            }
        };
        const flush = () => {
            if (!ready || !latest) return;
            if (sentKey !== latest.key) {
                frame.dataset.loaded = 'false';
                sentKey = latest.key;
                lastIndex = latest.index;
                send('load', latest);
            } else if (lastIndex !== latest.index) {
                lastIndex = latest.index;
                send('seek', { key: latest.key, index: latest.index });
            }
        };
        const handshake = () => {
            if (ready) return;
            send('ready', {});
        };
        frame.dataset.bridge = 'pending';
        const startPing = () => {
            if (pinger) clearInterval(pinger);
            pinger = setInterval(handshake, 700);
            setTimeout(() => {
                if (!ready && pinger) {
                    clearInterval(pinger);
                    pinger = null;
                }
            }, 8000);
        };
        // iframe が読み込み直されるたびに仕切り直す
        frame.addEventListener('load', () => {
            ready = false;
            sentKey = null;
            lastIndex = -1;
            handshake();
            startPing();
        });
        startPing();
        return {
            frame,
            get ready() { return ready; },
            show(match, pages, index) {
                if (!match || !pages.length) return;
                latest = {
                    key: state.videoId + ':' + match.index,
                    title: '試合 ' + match.index,
                    pages,
                    index: Math.max(0, Math.min(index, pages.length - 1)),
                    theme: 'light'
                };
                flush();
            },
            receive(message) {
                if (!message || typeof message !== 'object') return;
                if (message.type === 'labViewerReady') {
                    frame.dataset.bridge = 'ready';
                    ready = true;
                    if (pinger) { clearInterval(pinger); pinger = null; }
                    sentKey = null;
                    flush();
                    return;
                }
                if (message.key !== (latest && latest.key)) return;
                if (message.type === 'labViewerLoaded') {
                    frame.dataset.loaded = 'true';
                    return;
                }
                if (message.type === 'labViewerPage' && Number.isInteger(message.index)
                    && latest && message.index >= 0 && message.index < latest.pages.length) {
                    lastIndex = message.index;
                    seekPosition(state.currentMatch, message.index, { pause: state.follow });
                    return;
                }
                if (message.type === 'labViewerSize' && Number.isFinite(message.height)
                    && message.height >= 100 && message.height <= 4000) {
                    frame.style.height = message.height + 'px';
                    return;
                }
                if (message.type === 'labViewerPause') {
                    const media = document.getElementById('detail-video');
                    if (media) media.pause();
                    state.ytPlayer?.pauseVideo?.();
                }
            }
        };
    }
    const SYNC_MS = 250;
    const SEEK_MUTE_MS = 800;
    // ビューワー（F/）が postMessage の受け付けを始めるまで待つ時間。
    // これを過ぎても返事が無い環境だけ、URL を開き直す方式に切り替える。
    const VIEWER_WAIT_MS = 15000;

    // Lab と同じ手順でビューワーを postMessage で駆動する（リロードしない）
    function pageIndexAt(pages, relative) {
        if (!pages || !pages.length) return -1;
        let low = 0;
        let high = pages.length;
        while (low < high) {
            const mid = (low + high) >> 1;
            if ((pages[mid].time || 0) <= relative + 0.001) low = mid + 1;
            else high = mid;
        }
        return Math.max(0, low - 1);
    }

    function matchAt(seconds) {
        const list = state.matches || [];
        let best = null;
        list.forEach((row, index) => {
            const next = list[index + 1];
            const end = Number.isFinite(row.end) && row.end > row.start
                ? row.end
                : (next ? next.start : Infinity);
            if (row.start <= seconds && seconds <= end + 0.5 && (!best || row.start > best.start)) {
                best = row;
            }
        });
        return best;
    }

    // プレイヤーが実際に再生している位置（まだ読めていなければ NaN）
    function playerTime() {
        const media = document.getElementById('detail-video');
        if (media) return media.readyState ? media.currentTime : NaN;
        if (state.ytReady && state.ytPlayer?.getCurrentTime) {
            const value = state.ytPlayer.getCurrentTime();
            return Number.isFinite(value) ? value : NaN;
        }
        return NaN;
    }

    // シーク直後はプレイヤーが古い位置を返すことがあるので、
    // 追いつくまで（最大 4 秒）だけシーク先を再生位置として扱う。
    // 握り続けると同期が固まるので、必ず解除する。
    function videoTime() {
        const seconds = playerTime();
        if (Number.isFinite(state.pendingSeek)) {
            const target = state.pendingSeek;
            const from = Number.isFinite(state.pendingSeekFrom) ? state.pendingSeekFrom : null;
            const age = Date.now() - (state.pendingSeekAt || 0);
            if (age > 4000) state.pendingSeek = null;
            else if (!Number.isFinite(seconds)) return target;
            else if (Math.abs(seconds - target) <= 1.5) state.pendingSeek = null;
            // シーク前の位置から動き出した＝シークが効いていないので実際の値に任せる
            else if (from !== null && Math.abs(seconds - from) > 2) state.pendingSeek = null;
            else return target;
        }
        return seconds;
    }

    function videoState() {
        const media = document.getElementById('detail-video');
        if (media) return media.error ? -1 : media.paused ? 2 : 1;
        if (state.ytReady) return state.ytPlayer?.getPlayerState?.() ?? -1;
        return -1;
    }

    function videoPlaying() {
        return videoState() === 1;
    }

    function seekVideoTo(seconds, options) {
        if (!Number.isFinite(seconds)) return;
        const opts = options || {};
        state.lastSeekAt = Date.now();
        const target = Math.max(0, seconds);
        const media = document.getElementById('detail-video');
        if (media) {
            state.pendingSeek = null;
            const apply = () => {
                try {
                    media.currentTime = target;
                } catch (error) {
                    /* シークできない形式でも続行 */
                }
            };
            if (media.readyState > 0) apply();
            else media.addEventListener('loadedmetadata', apply, { once: true });
            if (opts.pause) media.pause();
            else media.play().catch(() => {});
            return;
        }
        // シーク先を覚えておく（追いつくまでの再生位置として使う）
        const remember = () => {
            state.pendingSeek = target;
            state.pendingSeekAt = state.lastSeekAt;
            state.pendingSeekPaused = Boolean(opts.pause);
            state.pendingSeekFrom = playerTime();
        };
        if (state.ytReady && state.ytPlayer?.seekTo) {
            remember();
            const status = state.ytPlayer.getPlayerState?.();
            if (status === -1 || status === 5) {
                if (!opts.pause) state.ytPlayer.playVideo();
                return;
            }
            state.ytPlayer.seekTo(target, true);
            if (opts.pause) state.ytPlayer.pauseVideo?.();
            else state.ytPlayer.playVideo();
            return;
        }
        // プレイヤーの準備待ち。用意できたら onReady がシークする。
        remember();
        const frame = document.getElementById('detail-frame');
        if (frame && frame.dataset.base) {
            frame.src = frame.dataset.base + '?start=' + Math.round(target) + '&autoplay=' + (opts.pause ? 0 : 1);
        }
    }

    function seekPlayer(seconds) {
        seekVideoTo(seconds, { pause: false });
    }

    // 盤面 → 動画（ビューワー内でページを動かしたとき）
    function seekPosition(match, index, options) {
        if (!match || !state.pages[index]) return;
        const opts = options || {};
        const page = Math.max(0, Math.min(index, state.pages.length - 1));
        const seconds = match.start + (state.pages[page].time || 0);
        state.preferred = { matchIndex: match.index, page, seconds };
        state.pageIndex = page;
        const slider = document.getElementById('position-slider');
        if (slider) slider.value = String(page);
        const caption = document.querySelector('.lab-replay-caption .lab-position');
        if (caption) {
            caption.textContent = '局面 ' + (page + 1) + ' / ' + state.pages.length + ' · 動画 ' + timeText(seconds);
        }
        seekVideoTo(seconds, { pause: opts.pause !== false });
    }

    // ビューワーを指定ページで開き直す（同一ページなら何もしない）
    // F/ のリンクからハッシュを取り、シミュレータ用リンクを組み立てる
    // シミュレータは1つの盤面しか読めないので、?page=N（1始まり）で
    // 「いま見ている局面」を渡す。これが無いと試合開始（空の盤面）が開く。
    function simulatorLink(url, page) {
        if (!url || !url.includes('#')) return '';
        const index = Math.max(1, Math.round(Number(page) || 1));
        return SIMULATOR_BASE + 'hub/?entry=simulator&page=' + index + '#' + url.split('#')[1];
    }

    // 盤面復元に失敗した試合（batch-results.json の status が complete 以外）は
    // simulator フォルダが作られていないため、シミュレータ／ビューワーを開けない。
    const FAILED_MATCH_REASON =
        'この試合は盤面復元に失敗したため、シミュレータ／ビューワーを開けません（データがありません）';
    // 盤面データは残っているが、解析結果を信用できない試合（品質警告・開始盤面の
    // 読み取り誤り）。シミュレータ等は開けるので、row.failed とは分けて扱う。
    const LOW_QUALITY_MATCH_REASON =
        '盤面と読み取った盤面の一致率が低い試合です（盤面は参考程度に表示されます）';

    // 試合一覧のバッジ用。'' なら正常（バッジを出さない）。
    // needs_correction は「要修正」、それ以外の解析失敗は「解析失敗」。
    function matchFailureLabel(row) {
        if (!row) return '';
        if (row.analysisStatus === 'needs_correction') return '要修正';
        if (row.failed) return '解析失敗';
        if (row.analysisStatus === 'opening_failed'
            || row.analysisStatus === 'quality_failed'
            || row.analysisStatus === 'failed') return '解析失敗';
        return '';
    }

    // バッジのツールチップ（原因の短い説明）。
    function matchFailureReason(row) {
        const label = matchFailureLabel(row);
        if (!label) return '';
        const reason = String((row && row.analysisReason) || '').trim();
        if (!reason) return row && row.failed ? FAILED_MATCH_REASON : LOW_QUALITY_MATCH_REASON;
        const note = row && row.failed ? '（シミュレータ／ビューワーは開けません）' : '';
        return label + ': ' + reason + note;
    }

    function setToolLinkEnabled(link, enabled) {
        link.classList.toggle('is-disabled', !enabled);
        if (enabled) {
            link.removeAttribute('aria-disabled');
            link.removeAttribute('tabindex');
            link.removeAttribute('title');
            return;
        }
        // href を外すと、押しても画面遷移しない（見た目は CSS で控えめに落とす）
        link.removeAttribute('href');
        link.setAttribute('aria-disabled', 'true');
        link.setAttribute('tabindex', '-1');
        link.title = FAILED_MATCH_REASON;
    }

    // 解析に失敗した試合はビューワーに盤面が無い。空のままにせず理由を出す。
    function setViewerNotice(message) {
        const host = document.querySelector('.lab-boards.lab-viewer-host');
        if (!host) return;
        let notice = host.querySelector('.lab-viewer-status');
        if (!message) {
            if (notice) notice.remove();
            return;
        }
        if (!notice) {
            notice = node('p', null, 'lab-viewer-status');
            notice.setAttribute('role', 'status');
            host.append(notice);
        }
        notice.textContent = message;
    }

    // 「シミュレータ」「ビューワー」ボタンを今の局面に合わせる
    function updateMatchToolLinks(pageIndex) {
        const links = document.querySelectorAll('.lab-match-tools > a.btn-outline');
        const failed = Boolean(state.currentMatch && state.currentMatch.failed);
        links.forEach(link => setToolLinkEnabled(link, !failed));
        if (failed) return;
        const urls = state.currentUrls;
        if (!urls || !urls.combined) return;
        const page = Math.max(1, (Number(pageIndex) || 0) + 1);
        if (links[0]) links[0].href = simulatorLink(urls.combined, page) || '#';
        if (links[1]) links[1].href = withPage(siteUrl(urls.combined), page) || '#';
    }

    // リプレイJSON（urls + ページ列）を取得してキャッシュする
    async function replayUrls(index) {
        const cacheKey = state.videoId + ':' + index;
        if (state.replays.has(cacheKey)) return state.replays.get(cacheKey);
        const response = await fetch('/api/videos/' + state.videoId + '/replay/' + index);
        if (!response.ok) throw new Error('リプレイを読み込めませんでした');
        const replay = await response.json();
        const urls = replay.urls || {};
        let pages = replay?.simulatorData?.cases?.[0]?.pages || [];
        if (!pages.length && replay?.hash && window.TetrisEventCodec) {
            // 閲覧専用版: 状態ハッシュからページを復号する
            try {
                const binary = atob(String(replay.hash).replace(/-/g, '+').replace(/_/g, '/'));
                const json = JSON.parse(new TextDecoder().decode(Uint8Array.from(binary, ch => ch.charCodeAt(0))));
                const decoded = window.TetrisEventCodec.decodeCollection(json, { compactBoards: true });
                pages = decoded?.cases?.[decoded.currentCase || 0]?.pages || [];
            } catch (error) {
                pages = [];
            }
        }
        if (!pages.length && window.TetrisEventCodec && replay?.simulatorData && (replay.simulatorData.cases || window.TetrisEventCodec.isEventReplay?.(replay.simulatorData))) {
            try {
                const decoded = window.TetrisEventCodec.decodeCollection(replay.simulatorData, { compactBoards: true });
                pages = decoded?.cases?.[decoded.currentCase || 0]?.pages || [];
            } catch (error) {
                pages = [];
            }
        }
        if (!pages.length && urls.combined && urls.combined.includes('#') && window.TetrisEventCodec) {
            // 取り込み動画: URLのハッシュからページを復号する
            try {
                const hash = urls.combined.split('#')[1] || '';
                const binary = atob(hash.replace(/-/g, '+').replace(/_/g, '/'));
                const json = JSON.parse(new TextDecoder().decode(Uint8Array.from(binary, ch => ch.charCodeAt(0))));
                const decoded = window.TetrisEventCodec.decodeCollection(json, { compactBoards: true });
                pages = decoded?.cases?.[decoded.currentCase || 0]?.pages || [];
            } catch (error) { pages = pages || []; }
        }
        const value = {
            combined: urls.combined || '',
            p1: urls.p1 || '',
            p2: urls.p2 || '',
            pages: Array.isArray(pages) ? pages : []
        };
        state.replays.set(cacheKey, value);
        // 使いすぎないように古いものから捨てる（1試合あたり約2MB）
        while (state.replays.size > 8) {
            const oldest = state.replays.keys().next().value;
            if (oldest === cacheKey) break;
            state.replays.delete(oldest);
        }
        return value;
    }

    // 「?page=N」だけ差し替える（ほかのクエリと #ハッシュは残す）
    function withPage(url, page) {
        const [base, hash] = String(url).split('#');
        const clean = base.replace(/([?&])page=\d+&?/g, '$1').replace(/[?&]$/, '');
        return clean + (clean.includes('?') ? '&' : '?') + 'page=' + page + (hash !== undefined ? '#' + hash : '');
    }

    // 「?labViewer=1」を付ける（付いていればそのまま）。
    // URL を開き直す方式に落ちても、ビューワーの準備ができたら postMessage へ戻せる。
    function withLabViewer(url) {
        const [base, hash] = String(url).split('#');
        if (/([?&])labViewer=1(&|$)/.test(base)) return String(url);
        const clean = base.replace(/[?&]$/, '');
        return clean + (clean.includes('?') ? '&' : '?') + 'labViewer=1' + (hash !== undefined ? '#' + hash : '');
    }

    // ビューワーを postMessage で動かせる状態か。
    // 準備中でも「連携用の URL で開いている」なら true を返す（＝読み込み直さずに待つ）。
    // showPage() は 250ms ごとに呼ばれるので、ここで iframe の URL を書き換えると
    // 1ページ送るたびに全体を読み込み直すことになり、画面が激しく点滅する。
    function viewerBridgeLive() {
        const frame = document.getElementById('detail-viewer');
        if (!frame) return false;
        const bridge = state.bridge;
        if (!bridge || bridge.frame !== frame) return false;
        if (bridge.ready) return true;
        if (state.viewerFallback) return false;
        if (!String(frame.getAttribute('src') || '').includes('labViewer=1')) return false;
        return Date.now() < (state.viewerReadyBy || 0);
    }

    function setViewerPage(page) {
        const viewer = document.getElementById('detail-viewer');
        if (!viewer || !state.currentUrls || !state.currentUrls.combined) return;
        if (viewer.dataset.page === String(page)) return;
        // postMessage が使えるなら、そちらが来るまで待つ（二重読み込みを避ける）
        if (state.bridge && !state.bridge.ready && Date.now() < (state.bridgeGrace || 0)) return;
        const now = Date.now();
        if (now - (state.lastReloadAt || 0) < 700) return;   // 連続リロードを避ける
        state.lastReloadAt = now;
        // ここまで来たら URL 方式（連携が返ってこない環境）。以後は同じ方式で揃える。
        state.viewerFallback = true;
        viewer.dataset.page = String(page);
        viewer.src = withPage(withLabViewer(siteUrl(state.currentUrls.combined)), page + 1);
    }

    function showPage(index, options) {
        const pages = state.pages || [];
        if (!pages.length) return;
        const page = Math.max(0, Math.min(index, pages.length - 1));
        state.pageIndex = page;
        const slider = document.getElementById('position-slider');
        if (slider) slider.value = String(page);
        const caption = document.querySelector('.lab-replay-caption .lab-position');
        const match = state.currentMatch;
        const seconds = match ? match.start + (pages[page].time || 0) : 0;
        if (caption) {
            caption.textContent = '局面 ' + (page + 1) + ' / ' + pages.length + ' · 動画 ' + timeText(seconds);
        }
        if (options && options.seek) seekVideoTo(seconds, { pause: false });
        updateMatchToolLinks(page);
        if (match && viewerBridgeLive()) {
            // 準備中でも予約だけして、準備できた時点で最新のページを反映してもらう
            state.bridge.show(match, pages, page);
            return;
        }
        if (match && state.currentUrls && state.currentUrls.combined) {
            setViewerPage(page);
            return;
        }
        const viewer = document.getElementById('detail-viewer');
        if (viewer && state.currentUrls && state.currentUrls.combined) {
            viewer.src = withPage(withLabViewer(siteUrl(state.currentUrls.combined)), page + 1);
        }
    }

    // 局面が切り替わるときに一瞬だけ薄くして、ちらつきを目立たせない
    function fadeBoards() {
        const frame = document.getElementById('detail-viewer');
        if (!frame) return;
        frame.classList.remove('ppt-fading');
        void frame.offsetWidth;
        frame.classList.add('ppt-fading');
        setTimeout(() => frame.classList.remove('ppt-fading'), 260);
    }

    let youtubeApiPromise = null;
    function loadYouTubeApi() {
        if (window.YT && window.YT.Player) return Promise.resolve(window.YT);
        if (youtubeApiPromise) return youtubeApiPromise;
        youtubeApiPromise = new Promise(resolve => {
            const previous = window.onYouTubeIframeAPIReady;
            window.onYouTubeIframeAPIReady = () => {
                if (typeof previous === 'function') previous();
                resolve(window.YT);
            };
            const script = document.createElement('script');
            script.src = 'https://www.youtube.com/iframe_api';
            document.head.append(script);
        });
        return youtubeApiPromise;
    }

    async function mountYouTubePlayer(videoId) {
        if (!videoId) return;
        try {
            const YT = await loadYouTubeApi();
            if (state.ytPlayer && state.ytPlayer.destroy) {
                try { state.ytPlayer.destroy(); } catch (error) { /* 破棄できなくてもよい */ }
            }
            state.ytReady = false;
            state.ytPlayer = new YT.Player('detail-frame', {
                videoId,
                playerVars: { rel: 0, modestbranding: 1 },
                events: {
                    onReady: event => {
                        state.ytPlayer = event.target;
                        state.ytReady = true;
                        applyPlaybackRate();
                        // 準備前に来ていたシークを取りこぼさずに反映する
                        if (Number.isFinite(state.pendingSeek)) {
                            const target = state.pendingSeek;
                            state.pendingSeekAt = Date.now();
                            try {
                                state.ytPlayer.seekTo(target, true);
                                if (state.pendingSeekPaused) state.ytPlayer.pauseVideo?.();
                                else state.ytPlayer.playVideo();
                            } catch (error) {
                                /* シークできなくても再生はできる */
                            }
                        }
                    },
                    onStateChange: event => {
                        if (event.data === 0 && !advanceFeature()) playNextFeature();
                    }
                }
            });
        } catch (error) {
            /* API が使えなくても動画自体は再生できる */
        }
    }


    // 連続再生: 終わったら絞り込み結果の次の動画へ
    function playNextFeature() {
        if (!state.featureList || !state.featureList.length) return;
        const next = state.featureIndex + 1;
        if (next >= state.featureList.length) {
            toast('連続再生が終わりました。');
            state.featureList = [];
            state.featureIndex = -1;
            return;
        }
        state.featureIndex = next;
        state.videoId = state.featureList[next];
        state.pendingMatch = 1;
        state.follow = true;
        location.hash = '#video/' + state.videoId;
    }

    function startSync() {
        stopSync();
        state.syncTimer = setInterval(syncTick, SYNC_MS);
    }

    function stopSync() {
        if (state.syncTimer) clearInterval(state.syncTimer);
        state.syncTimer = null;
    }

    function syncTick() {
        const frame = document.getElementById('detail-viewer');
        if (!frame || !frame.isConnected) {
            stopSync();
            return;
        }
        if (document.hidden || !state.follow || state.syncing) return;
        if (Date.now() - state.lastSeekAt < SEEK_MUTE_MS) return;
        const status = videoState();
        if (status === -1 || status === 3) return;
        if (state.feature && state.feature.segments.length) {
            featureTick();
            return;
        }
        let seconds = videoTime();
        if (!Number.isFinite(seconds)) return;
        if (state.preferred && !videoPlaying() && Math.abs(seconds - state.preferred.seconds) < 1) {
            seconds = state.preferred.seconds;
        }
        const match = matchAt(seconds);
        if (!match) return;
        if (!state.currentMatch || state.currentMatch.index !== match.index) {
            state.syncing = true;
            showMatch(match, { fromSync: true })
                .catch(() => {})
                .finally(() => {
                    state.syncing = false;
                });
            return;
        }
        const next = pageIndexAt(state.pages, seconds - match.start);
        if (next !== state.pageIndex) showPage(next, { seek: false });
    }

    async function showMatch(row, options) {
        const opts = options || {};
        if (!opts.fromSync) seekVideoTo(row.start, { pause: false });
        state.currentMatch = row;
        const captionTitle = document.querySelector('.lab-replay-caption h3');
        // 下の試合カード（試合N・王冠）の選択も合わせる
        const railList = document.querySelector('.lab-match-list');
        if (railList) {
            railList.querySelectorAll('.lab-match-row').forEach(item => {
                item.classList.toggle('active', item.dataset.index === String(row.index));
            });
            const activeRow = railList.querySelector('.lab-match-row.active');
            if (activeRow) activeRow.scrollIntoView({ behavior: 'smooth', block: 'nearest', inline: 'center' });
        }
        if (captionTitle) captionTitle.textContent = '試合 ' + row.index;
        const viewer = document.getElementById('detail-viewer');
        if (!viewer) return;
        if (row.failed) {
            // 解析に失敗した試合はリプレイが無い（simulator フォルダが作られていない）。
            // 空のビューワーを読み込み直さず、理由だけ出す。
            state.currentUrls = null;
            state.pages = [];
            state.pageIndex = 0;
            // 連続再生の待ち状態が残ると次へ進めなくなるので、ここで解く
            if (state.featurePending && state.featurePending.videoId === state.videoId
                && state.featurePending.matchIndex === row.index) {
                state.featurePending = null;
            }
            viewer.dataset.status = 'failed';
            viewer.dataset.page = '';
            viewer.removeAttribute('data-ready');
            setViewerNotice('この試合は盤面復元に失敗したため、盤面データがありません。');
            const failedSlider = document.getElementById('position-slider');
            if (failedSlider) {
                failedSlider.max = '0';
                failedSlider.value = '0';
                failedSlider.disabled = true;
            }
            if (captionTitle) {
                captionTitle.textContent = '試合 ' + row.index
                    + '（' + (matchFailureLabel(row) || '解析失敗') + '）';
            }
            const failedPosition = document.querySelector('.lab-replay-caption .lab-position');
            if (failedPosition) {
                failedPosition.textContent = '動画 ' + timeText(row.start) + ' · 盤面データなし';
            }
            updateMatchToolLinks(0);
            return;
        }
        setViewerNotice('');
        viewer.dataset.status = 'loading';
        viewer.dataset.page = '';
        state.lastReloadAt = 0;
        // 読み込むまでの間、前の試合のページで同期しないように空にしておく
        state.pages = [];
        const videoId = state.videoId;
        const urls = await replayUrls(row.index);
        // 読み込んでいる間に別の動画へ移っていたら、そちらの表示を壊さない
        if (state.videoId !== videoId) return;
        state.currentUrls = urls;
        state.pages = urls.pages || [];
        state.pageIndex = -1;
        state.pageIndex = 0;
        if (state.featurePending && state.featurePending.videoId === state.videoId
            && state.featurePending.matchIndex === row.index) {
            state.featurePending = null;
        }
        const slider = document.getElementById('position-slider');
        if (slider) {
            slider.max = String(Math.max(0, state.pages.length - 1));
            slider.disabled = state.pages.length <= 1;
            slider.value = '0';
        }
        updateMatchToolLinks(0);
        if (state.bridge) state.bridge.show(row, state.pages, 0);
        setTimeout(() => {
            const frame = document.getElementById('detail-viewer');
            if (!frame || !frame.isConnected || frame.dataset.loaded === 'true') return;
            // ビューワーの準備待ちなら、そのまま待つ（読み込み直すと毎ページ全読み込みになる）
            if (viewerBridgeLive()) return;
            state.lastReloadAt = 0;
            frame.dataset.page = '';
            setViewerPage(state.pageIndex || 0);
        }, 3000);
        showPage(0, { seek: false });
        viewer.dataset.status = 'ready';
        viewer.dataset.ready = 'true';
    }
    // 保存する URL に「今の局面」を埋め込む（これが無いと1ページ目＝空の盤面が開く）
    function pinPage(url, page) {
        if (!url) return url;
        const parts = url.split('#');
        if (parts.length < 2) return url;
        const base = parts[0].replace(/\?.*$/, '');
        return base + '?page=' + Math.max(1, page) + '#' + parts[1];
    }

    async function saveMatchToTetofu(kind) {
        const row = state.currentMatch;
        if (!row) {
            toast('先に試合を選んでください。');
            return;
        }
        try {
            const urls = await replayUrls(row.index);
            const pages = (state.pages && state.pages.length) ? state.pages.length : 1;
            const current = (state.pageIndex || 0) + 1;
            const target = kind === 'p1' ? urls.p1 : kind === 'p2' ? urls.p2 : urls.combined;
            // 試合全体は最初の局面から開く（局面は持たない）
            const page = kind === 'all' ? pages : kind === 'match' ? 1 : current;
            const url = pinPage(target, page);
            if (!url) throw new Error('この盤面のリンクを作れませんでした');
            const label = kind === 'match' ? '試合' : kind === 'all' ? '全体盤面'
                : kind.toUpperCase() + '盤面';
            const note = kind === 'match' ? '' : kind === 'all' ? '（決着時）' : '（局面 ' + page + '/' + pages + '）';
            // 元の動画のどこか（試合全体は試合の始まり、局面はその局面の時刻）
            const pageInfo = (state.pages || [])[page - 1];
            const seconds = kind === 'match' || !pageInfo
                ? row.start : row.start + (pageInfo.time || 0);
            const title = ((shownName(row, 'p1') || '?') + ' / ' + (shownName(row, 'p2') || '?') + ' · ' + label + note).slice(0, 200);
            const saveIt = async tags => {
                const created = await api('/api/tetofu', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ kind: 'simulator', title, url,
                        videoId: state.videoId, matchIndex: row.index, scope: kind, seconds })
                });
                if (tags.length) {
                    await api('/api/tags/apply', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ target: created.id, tags })
                    });
                    await loadTags();
                }
                await loadTetofu();
                if (kind === 'match' && state.view === 'video') renderVideoDetail();
                notify(label + note + ' をテト譜に保存しました。');
            };
            // 設定「保存時にタグ選択を出す」がオフなら、タグなしでそのまま保存する
            if (!settingOn('tagPromptOnSave')) {
                await saveIt([]);
                return;
            }
            // 先にタグを選んでもらってから保存する
            openTagDialog({
                id: '',
                kind: 'tetofu',
                title: label + note + ' を保存',
                tags: [],
                autoTags: []
            }, {
                onSave: saveIt
            });
        } catch (error) {
            toast(error.message);
        }
    }

    function renderVideoDetail() {
        const video = state.videos.find(item => item.id === state.videoId);
        const matches = state.matches || [];
        state.currentMatch = matches[0] || null;

        const container = node('div', null, 'video-detail-container lab-analyzed-video');
        const comparison = node('section', null, 'lab-comparison');
        comparison.setAttribute('aria-label', '動画と認識盤面の比較');

        // ---- 左: プレイヤー ----
        const left = node('div', null, 'video-sticky-area');
        const wrapper = node('div', null, 'video-wrapper');
        const id = youtubeId(video && video.source);
        if (id) {
            const holder = node('div', null, 'ppt-yt-holder');
            holder.id = 'detail-frame';
            wrapper.append(holder);
            state.ytVideoId = id;
        } else if (video && video.source && !video.source.includes('://')) {
            const media = node('video', null, null);
            media.id = 'detail-video';
            media.controls = true;
            media.addEventListener('ended', () => { if (!advanceFeature()) playNextFeature(); });
            media.preload = 'metadata';
            media.src = '/api/videos/' + video.id + '/media';
            media.addEventListener('loadedmetadata', applyPlaybackRate);
            wrapper.append(media);
        } else {
            wrapper.append(node('p', 'この動画はこの画面では再生できません。', 'lab-help-text'));
        }
        left.append(wrapper);

        const controls = node('div', null, 'video-controls');
        [['x0.25', 0.25], ['x0.5', 0.5], ['x1.0', 1], ['x1.5', 1.5]].forEach(([label, rate]) => {
            const button = node('button', label, 'rate-btn' + (rate === (Number(state.playbackRate) || 1) ? ' active' : ''));
            button.type = 'button';
            button.addEventListener('click', () => {
                controls.querySelectorAll('.rate-btn').forEach(item => item.classList.remove('active'));
                button.classList.add('active');
                state.playbackRate = Number(rate) || 1;
                state.playbackRateChosen = true;
                applyPlaybackRate();
            });
            controls.append(button);
        });
        left.append(controls);

        const heading = node('div', null, 'lab-video-heading');
        const identity = node('div', null, 'lab-video-identity');
        identity.append(node('h1', video ? video.title : '動画', 'article-title'));
        heading.append(identity);
        const meta = node('div', null, 'lab-video-meta');
        meta.append(node('span', formatDate(videoDate(video))));
        if (video && video.matchCount) {
            const failedInVideo = matches.filter(row => Boolean(matchFailureLabel(row))).length;
            meta.append(node('span', '解析 ' + video.matchCount + '試合'
                + (failedInVideo ? '（うち解析失敗' + failedInVideo + '）' : '')));
        }
        heading.append(meta);
        const tagRow = node('div', null, 'lab-video-tags');
        videoTagKeys(video).forEach(key => tagRow.append(tagLink(key)));
        heading.append(tagRow);
        const headingActions = node('div', null, 'lab-video-heading-actions');
        // 編集メニュー（鉛筆アイコン）: 名前 / タグ / タイトル
        const editMenu = node('div', null, 'lab-save-menu ppt-edit-menu ppt-server-only');
        const editTrigger = node('button', null, 'ppt-icon-button');
        editTrigger.type = 'button';
        editTrigger.setAttribute('aria-label', '編集');
        editTrigger.setAttribute('aria-haspopup', 'true');
        editTrigger.setAttribute('aria-expanded', 'false');
        editTrigger.innerHTML = '<svg viewBox="0 0 24 24" width="16" height="16" aria-hidden="true"><path d="M4 20h4l10-10-4-4L4 16v4z" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/><path d="M13.6 5.6l4 4" fill="none" stroke="currentColor" stroke-width="1.8"/></svg>';
        const editList = node('div', null, 'lab-save-actions');
        const nameAction = node('button', 'プレイヤー名を変更', 'btn-outline');
        nameAction.type = 'button';
        nameAction.addEventListener('click', () => {
            editMenu.classList.remove('open');
            state.videoTitle = video ? video.title : '';
            state.edits = new Map();
            state.groups = new Set();
            location.hash = '#names/' + state.videoId;
        });
        const tagAction = node('button', 'タグを編集', 'btn-outline');
        tagAction.type = 'button';
        tagAction.addEventListener('click', () => {
            editMenu.classList.remove('open');
            openTagDialog({
                id: state.videoId,
                kind: 'video',
                title: video ? video.title : '',
                tags: (video && video.tags) || [],
                autoTags: (video && video.autoTags) || []
            });
        });
        const renameAction = node('button', 'タイトルを変更', 'btn-outline');
        renameAction.type = 'button';
        renameAction.addEventListener('click', () => {
            editMenu.classList.remove('open');
            openRename(video);
        });
        // 「タグそのものを全体から消す」はタグ画面に移した（この動画から外すのは「タグを編集」）
        const deleteVideoAction = node('button', '動画を削除', 'btn-outline');
        deleteVideoAction.type = 'button';
        deleteVideoAction.addEventListener('click', async () => {
            editMenu.classList.remove('open');
            if (!window.confirm('この動画を削除しますか？（解析結果とダウンロード動画も消えます）')) return;
            try {
                const result = await api('/api/videos/' + state.videoId, { method: 'DELETE' });
                state.videoId = '';
                await loadVideos();
                location.hash = '#videos';
                const freed = Number(result && result.removedMB) || 0;
                const warn = (result && result.warnings && result.warnings.length)
                    ? '（消せなかったファイルがあります）' : '';
                toast('動画を削除しました。' + (freed ? ' ' + freed.toFixed(1) + ' MB 解放' : '') + warn);
            } catch (error) {
                toast(error.message);
            }
        });
        editList.append(nameAction, tagAction, renameAction);
        if (video && video.status === 'failed') {
            const retryAction = node('button', 'もう一度解析する', 'btn-outline');
            retryAction.type = 'button';
            retryAction.addEventListener('click', () => {
                editMenu.classList.remove('open');
                retryVideo(video);
            });
            editList.append(retryAction);
        }
        editList.append(deleteVideoAction);
        editTrigger.addEventListener('click', () => {
            const open = editMenu.classList.toggle('open');
            editTrigger.setAttribute('aria-expanded', String(open));
        });
        editMenu.append(editTrigger, editList);
        headingActions.append(editMenu);
        const back = node('a', '← 動画一覧に戻る', 'btn-outline');
        back.href = '#videos';
        headingActions.append(back);
        heading.append(headingActions);
        left.append(heading);

        // 試合の一覧（横スクロールのレール）
        // Lab の動画詳細と同じく、レールの上に検索とプレイヤー1／2 の絞り込みを置く
        const rail = node('div', null, 'lab-match-timeline');
        rail.setAttribute('aria-label', '試合のタイムスタンプ');
        const railList = node('div', null, 'lab-match-list');
        railList.setAttribute('aria-label', '試合一覧');

        const filterRow = node('div', null, 'lab-match-filters');
        const matchSearch = node('input', null, 'lab-tag-filter');
        matchSearch.type = 'search';
        matchSearch.placeholder = '試合を検索（プレイヤー名）';
        matchSearch.setAttribute('aria-label', '解析した試合を絞り込む');
        matchSearch.value = state.matchFilters.query;
        matchSearch.addEventListener('input', () => {
            // レールだけを描き直すので、入力欄のフォーカスはそのまま残る
            state.matchFilters.query = matchSearch.value;
            renderMatchRail();
        });
        filterRow.append(matchSearch);

        const playerFilters = node('div', null, 'lab-match-player-filters');
        const playerChoices = matchPlayerChoices(matches, video);
        const playerSelects = ['player', 'opponent'].map((key, index) => {
            const wrap = node('label');
            const select = node('select');
            select.setAttribute('aria-label', 'この動画のプレイヤー' + (index + 1));
            select.dataset.matchFilter = key;
            [['', 'すべてのプレイヤー'], ...playerChoices.map(name => [name, name])]
                .forEach(([value, text]) => {
                    const option = node('option', text);
                    option.value = value;
                    select.append(option);
                });
            select.value = state.matchFilters[key];
            // 選択肢に無い名前（別の動画の絞り込み）は残さない
            if (select.value !== state.matchFilters[key]) state.matchFilters[key] = select.value;
            select.addEventListener('change', () => {
                state.matchFilters[key] = select.value;
                renderMatchRail();
            });
            wrap.append(select);
            playerFilters.append(wrap);
            return select;
        });
        filterRow.append(playerFilters);

        const clearMatches = node('button', '絞り込みを解除', 'btn-outline');
        clearMatches.type = 'button';
        clearMatches.addEventListener('click', () => {
            state.matchFilters = { query: '', player: '', opponent: '' };
            matchSearch.value = '';
            playerSelects.forEach(select => { select.value = ''; });
            renderMatchRail();
        });
        filterRow.append(clearMatches);
        // 解析結果が無い動画では絞り込む対象が無いので出さない
        filterRow.hidden = !matches.length;
        rail.append(filterRow);

        const matchCount = node('p', null, 'lab-match-count');
        matchCount.setAttribute('role', 'status');
        rail.append(matchCount);

        const renderMatchRail = () => {
            const rows = detailMatches(matches);
            railList.replaceChildren();
            rows.forEach(row => {
                const item = node('button', null, 'lab-match-row');
                item.type = 'button';
                item.dataset.index = String(row.index);
                const jump = node('span', null, 'lab-match-jump');
                const label = node('span', null, 'lab-match-label');
                label.append(node('strong', '試合 ' + row.index),
                    node('span', '▶ ' + timeText(row.start), 'ts-link'));
                jump.append(label);
                const failureLabel = matchFailureLabel(row);
                if (failureLabel) {
                    // 解析失敗（盤面データ無し／要修正）と、品質警告・開始盤面の読み取り誤り。
                    // 後者は盤面が残っているので、シミュレータ等はそのまま開ける。
                    item.classList.add('lab-match-row-failed');
                    item.dataset.failed = 'true';
                    item.title = matchFailureReason(row);
                    item.setAttribute('aria-label',
                        '試合 ' + row.index + '（' + failureLabel + '）');
                    jump.append(node('span', failureLabel, 'lab-match-failed'));
                }
                jump.append(playerNames(row));
                // 開幕テンプレ（名前を付けた1巡目）
                const openingNames = [row.p1Opening, row.p2Opening].filter(Boolean);
                if (openingNames.length && settingOn('showOpeningOnCards')) {
                    jump.append(node('span', (row.p1Opening || '—') + ' / ' + (row.p2Opening || '—'), 'lab-match-opening'));
                }
                // 試合全体として保存したテト譜のタグを、その試合のカードに出す
                const matchTags = matchTetofuTags(row.index);
                if (matchTags.length) {
                    const tagRow = node('span', null, 'lab-match-tags');
                    matchTags.forEach(tag => tagRow.append(tagLink(tag)));
                    jump.append(tagRow);
                }
                item.append(jump);
                item.addEventListener('click', () => {
                    railList.querySelectorAll('.lab-match-row').forEach(other => other.classList.remove('active'));
                    item.classList.add('active');
                    // 手動で別の試合を選んだら連続再生はやめる
                    const segment = state.feature.segments[state.feature.index];
                    if (state.feature.segments.length
                        && (!segment || segment.videoId !== state.videoId || segment.matchIndex !== row.index)) {
                        stopFeature();
                    }
                    showMatch(row);
                });
                railList.append(item);
            });
            if (!matches.length) {
                railList.append(node('p', '解析結果がまだありません。', 'lab-help-text'));
            } else if (!rows.length) {
                railList.append(node('p', '該当する試合がありません。', 'lab-help-text'));
            }
            // いま開いている試合が絞り込みに残っていれば、その行を選んだままにする
            const current = state.currentMatch;
            railList.querySelectorAll('.lab-match-row').forEach(item => {
                item.classList.toggle('active',
                    Boolean(current) && item.dataset.index === String(current.index));
            });
            const filtering = Boolean(state.matchFilters.query
                || state.matchFilters.player || state.matchFilters.opponent);
            // 動画全体の試合数に、解析に失敗した試合の件数を併記する
            const failedTotal = matches.filter(row => Boolean(matchFailureLabel(row))).length;
            matchCount.hidden = !matches.length;
            matchCount.textContent = matches.length
                ? (filtering ? rows.length + ' / ' + matches.length + '試合' : matches.length + '試合')
                    + (failedTotal ? '（うち解析失敗' + failedTotal + '）' : '')
                : '';
        };
        renderMatchRail();
        rail.append(railList);
        left.append(rail);

        // ---- 右: 作業台 ----
        const work = node('section', null, 'lab-workbench');
        const tools = node('div', null, 'lab-inline-actions lab-match-tools');
        const simLink = node('a', 'シミュレータ', 'btn-outline');
        simLink.target = '_blank';
        simLink.rel = 'noopener';
        simLink.href = '#';
        const viewerLink = node('a', 'ビューワー', 'btn-outline');
        viewerLink.target = '_blank';
        viewerLink.rel = 'noopener';
        viewerLink.href = '#';
        // 解析に失敗した試合では、押しても開けない理由を出す（href は外してある）。
        // 設定が「このウィンドウ内」なら、押したときに Lab の上へ重ねて開く
        // （target は残す。Ctrl+クリックなどは今までどおり新しいタブで開く）。
        [simLink, viewerLink].forEach(link => {
            link.addEventListener('click', event => {
                if (link.getAttribute('aria-disabled') === 'true') {
                    event.preventDefault();
                    toast(FAILED_MATCH_REASON);
                    return;
                }
                const href = link.getAttribute('href');
                if (!href || href === '#') {
                    // まだ試合を読み込んでいる途中（href が用意できていない）
                    event.preventDefault();
                    return;
                }
                openLinkInLab(event, href, link.textContent);
            });
        });
        tools.append(simLink, viewerLink);
        const copyButton = node('button', 'リンクをコピー', 'btn-outline');
        copyButton.type = 'button';
        copyButton.addEventListener('click', async () => {
            if (!state.currentMatch) return;
            try {
                const urls = await replayUrls(state.currentMatch.index);
                await navigator.clipboard.writeText(urls.combined);
                toast('リンクをコピーしました。');
            } catch (error) {
                toast(error.message);
            }
        });
        tools.append(copyButton);

        const saveMenu = node('div', null, 'lab-save-menu');
        const saveTrigger = node('button', '保存', 'btn-outline');
        saveTrigger.type = 'button';
        saveTrigger.setAttribute('aria-haspopup', 'true');
        saveTrigger.setAttribute('aria-expanded', 'false');
        const saveList = node('div', null, 'lab-save-actions');
        [['試合を保存', 'match'], ['全体盤面を保存', 'all'], ['P1盤面を保存', 'p1'], ['P2盤面を保存', 'p2']]
            .forEach(([label, kind]) => {
                const button = node('button', label, 'btn-outline');
                button.type = 'button';
                button.addEventListener('click', () => {
                    saveMenu.classList.remove('open');
                    saveMatchToTetofu(kind);
                });
                saveList.append(button);
            });
        saveTrigger.addEventListener('click', () => {
            const open = saveMenu.classList.toggle('open');
            saveTrigger.setAttribute('aria-expanded', String(open));
        });
        saveMenu.append(saveTrigger, saveList);
        tools.append(saveMenu);
        work.append(tools);

        const boards = node('div', null, 'lab-boards lab-viewer-host');
        const viewerFrame = ensureViewerFrame();
        // 盤面ビューワーは動画詳細を開いたときだけ読み込む（一覧を軽くする）
        if (!viewerFrame.getAttribute('src')) {
            viewerFrame.src = VIEWER_BASE + '?labViewer=1';
        }
        viewerFrame.hidden = false;
        viewerFrame.dataset.status = 'loading';
        viewerFrame.removeAttribute('data-ready');
        viewerFrame.dataset.status = 'loading';
        boards.append(viewerFrame);
        // 一覧に戻ると iframe が DOM から外れ、次に開くと別の iframe になる。
        // そのときは古いブリッジを捨てて、新しい iframe で作り直す。
        if (!state.bridge || state.bridge.frame !== viewerFrame) {
            state.bridge = createViewerBridge(viewerFrame);
            state.bridgeGrace = Date.now() + 6000;
        }
        // 開いた直後はビューワーの準備ができていない。準備できるまでページ送りは
        // 予約だけにして、読み込み直さない（毎ページ全読み込み＝点滅の原因）。
        state.viewerFallback = false;
        state.viewerReadyBy = Date.now() + VIEWER_WAIT_MS;
        if (!viewerFrame.__readyWatch) {
            viewerFrame.__readyWatch = true;
            viewerFrame.addEventListener('load', () => {
                // 読み込み直したら、その回の準備待ちをやり直す
                state.viewerReadyBy = Date.now() + VIEWER_WAIT_MS;
            });
        }
        work.append(boards);

        const caption = node('div', null, 'lab-replay-caption');
        const matchTitle = node('h3', '試合 ' + (state.currentMatch ? state.currentMatch.index : '-'));
        const positionText = node('p', null, 'lab-position');
        positionText.textContent = state.currentMatch
            ? '動画 ' + timeText(state.currentMatch.start)
            : '';
        caption.append(matchTitle, positionText);

        // 局面スライダー（Lab と同じ並び）
        const slider = node('input');
        slider.type = 'range';
        slider.id = 'position-slider';
        slider.min = '0';
        slider.max = '0';
        slider.value = '0';
        slider.disabled = true;
        slider.setAttribute('aria-label', '解析した局面');
        slider.addEventListener('input', () => showPage(Number(slider.value), { seek: true }));
        work.append(slider);

        const followLabel = node('label', null, 'lab-follow');
        const follow = node('input');
        follow.type = 'checkbox';
        follow.checked = true;
        follow.id = 'follow-page';
        follow.checked = state.follow !== false;
        follow.addEventListener('change', () => { state.follow = follow.checked; });
        followLabel.append(follow, '動画に局面を合わせる');
        work.append(followLabel);
        work.append(caption);

        const nav = node('div', null, 'lab-replay-navigation');
        const prev = node('button', '前の試合', 'btn-outline');
        prev.type = 'button';
        prev.dataset.shortcut = 'match-prev';
        const next = node('button', '次の試合', 'btn-outline');
        next.type = 'button';
        next.dataset.shortcut = 'match-next';
        const step = delta => {
            // 前/次の試合も絞り込んだ範囲の中だけで動かす
            const rows = detailMatches(matches);
            const index = rows.findIndex(row => row === state.currentMatch);
            const target = rows[Math.max(0, Math.min(rows.length - 1, index + delta))];
            if (!target) return;
            railList.querySelectorAll('.lab-match-row').forEach(other => {
                other.classList.toggle('active', other.dataset.index === String(target.index));
            });
            const activeRow = railList.querySelector('.lab-match-row.active');
            if (activeRow) activeRow.scrollIntoView({ behavior: 'smooth', block: 'nearest', inline: 'center' });
            stopFeature();
            showMatch(target);
        };
        prev.addEventListener('click', () => step(-1));
        next.addEventListener('click', () => step(1));
        const pagePrev = node('button', '前の局面', 'btn-outline');
        pagePrev.type = 'button';
        pagePrev.dataset.shortcut = 'page-prev';
        pagePrev.addEventListener('click', () => { fadeBoards(); showPage(state.pageIndex - 1, { seek: follow.checked }); });
        const pageNext = node('button', '次の局面', 'btn-outline');
        pageNext.type = 'button';
        pageNext.dataset.shortcut = 'page-next';
        pageNext.addEventListener('click', () => { fadeBoards(); showPage(state.pageIndex + 1, { seek: follow.checked }); });
        const pageNav = node('div', null, 'lab-inline-actions');
        pageNav.append(pagePrev, pageNext);
        work.append(pageNav);
        nav.append(prev, next);
        work.append(nav);

        comparison.append(left, work);
        container.append(comparison);
        const credit = creditLine(video);
        if (credit) container.append(credit);
        mainView.replaceChildren(container);
        if (state.ytVideoId) mountYouTubePlayer(state.ytVideoId);
        if (state.feature && state.feature.segments.length) {
            const startSegment = state.feature.segments[state.feature.index];
            if (startSegment && startSegment.videoId === state.videoId) {
                setTimeout(() => seekVideoTo(startSegment.start, { pause: false }), 350);
            }
        }

        if (matches.length) {
            const wanted = Number(state.pendingMatch) || 0;
            // 絞り込み中に開き直したときは、実際に見えている試合から始める
            const first = matches.find(row => row.index === wanted)
                || detailMatches(matches)[0]
                || matches[0];
            state.pendingMatch = 0;
            const pendingSeek = state.pendingSeek;
            state.pendingSeek = null;
            if (Number.isFinite(pendingSeek) && first.index === wanted) {
                // テト譜から来たとき: 試合の頭ではなく保存した局面の時刻へ
                setTimeout(() => seekVideoTo(pendingSeek, { pause: false }), 400);
            }
            const target = railList.querySelector('.lab-match-row[data-index="' + first.index + '"]');
            if (target) {
                railList.querySelectorAll('.lab-match-row').forEach(other => other.classList.remove('active'));
                target.classList.add('active');
            }
            showMatch(first).catch(() => {
                state.featurePending = null;
            });
        } else if (state.feature && state.feature.segments.length) {
            // この動画に試合が無い → 次の試合へ送る（止まったままにしない）
            state.featurePending = null;
            advanceFeature();
        }
        startSync();
    }
    function timeText(seconds) {
        const value = Math.max(0, Math.floor(Number(seconds) || 0));
        const minutes = Math.floor(value / 60);
        const rest = value % 60;
        return String(minutes).padStart(2, '0') + ':' + String(rest).padStart(2, '0');
    }
    // ---- 名前編集 ---------------------------------------------------------
    function groupKey(row) {
        return (row.p1Group || 0) + ':' + (row.p2Group || 0);
    }

    function editsFor(row) {
        return state.edits.get(row.index) || {
            p1: row.p1Guess || row.p1,
            p2: row.p2Guess || row.p2
        };
    }

    function isRemembered(row, side) {
        const guess = row[side + 'Guess'];
        return Boolean(guess) && guess !== row[side];
    }

    function openNames(videoId, title) {
        state.videoId = videoId;
        state.videoTitle = title || '';
        state.edits = new Map();
        state.groups = new Set();
        location.hash = '#names/' + videoId;
    }

    async function loadMatches(videoId) {
        const body = await api('/api/videos/' + videoId + '/matches');
        state.matches = Array.isArray(body.matches) ? body.matches : [];
        state.matchGroups = Array.isArray(body.groups) ? body.groups : [];
        state.warning = body.warning || '';
    }

    function applyName(row, side, value) {
        const key = groupKey(row);
        const targets = state.groups.has(key)
            ? state.matches.filter(other => groupKey(other) === key)
            : [row];
        targets.forEach(target => {
            const current = state.edits.get(target.index) || { p1: target.p1, p2: target.p2 };
            const next = { p1: current.p1, p2: current.p2 };
            next[side] = value;
            state.edits.set(target.index, next);
        });
        return targets.length;
    }

    // 名前確認の1枚（＝同じ人のまとまり）を作る。
    // サーバーが名前画像の指紋でまとめた組（/matches の groups）があればそれを
    // そのまま使い、無ければ従来どおり画像グループと文字でまとめる
    // （静的書き出しの matches/<id>.json には groups が無いため）。
    function nameEntries() {
        const remote = state.matchGroups || [];
        if (remote.length) {
            return remote.map(entry => ({
                key: entry.key,
                side: (entry.sides || [])[0] || 'p1',
                sides: new Set(entry.sides || []),
                image: entry.image || '',
                guess: entry.guess || '',
                name: entry.name || '',
                variants: Array.isArray(entry.variants) ? entry.variants : [],
                fingerprints: Array.isArray(entry.fingerprints) ? entry.fingerprints : [],
                fingerprint: (entry.fingerprints || [])[0] || '',
                remembered: Boolean(entry.remembered),
                only: Boolean(entry.override),
                rows: (entry.records || []).reduce((list, record) => {
                    let item = list.find(other => other.index === record.index);
                    if (!item) {
                        item = { index: record.index, sides: new Set() };
                        list.push(item);
                    }
                    item.sides.add(record.side);
                    return list;
                }, [])
            }));
        }
        // 同じ画像は P1/P2 をまたいでも1つにまとめる（試合番号とは切り離す）
        const groups = [];
        const index = new Map();
        state.matches.forEach(row => {
            ['p1', 'p2'].forEach(side => {
                const imageGroup = Number(row[side + 'Group']) || 0;
                const name = String(row[side] || '').trim();
                const fingerprint = row[side + 'Fingerprint'] || '';
                // まず画像のまとまり（p1Group / p2Group）で分ける。
                // 同じ名前の組は、あとで P1/P2 をまたいで統合する。
                const key = imageGroup
                    ? side + ':g' + imageGroup
                    : (name ? 'name:' + name : side + ':unknown');
                if (!index.has(key)) {
                    const entry = {
                        key,
                        side,
                        sides: new Set(),
                        image: row[side + 'Image'] || '',
                        guess: row[side + 'Guess'] || '',
                        rows: [],
                        remembered: false
                    };
                    index.set(key, entry);
                    groups.push(entry);
                }
                const entry = index.get(key);
                entry.sides.add(side);
                let record = entry.rows.find(item => item.index === row.index);
                if (!record) {
                    record = { index: row.index, sides: new Set() };
                    entry.rows.push(record);
                }
                record.sides.add(side);
                if (!entry.fingerprint && fingerprint) entry.fingerprint = fingerprint;
                if (row[side + 'Override']) entry.only = true;
                if (row[side + 'Guess']) entry.remembered = true;
                if (!entry.image && row[side + 'Image']) entry.image = row[side + 'Image'];
            });
        });
        // 同じ名前（表記ゆれを畳んだもの）の組は1つにまとめる
        const fold = value => String(value || '').normalize('NFKC')
            .replace(/[\s_\-・.,:;!?"'()（）\[\]]+/g, '').toLowerCase();
        const byName = new Map();
        groups.forEach(entry => {
            const firstRow = state.matches.find(row => row.index === entry.rows[0].index);
            const label = firstRow ? String(firstRow[[...entry.sides][0]] || '').trim() : '';
            entry.label = label;
            const key = fold(label);
            if (!key) return;
            if (!byName.has(key)) {
                byName.set(key, entry);
                return;
            }
            const host = byName.get(key);
            entry.rows.forEach(record => {
                let existing = host.rows.find(item => item.index === record.index);
                if (!existing) {
                    existing = { index: record.index, sides: new Set() };
                    host.rows.push(existing);
                }
                record.sides.forEach(side => {
                    existing.sides.add(side);
                    host.sides.add(side);
                });
            });
            if (!host.image && entry.image) host.image = entry.image;
            if (!host.fingerprint && entry.fingerprint) host.fingerprint = entry.fingerprint;
            if (entry.only) host.only = true;
            if (entry.remembered) host.remembered = true;
            entry.merged = true;
        });
        const merged = groups.filter(entry => !entry.merged);
        merged.sort((a, b) => b.rows.length - a.rows.length || a.rows[0].index - b.rows[0].index);
        merged.forEach(entry => {
            entry.name = entry.label || '';
            entry.variants = [];
            entry.fingerprints = entry.fingerprint ? [entry.fingerprint] : [];
        });
        return merged;
    }

    function renderNames() {
        const root = node('section', null, 'lab-video-library');
        mainView.replaceChildren(root);

        const header = node('div', null, 'lab-video-library-heading');
        const videoInfo = state.videos.find(item => item.id === state.videoId);
        header.append(node('h2', 'プレイヤー名の確認' + (videoInfo ? ' — ' + videoInfo.title : ''), 'section-title'));
        const headingActions = node('div', null, 'lab-inline-actions');
        // 名前の確認は動画ごとの作業なので、戻る先はその動画
        const back = node('a', '← 動画に戻る', 'btn-outline');
        back.href = '#video/' + state.videoId;
        const register = node('button', '登録', 'btn-outline');
        register.type = 'button';
        register.addEventListener('click', () => saveNames(register));
        headingActions.append(back, register);
        header.append(headingActions);
        root.append(header);

        const changedCount = () => {
            const finals = finalNames();
            return state.matches.filter(row => {
                const names = finals.get(row.index) || {};
                return (names.p1 !== null && names.p1 !== undefined && names.p1 !== row.p1) ||
                       (names.p2 !== null && names.p2 !== undefined && names.p2 !== row.p2);
            }).length;
        };

        // 名前画像の指紋でまとめた組（＝1組1枚）を使う。画像が取れない試合だけ、
        // 従来どおり文字でまとめる。
        const groups = nameEntries();

        const summary = node('p', null, 'lab-video-count');
        const updateSummary = () => {
            summary.textContent = state.matches.length + '試合 · ' + groups.length + '組 · 変更 ' + changedCount() + '件';
        };
        root.append(summary);
        if (state.warning) root.append(node('p', state.warning, 'lab-help-text'));

        const hint = node('p', '同じ名前帯の画像はまとめてあります。1か所直すと、その組の試合すべてに反映されます。', 'lab-help-text');
        root.append(hint);

        state.nameGroups = new Map(groups.map(item => [item.key, item]));
        const grid = node('div', null, 'ppt-name-groups');
        root.append(grid);

        groups.forEach(entry => {
            const card = node('article', null, 'card ppt-name-group');
            const head = node('div', null, 'ppt-name-group-head');
            head.append(node('span', [...entry.sides].map(side => side === 'p1' ? 'P1' : 'P2').join('/'), 'lab-source-badge'));
            head.append(node('span', entry.rows.length + '試合', 'lab-video-count'));
            if (!entry.name && !entry.guess) head.append(node('span', '（OCR空）', 'ppt-muted'));
            if (entry.remembered) head.append(node('span', '過去の確定名', 'ppt-remembered'));
            card.append(head);

            if (entry.image) {
                const image = node('img', null, 'ppt-name-image');
                image.src = entry.image;
                image.alt = '名前';
                image.loading = 'lazy';
                card.append(image);
            } else {
                card.append(node('span', '画像なし', 'ppt-muted'));
            }

            const input = node('input', null, 'ppt-name-input');
            input.dataset.side = entry.side;
            input.dataset.key = entry.key;
            input.type = 'text';
            input.maxLength = 40;
            const firstRow = entry.rows.length ? state.matches.find(row => row.index === entry.rows[0].index) : null;
            const firstSide = [...entry.sides][0] || 'p1';
            const edited = firstRow ? state.edits.get(firstRow.index) : null;
            // 編集中の値 > 過去の確定名 > 代表名（組で最多の OCR 表記）の順に出す
            input.value = edited
                ? (edited[firstSide] || '')
                : (entry.guess || entry.name || (firstRow ? editsFor(firstRow)[firstSide] : '') || '');
            input.placeholder = entry.guess || entry.name ? '名前を入力' : '（OCR空）';
            input.addEventListener('input', () => {
                entry.rows.forEach(record => {
                    const row = state.matches.find(item => item.index === record.index);
                    if (!row) return;
                    record.sides.forEach(side => applyName(row, side, input.value));
                });
                updateSummary();
            });
            card.append(input);
            // 組の中のほかの表記（OCR の揺れ）も参照できるようにする
            if (entry.variants && entry.variants.length > 1) {
                const text = item => (item.text || '（OCR空）') + ' (' + item.count + ')';
                const all = entry.variants.map(text);
                const variants = node('p', null, 'ppt-name-variants');
                variants.textContent = '表記ゆれ: ' + all.slice(0, 3).join(' / ') +
                    (all.length > 3 ? ' ほか' + (all.length - 3) + '件' : '');
                variants.title = all.join('\n');
                card.append(variants);
            }
            const onlyLabel = node('label', null, 'ppt-name-override');
            const only = node('input');
            only.type = 'checkbox';
            only.checked = Boolean(entry.only);
            only.addEventListener('change', () => { entry.only = only.checked; });
            onlyLabel.append(only, 'この動画だけ');
            card.append(onlyLabel);

            const numbers = node('p', null, 'ppt-name-group-matches');
            const shown = entry.rows.slice(0, 12).map(record => '#' + record.index).join(' ');
            numbers.textContent = shown + (entry.rows.length > 12 ? ' …' : '');
            card.append(numbers);

            grid.append(card);
        });

        // 試合ごとの一覧も残す（折りたたみ）
        const details = document.createElement('details');
        details.className = 'ppt-name-details';
        details.append(node('summary', '試合ごとの一覧を見る'));
        const table = node('table', null, 'ppt-name-table');
        const head = node('thead');
        const headRow = node('tr');
        ['#', '名前画像', 'P1', 'P2'].forEach(label => headRow.append(node('th', label)));
        head.append(headRow);
        table.append(head);
        const body = node('tbody');
        table.append(body);
        state.matches.forEach(row => {
            const tr = node('tr');
            tr.append(node('td', String(row.index)));
            const imageCell = node('td');
            if (row.p1Image && row.p2Image) {
                const wrap = node('div', null, 'ppt-name-images');
                [row.p1Image, row.p2Image].forEach(source => {
                    const image = node('img');
                    image.src = source;
                    image.alt = '';
                    image.loading = 'lazy';
                    wrap.append(image);
                });
                imageCell.append(wrap);
            } else {
                imageCell.append(node('span', '画像なし', 'ppt-muted'));
            }
            tr.append(imageCell);
            ['p1', 'p2'].forEach(side => {
                const cell = node('td');
                const input = node('input', null, 'ppt-name-input');
                input.type = 'text';
                input.maxLength = 40;
                input.value = editsFor(row)[side];
                input.addEventListener('input', () => {
                    applyName(row, side, input.value);
                    updateSummary();
                });
                cell.append(input);
                tr.append(cell);
            });
            body.append(tr);
        });
        table.append(body);
        details.append(table);
        root.append(details);
        updateSummary();
    }
    // 登録する名前を試合ごとに決める。
    // 組（名前画像のまとまり）の欄に出ている名前は、触っていなくても組の全試合に当てる
    // （表記ゆれの試合も代表名にそろえる）。試合ごとの表で直した名前はそちらを優先する。
    function finalNames() {
        const result = new Map();
        state.matches.forEach(row => {
            const edit = state.edits.get(row.index);
            result.set(row.index, {
                p1: edit && edit.p1 !== row.p1 ? edit.p1 : null,
                p2: edit && edit.p2 !== row.p2 ? edit.p2 : null
            });
        });
        document.querySelectorAll('.ppt-name-groups .ppt-name-input[data-key]').forEach(box => {
            const entry = (state.nameGroups || new Map()).get(box.dataset.key);
            const value = box.value.trim();
            if (!entry || !value) return;
            entry.rows.forEach(record => {
                const names = result.get(record.index);
                if (!names) return;
                (record.sides || []).forEach(side => {
                    if (names[side] === null) names[side] = value;
                });
            });
        });
        return result;
    }

    async function saveNames(button) {
        const finals = finalNames();
        const updates = state.matches
            .map(row => {
                const names = finals.get(row.index) || {};
                // 変わった側だけ送る（変えていない側の誤読を「確定名」として覚えさせない）
                const p1 = names.p1 !== null && names.p1 !== undefined && names.p1 !== row.p1 ? names.p1 : null;
                const p2 = names.p2 !== null && names.p2 !== undefined && names.p2 !== row.p2 ? names.p2 : null;
                if (p1 === null && p2 === null) return null;
                return { index: row.index, p1, p2 };
            })
            .filter(Boolean);
        // 登録 = 表示されている名前で確定する。変更が無くても「正しい」として確定し、
        // 名前画像の記憶にも入れる（以前は「変更なし」で止まり、名前待ちのまま残っていた）
        const confirmed = state.matches
            .map(row => {
                const names = finals.get(row.index) || {};
                const pick = (value, fallback) => String((value !== null && value !== undefined ? value : fallback) || '').trim();
                const p1 = pick(names.p1, row.p1);
                const p2 = pick(names.p2, row.p2);
                return (p1 || p2) ? { index: row.index, p1: p1 || null, p2: p2 || null } : null;
            })
            .filter(Boolean);
        button.disabled = true;
        button.textContent = '保存中…';
        try {
            const overrides = [];
            (state.nameGroups || new Map()).forEach(entry => {
                if (!entry.only) return;
                // 組に含まれる指紋すべてに同じ名前を覚えさせる（保存の形は従来どおり）
                const fingerprints = (entry.fingerprints && entry.fingerprints.length)
                    ? entry.fingerprints
                    : (entry.fingerprint ? [entry.fingerprint] : []);
                if (!fingerprints.length) return;
                const box = document.querySelector('.ppt-name-groups .ppt-name-input[data-key="' + entry.key + '"]');
                const value = box ? box.value.trim() : '';
                if (value) fingerprints.forEach(fingerprint => overrides.push({ fingerprint, name: value }));
            });
            const result = await api('/api/videos/' + state.videoId + '/names', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ updates, overrides, confirm: true, confirmed })
            });
            toast(result.changed > 0 ? result.changed + '試合の名前を保存しました。' : '名前を確定しました。');
            // 一覧・試合一覧の名前も保存後の名前で出し直す
            if (state.matchCache) state.matchCache.delete(state.videoId);
            state.playerStats = new Map();
            await loadVideos();
            await loadMatches(state.videoId);
            state.edits = new Map();
            renderNames();
        } catch (error) {
            toast(error.message);
        } finally {
            button.disabled = false;
            button.textContent = '登録';
        }
    }
    // ---- 取得 -------------------------------------------------------------
    async function loadVideos() {
        const body = await api('/api/videos');
        const videos = Array.isArray(body.videos) ? body.videos : [];
        const serialized = JSON.stringify(videos);
        const changed = serialized !== state.lastVideosJson;
        // 解析が終わった動画があれば知らせる
        const before = new Map((state.videos || []).map(item => [item.id, item.status]));
        // 設定「解析完了の通知」がオフなら知らせない
        const wantNotice = settingOn('notifyAnalysis');
        videos.forEach(item => {
            const previous = before.get(item.id);
            if (!wantNotice || !previous || previous === item.status) return;
            if (previous !== 'needs-names' && item.status === 'needs-names') {
                notify('解析が終わりました：' + (item.title || '') + '（' + (item.matchCount || 0) + '試合）');
            } else if ((previous === 'queued' || previous === 'analyzing') && item.status === 'failed') {
                toast('解析に失敗しました：' + (item.title || '') + (item.error ? '（' + String(item.error).slice(0, 80) + '）' : ''));
            }
        });
        state.videos = videos;
        state.lastVideosJson = serialized;
        if (!state.tagsLoaded) {
            try { await loadTags(); } catch (error) { /* 色分けできなくても一覧は出す */ }
        }
        return changed;
    }

    // ---- 開幕テンプレ（1巡目） ------------------------------------------------
    // 1巡目の7個の置き場所が完全に同じもの（左右反転を含む）を1つにまとめ、回数の多い順に出す。
    // 名前を付けると、その形はこの先の解析でも自動でその名前になる（プレイヤー名と同じ考え方）。
    async function loadOpenings() {
        state.openings = await api('/api/openings');
    }

    function drawOpening(canvas, pieces, extras) {
        const cells = [];
        Object.entries(pieces || {}).forEach(([piece, list]) => {
            (list || []).forEach(([row, col]) => cells.push([row, col, piece, false]));
        });
        // 7種類そろう前に置いた2巡目のミノ（薄く）
        (extras || []).forEach(([piece, list]) => {
            (list || []).forEach(([row, col]) => cells.push([row, col, piece, true]));
        });
        const rows = Math.max(4, ...cells.map(cell => cell[0] + 1));
        const size = 14;
        canvas.width = 10 * size;
        canvas.height = rows * size;
        const ctx = canvas.getContext('2d');
        ctx.fillStyle = TETOFU_BOARD_BG;
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.strokeStyle = TETOFU_GRID;
        ctx.lineWidth = 1;
        for (let x = 0; x <= 10; x++) {
            ctx.beginPath(); ctx.moveTo(x * size + 0.5, 0); ctx.lineTo(x * size + 0.5, canvas.height); ctx.stroke();
        }
        for (let y = 0; y <= rows; y++) {
            ctx.beginPath(); ctx.moveTo(0, y * size + 0.5); ctx.lineTo(canvas.width, y * size + 0.5); ctx.stroke();
        }
        cells.forEach(([row, col, piece, faint]) => {
            ctx.globalAlpha = faint ? 0.3 : 1;
            ctx.fillStyle = TETOFU_COLORS[piece] || TETOFU_COLORS.G;
            ctx.fillRect(col * size + 1, (rows - 1 - row) * size + 1, size - 1, size - 1);
        });
        ctx.globalAlpha = 1;
    }

    async function saveOpeningName(key, name) {
        await api('/api/openings/name', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ key, name })
        });
        state.playerStats = new Map();
        state.matchCache = new Map();
        await loadOpenings();
    }

    function openingCard(group, data, options) {
        const opts = options || {};
        const classified = data.classified || 1;
        const card = node('article', null, 'card ppt-opening-card' + (group.name ? ' named' : ''));
        if (group.name) card.style.borderColor = (data.colors || {})[group.name] || '';
        const canvas = node('canvas', null, 'ppt-opening-board');
        drawOpening(canvas, group.pieces, group.extras);
        card.append(canvas);
        const decided = group.wins + group.losses;
        card.append(node('p', group.count + ' 回（' + (group.count * 100 / classified).toFixed(1) + '%）'
            + (group.flipped ? ' · うち反転 ' + group.flipped : '')
            + (decided ? ' · 勝率 ' + Math.round(group.wins * 100 / decided) + '%' : ''), 'ppt-opening-count'));
        if (group.extrasCount) {
            card.append(node('p', '薄い色 = 7種類そろう前に置いた2巡目のミノ（' + group.extrasCount + ' 回中いちばん多い置き方）', 'ppt-opening-note'));
        }
        if ((group.players || []).length) {
            card.append(node('p', group.players.map(item => item.name + ' ' + item.count).join(' / '), 'ppt-opening-players'));
        }
        const form = node('form', null, 'ppt-opening-form');
        const input = node('input');
        input.type = 'text';
        input.maxLength = 60;
        input.placeholder = 'テンプレ名';
        input.value = group.name || '';
        input.setAttribute('list', 'opening-name-list');
        input.setAttribute('aria-label', 'テンプレ名');
        const save = node('button', group.name ? '変更' : '登録', 'btn-outline');
        save.type = 'submit';
        form.append(input, save);
        form.addEventListener('submit', async event => {
            event.preventDefault();
            save.disabled = true;
            try {
                await saveOpeningName(group.key, input.value);
                notify(input.value.trim() ? '「' + input.value.trim() + '」として登録しました。' : '名前を外しました。');
                const y = window.scrollY;
                opts.rerender();
                window.scrollTo(0, y);
            } catch (error) {
                toast(error.message);
                save.disabled = false;
            }
        });
        card.append(form);
        const examples = node('div', null, 'ppt-opening-examples');
        (group.examples || []).slice(0, 4).forEach(example => {
            const link = node('a', (example.player || example.side.toUpperCase()) + ' · 試合 ' + example.matchIndex);
            link.href = '#video/' + example.videoId;
            link.title = example.videoTitle || '';
            link.addEventListener('click', event => {
                event.preventDefault();
                state.pendingMatch = example.matchIndex;
                location.hash = '#video/' + example.videoId;
            });
            examples.append(link);
        });
        card.append(examples);
        return card;
    }

    function renderOpenings(container) {
        container.replaceChildren();
        const rerender = () => renderOpenings(container);
        const data = state.openings || { openings: [] };
        const groups = data.openings || [];
        const skipped = Object.values(data.skipped || {}).reduce((sum, value) => sum + value, 0);
        container.append(node('p', '1巡目の7個（I・J・L・O・S・T・Z 各1個）の置き場所が完全に同じものを、左右反転も含めてまとめています。'
            + 'ライン消去は無かったことにして、消える前の盤面の座標で並べます（そろった段もそのまま表示）。'
            + '全 ' + (data.total || 0) + ' 人分のうち ' + (data.classified || 0) + ' 人分を分類'
            + (skipped ? '（せり上がりなどで座標が取れない ' + skipped + ' 人分は対象外）' : '') + '。'
            + '名前を付けると、今後の解析にも自動で付きます。同じ名前を複数の形に付けてもかまいません。', 'lab-help-text'));
        if (data.building) {
            container.append(node('p', '集計中… ' + (data.progress ? data.progress.done + ' / ' + data.progress.total + ' 本' : ''), 'lab-help-text'));
        }

        // 判定エンジン（名前を付けた形の一式）の書き出し・読み込み
        const io = node('div', null, 'lab-inline-actions');
        const exportLink = node('a', 'テンプレ判定を書き出す', 'btn-outline');
        exportLink.href = '/api/openings/export';
        exportLink.setAttribute('download', '');
        const importButton = node('button', 'テンプレ判定を読み込む', 'btn-outline');
        importButton.type = 'button';
        const file = node('input');
        file.type = 'file';
        file.accept = '.json,application/json';
        file.hidden = true;
        importButton.addEventListener('click', () => file.click());
        file.addEventListener('change', async () => {
            const chosen = file.files && file.files[0];
            file.value = '';
            if (!chosen) return;
            try {
                const payload = JSON.parse(await chosen.text());
                const count = (payload.templates || []).length;
                const replace = window.confirm('「' + chosen.name + '」（' + count + ' 形）を読み込みます。\n'
                    + 'OK = 今の名前を全部消して置き換える ／ キャンセル = 今の名前に追加する');
                const result = await api('/api/openings/import', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ data: payload, replace })
                });
                state.playerStats = new Map();
                state.matchCache = new Map();
                await loadOpenings();
                rerender();
                notify(result.names + ' 個のテンプレ（' + result.imported + ' 形）を読み込みました。');
            } catch (error) {
                toast(error.message || '読み込めませんでした');
            }
        });
        io.append(exportLink, importButton, file);
        container.append(io);

        const controls = node('div', null, 'lab-video-filters');
        const add = (label, key, fallback, choices) => {
            const wrap = node('label', label);
            const select = node('select');
            choices.forEach(([value, text]) => {
                const option = node('option', text);
                option.value = value;
                select.append(option);
            });
            select.value = state[key] || fallback;
            select.addEventListener('change', () => { state[key] = select.value; rerender(); });
            wrap.append(select);
            controls.append(wrap);
        };
        add('表示', 'openingMode', 'shapes', [['shapes', '形ごと（多い順）'], ['names', '名前ごと']]);
        if ((state.openingMode || 'shapes') === 'shapes') {
            add('名前', 'openingFilter', 'all', [['all', 'すべて'], ['unnamed', '名前なしだけ'], ['named', '名前ありだけ']]);
            add('回数', 'openingMin', '2', [['1', '1回以上'], ['2', '2回以上'], ['5', '5回以上'], ['10', '10回以上']]);
        }
        container.append(controls);

        const names = [...new Set(groups.map(group => group.name).filter(Boolean))].sort();
        const datalist = node('datalist');
        datalist.id = 'opening-name-list';
        names.forEach(name => { const option = node('option'); option.value = name; datalist.append(option); });
        container.append(datalist);

        if ((state.openingMode || 'shapes') === 'names') {
            // 名前ごとに、どの形がその名前に分類されるかを並べる
            if (!names.length) container.append(node('p', 'まだ名前を付けた開幕がありません。「形ごと」で名前を付けてください。', 'lab-help-text'));
            names.forEach(name => {
                const members = groups.filter(group => group.name === name);
                const total = members.reduce((sum, group) => sum + group.count, 0);
                const section = node('section', null, 'ppt-opening-name');
                const head = node('div', null, 'ppt-opening-name-head');
                const color = node('input');
                color.type = 'color';
                color.value = (data.colors || {})[name] || '#9a9a9a';
                color.title = 'この名前の色（円グラフなどに使う）';
                color.addEventListener('change', async () => {
                    try {
                        await api('/api/openings/color', {
                            method: 'POST',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({ name, color: color.value })
                        });
                        state.playerStats = new Map();
                        await loadOpenings();
                        rerender();
                    } catch (error) {
                        toast(error.message);
                    }
                });
                head.append(color, node('h3', name), node('span', members.length + ' 形 · ' + total + ' 回', 'lab-video-count'));
                section.append(head);
                const grid = node('div', null, 'ppt-tile-grid ppt-opening-grid');
                members.forEach(group => grid.append(openingCard(group, data, { rerender })));
                section.append(grid);
                container.append(section);
            });
            return;
        }

        const minCount = Number(state.openingMin || '2');
        const shown = groups.filter(group => {
            if (!group.count || group.count < minCount) return false;
            if (state.openingFilter === 'unnamed') return !group.name;
            if (state.openingFilter === 'named') return Boolean(group.name);
            return true;
        });
        container.append(node('p', shown.length + ' 種類', 'lab-video-count'));
        const grid = node('div', null, 'ppt-tile-grid ppt-opening-grid');
        shown.forEach(group => grid.append(openingCard(group, data, { rerender })));
        if (!shown.length) grid.append(node('p', '条件に合う開幕はありません。', 'lab-help-text'));
        container.append(grid);
    }

    // ---- ブラウザ版: 自分の保存データ（テト譜・タグ）のバックアップ ----------------
    async function renderViewerDataTab(body) {
        body.replaceChildren();
        const api = window.PPTLocal;
        if (!api) {
            body.append(node('p', 'この画面はブラウザ版だけで使います。', 'lab-help-text'));
            return;
        }
        const bar = node('div', null, 'lab-inline-actions');
        const exportButton = node('button', '書き出す（バックアップ）', 'btn-outline');
        exportButton.type = 'button';
        exportButton.addEventListener('click', async () => {
            const payload = await api.exportData();
            const blob = new Blob([JSON.stringify(payload, null, 1)], { type: 'application/json' });
            const link = document.createElement('a');
            link.href = URL.createObjectURL(blob);
            link.download = 'ppt-lab-mydata-' + new Date().toISOString().slice(0, 10) + '.json';
            document.body.append(link);
            link.click();
            link.remove();
            setTimeout(() => URL.revokeObjectURL(link.href), 1000);
        });
        const importButton = node('button', '読み込む', 'btn-outline');
        importButton.type = 'button';
        const file = node('input');
        file.type = 'file';
        file.accept = '.json,application/json';
        file.hidden = true;
        importButton.addEventListener('click', () => file.click());
        file.addEventListener('change', async () => {
            const chosen = file.files && file.files[0];
            file.value = '';
            if (!chosen) return;
            try {
                const payload = JSON.parse(await chosen.text());
                const replace = window.confirm('「' + chosen.name + '」を読み込みます。\n'
                    + 'OK = 今の保存を消して置き換える ／ キャンセル = 今の保存に追加する');
                const result = await api.importData(payload, replace);
                await Promise.all([loadTetofu(), loadTags(), loadSettings()]);
                notify('読み込みました（テト譜 ' + result.tetofu + ' 件）。');
                renderViewerDataTab(body);
            } catch (error) {
                toast(error.message || '読み込めませんでした');
            }
        });
        const clearButton = node('button', 'すべて消す', 'btn-outline ppt-danger');
        clearButton.type = 'button';
        clearButton.addEventListener('click', async () => {
            if (!window.confirm('このブラウザに保存したテト譜とタグをすべて消しますか？元に戻せません。')) return;
            await api.clear();
            await Promise.all([loadTetofu(), loadTags()]);
            notify('保存データを消しました。');
            renderViewerDataTab(body);
        });
        bar.append(exportButton, importButton, file, clearButton);
        body.append(bar);
    }

    async function retryVideo(video) {
        if (!video || !video.id) return;
        if (!window.confirm('「' + (video.title || '動画') + '」をもう一度解析しますか？')) return;
        try {
            await api('/api/videos/' + video.id + '/retry', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: '{}'
            });
            await loadVideos();
            render();
            notify('再解析を始めました。');
        } catch (error) {
            toast(error.message);
        }
    }

    function matchTetofuTags(index) {
        const seen = [];
        (state.tetofu || []).forEach(item => {
            if (item.scope !== 'match' || item.videoId !== state.videoId || item.matchIndex !== index) return;
            (item.tags || []).forEach(tag => { if (!seen.includes(tag)) seen.push(tag); });
        });
        return seen;
    }

    async function loadTetofu() {
        const body = await api('/api/tetofu');
        state.tetofu = Array.isArray(body.tetofu) ? body.tetofu : [];
    }

    async function loadTags() {
        const body = await api('/api/tags');
        state.tags = Array.isArray(body.tags) ? body.tags : [];
        state.tagsLoaded = true;
    }

    async function loadEnvironment() {
        // 環境は起動時に確認するだけで、画面には出さない
        try {
            state.env = await api('/api/env');
        } catch (error) {
            state.env = null;
        }
    }

    // ---- 追加 -------------------------------------------------------------
    function openRename(video) {
        state.videoTitle = video ? video.title : '';
        state.videoId = video ? video.id : state.videoId;
        openEntry('rename');
    }

    function openEntry(mode) {
        if (mode === 'rename') {
            dialogTitle.textContent = 'タイトルを変更';
            dialogNote.textContent = 'この動画の表示名です。';
            entryTitle.value = state.videoTitle || '';
            entryUrl.value = '';
            entryTitle.parentElement.hidden = false;
            entryUrl.parentElement.hidden = true;
            entryTitle.required = true;
            entrySaveField.hidden = true;
            entrySubmit.textContent = '保存する';
            state.entryMode = 'rename';
            dialog.showModal();
            entryTitle.focus();
            return;
        }
        state.entryMode = mode;
        const isYoutube = mode === 'youtube';
        dialogTitle.textContent = isYoutube ? 'YouTubeから追加' : 'テト譜を登録';
        dialogNote.textContent = isYoutube
            ? 'URLを貼ると、取得から解析まで自動で進みます。権利のある動画だけを追加してください。'
            : (mode === 'official'
                ? '本家テト譜のリンクを貼ってください。'
                : mode === 'editor'
                    ? 'エディタのリンクを貼ってください。'
                    : 'シミュレータのリンクを貼ってください。');
        entryTitle.value = '';
        entryUrl.value = '';
        entryTitle.parentElement.hidden = isYoutube;
        entryUrl.parentElement.hidden = false;
        entryTitle.required = !isYoutube;
        entrySaveField.hidden = !isYoutube;
        entrySaveVideo.checked = setting('downloadVideo') !== false;
        entrySubmit.textContent = '登録する';
        entryUrl.placeholder = isYoutube ? 'https://www.youtube.com/watch?v=...' : 'https://...';
        dialog.showModal();
        (isYoutube ? entryUrl : entryTitle).focus();
    }

    document.getElementById('entry-cancel').addEventListener('click', () => dialog.close());
    document.getElementById('tag-cancel').addEventListener('click', () =>
        document.getElementById('tag-dialog').close());
    document.getElementById('entry-form').addEventListener('submit', async event => {
        event.preventDefault();
        const url = entryUrl.value.trim();
        const renaming = state.entryMode === 'rename' || state.entryMode === 'rename-tetofu';
        if (!url && !renaming) {
            entryUrl.focus();
            return;
        }
        try {
            if (state.entryMode === 'rename-tetofu') {
                const title = entryTitle.value.trim();
                if (!title) { entryTitle.focus(); return; }
                await api('/api/tetofu/' + state.tetofuId + '/title', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ title })
                });
                dialog.close();
                await loadTetofu();
                render();
                toast('名前を変更しました。');
                return;
            }
            if (state.entryMode === 'rename') {
                const title = entryTitle.value.trim();
                if (!title) { entryTitle.focus(); return; }
                await api('/api/videos/' + state.videoId + '/title', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ title })
                });
                dialog.close();
                await loadVideos();
                state.videoTitle = title;
                renderVideoDetail();
                toast('タイトルを変更しました。');
                return;
            }
            if (state.entryMode === 'youtube') {
                await api('/api/videos', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ source: url, saveVideo: entrySaveVideo.checked })
                });
                dialog.close();
                state.view = 'videos';
                location.hash = '#videos';
                await loadVideos();
                render();
                toast('追加しました。解析の進み具合はこの画面に出ます。');
            } else {
                const title = entryTitle.value.trim() || url;
                await api('/api/tetofu', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ kind: state.entryMode, title, url })
                });
                dialog.close();
                await loadTetofu();
                render();
                toast('テト譜を登録しました。');
            }
        } catch (error) {
            toast(error.message);
        }
    });

    async function addFromFile(button) {
        const original = button.textContent;
        button.disabled = true;
        button.textContent = '選択中…';
        try {
            const picked = await api('/api/videos/pick', { method: 'POST' });
            if (picked && picked.path) {
                await api('/api/videos', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ source: picked.path })
                });
                await loadVideos();
                render();
                toast('追加しました。解析の進み具合はこの画面に出ます。');
            }
        } catch (error) {
            toast(error.message);
        } finally {
            button.disabled = false;
            button.textContent = original;
        }
    }

    // ---- ポーリング -------------------------------------------------------
    function hasActiveVideos() {
        return state.videos.some(video => video.status === 'queued' || video.status === 'analyzing');
    }

    // 入力・選択の途中かどうか（再描画すると操作が途切れる）
    function isUserTyping() {
        const element = document.activeElement;
        if (!element) return false;
        const tag = element.tagName;
        if (tag !== 'INPUT' && tag !== 'SELECT' && tag !== 'TEXTAREA') return false;
        return Boolean(element.closest('.lab-video-filters, .ppt-name-groups, .lab-dialog, dialog'));
    }

    function syncPolling() {
        if (state.pollTimer) {
            clearTimeout(state.pollTimer);
            state.pollTimer = null;
        }
        if (!hasActiveVideos() || document.hidden) return;
        state.pollTimer = setTimeout(async () => {
            state.pollTimer = null;
            try {
                const changed = await loadVideos();
                // 選択中のプルダウンや入力中は作り直さない（勝手に閉じるのを防ぐ）
                if (isUserTyping()) { syncPolling(); return; }
                if (changed && state.view === 'videos') renderVideos();
            } catch (error) {
                /* 失敗しても静かに次の周期へ */
            }
            syncPolling();
        }, 2000);
    }

    document.addEventListener('visibilitychange', () => {
        if (document.hidden) {
            if (state.pollTimer) {
                clearTimeout(state.pollTimer);
                state.pollTimer = null;
            }
        } else {
            syncPolling();
        }
    });

    // ---- ルーティング -----------------------------------------------------
    function hashParts() {
        return (location.hash || '#videos').replace('#', '').split('/');
    }

    function viewFromHash() {
        const name = hashParts()[0];
        return ['videos', 'tetofu', 'tags', 'names', 'video', 'settings'].includes(name) ? name : 'videos';
    }

    async function route() {
        state.view = viewFromHash();
        if (hashParts()[1] === 'matches') state.mode = 'matches';
        if (hashParts()[1] === 'videos') state.mode = 'videos';
        if (hashParts().length === 1) state.mode = 'videos';
        let tagParam = '';
        if (state.view === 'tags' && hashParts()[1]) {
            try { tagParam = decodeURIComponent(hashParts()[1]); } catch (error) { tagParam = hashParts()[1]; }
        }
        state.tagFocus = tagParam;
        if (state.tagFocus) {
            state.tagSelected = new Set(state.tagFocus.split('+').map(name => name.trim()).filter(Boolean));
            state.tagQuery = '';
        }
        const parts = hashParts();
        try {
            if (state.view === 'video') {
                const videoId = parts[1] || '';
                if (videoId !== state.editsVideoId) {
                    state.edits = new Map();
                    state.editsVideoId = videoId;
                    state.matches = [];
                    state.currentMatch = null;
                    state.pages = [];
                    state.pageIndex = 0;
                    // 動画が変わったら試合の絞り込みは解除する（Lab と同じ）
                    state.matchFilters = { query: '', player: '', opponent: '' };
                }
                // タグ画面のプレイヤーから来たときは、そのプレイヤーを1つ目に入れて始める
                if (state.pendingPlayerFilter) {
                    state.matchFilters = { query: '', player: state.pendingPlayerFilter, opponent: '' };
                    state.pendingPlayerFilter = '';
                }
                if (!videoId) {
                    state.view = 'videos';
                    if (location.hash !== '#videos') location.hash = '#videos';
                    await loadVideos();
                } else {
                    state.videoId = videoId;
                    if (state.featureList.length && !state.featureList.includes(videoId)) {
                        stopFeature();
                    }
                    await loadVideos();
                    await loadMatches(videoId);
                    // 試合カードに試合全体のテト譜のタグを出すため
                    try { await loadTetofu(); } catch (error) { /* タグが出ないだけ */ }
                }
            } else if (state.view === 'names') {
                const videoId = parts[1] || '';
                if (videoId !== state.editsVideoId) {
                    state.edits = new Map();
                    state.editsVideoId = videoId;
                    state.matches = [];
                    state.matchGroups = [];
                    state.currentMatch = null;
                    state.pages = [];
                    state.pageIndex = 0;
                }
                if (!videoId) {
                    state.view = 'videos';
                    if (location.hash !== '#videos') location.hash = '#videos';
                    await loadVideos();
                } else {
                    state.videoId = videoId;
                    await loadMatches(videoId);
                }
            } else if (state.view === 'tetofu') {
                await loadTetofu();
                // 「元の動画」の題名・動画での絞り込み・プレイヤータグの色分けに使う
                try { await loadVideos(); } catch (error) { /* 無くても一覧は出す */ }
            }
            else if (state.view === 'tags') {
                await loadTags();
                await loadVideos();
                await loadTetofu();
            } else if (state.view === 'settings') {
                await loadSettings();
            }
            else await loadVideos();
        } catch (error) {
            toast(error.message);
        }
        render();
    }

    // 動画の画面を離れたら再生を止める（YouTube の音が裏で流れ続けないように）
    function stopDetailPlayback() {
        if (state.ytPlayer) {
            try { state.ytPlayer.pauseVideo?.(); } catch (error) { /* 止められなくても次で破棄する */ }
            try { state.ytPlayer.destroy?.(); } catch (error) { /* 破棄できなくてもよい */ }
        }
        state.ytPlayer = null;
        state.ytReady = false;
        document.querySelectorAll('video, audio').forEach(media => {
            try { media.pause(); } catch (error) { /* 止められない要素は無視 */ }
        });
    }

    window.addEventListener('hashchange', () => {
        // 画面を移ったら、Lab の中で開いているパネルは閉じる
        closeInlineFrame();
        if (viewFromHash() !== 'video') stopDetailPlayback();
        route();
    });
    // 動作確認用: 連続再生と盤面同期の状態をまとめて見る
    window.__pptDebug = () => ({
        view: state.view,
        videoId: state.videoId,
        matches: state.matches.length,
        matchFilters: { ...state.matchFilters },
        matchFiltered: detailMatches(state.matches).length,
        currentMatch: state.currentMatch ? state.currentMatch.index : 0,
        currentStart: state.currentMatch ? Number(state.currentMatch.start) || 0 : 0,
        currentEnd: state.currentMatch ? Number(state.currentMatch.end) || 0 : 0,
        pages: state.pages.length,
        pagesWithP2: state.pages.filter(page => page && page.p2).length,
        pageIndex: state.pageIndex,
        follow: state.follow,
        syncing: Boolean(state.syncing),
        ytReady: Boolean(state.ytReady),
        videoTime: videoTime(),
        pendingSeek: state.pendingSeek,
        feature: {
            index: state.feature.index,
            segments: state.feature.segments.length,
            pending: state.featurePending ? state.featurePending.matchIndex : 0,
            current: state.feature.segments[state.feature.index] || null
        },
        edits: state.edits.size,
        nameGroups: state.nameGroups ? state.nameGroups.size : -1
    });
    window.__pptApply = (index, side, value) => { try { applyName(state.matches.find(r => r.index === index), side, value); return JSON.stringify({ edits: state.edits.size }); } catch (error) { return 'ERR ' + error.message + ' | ' + error.stack; } };
    // 同期の検証用（画面には出さない）
    window.__pptSeek = seconds => { seekVideoTo(Number(seconds), { pause: false }); return Boolean(state.bridge && state.bridge.ready); };
    window.__pptPlay = () => {
        state.ytPlayer?.playVideo?.();
        const media = document.getElementById('detail-video');
        if (media) media.play?.().catch?.(() => {});
        return videoState();
    };
    // メニューの外をクリックしたら閉じる
    document.addEventListener('click', event => {
        document.querySelectorAll('.lab-save-menu.open, .ppt-edit-menu.open').forEach(menu => {
            if (!menu.contains(event.target)) menu.classList.remove('open');
        });
        closeTetofuMenu();
    });
    // 動画詳細のキーボード操作: ←/→ 局面、Shift+←/→ 試合、Space 再生/停止、S シミュレータ
    document.addEventListener('keydown', event => {
        if (state.view !== 'video' || event.defaultPrevented) return;
        if (event.ctrlKey || event.altKey || event.metaKey) return;
        const target = event.target;
        if (target && (target.isContentEditable ||
            /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName || ''))) return;
        const overlay = document.getElementById('ppt-inline-frame');
        if (overlay && !overlay.hidden) return;
        const press = name => {
            const button = document.querySelector('[data-shortcut="' + name + '"]');
            if (button && !button.disabled) button.click();
        };
        if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
            const forward = event.key === 'ArrowRight';
            press(event.shiftKey ? (forward ? 'match-next' : 'match-prev')
                                 : (forward ? 'page-next' : 'page-prev'));
        } else if (event.key === ' ' && !event.shiftKey) {
            togglePlayback();
        } else if ((event.key === 's' || event.key === 'S') && !event.shiftKey) {
            openSimulatorHere();
        } else {
            return;
        }
        event.preventDefault();
    });

    function togglePlayback() {
        const media = document.getElementById('detail-video');
        if (media) {
            if (media.paused) media.play().catch(() => {});
            else media.pause();
            return;
        }
        if (state.ytReady && state.ytPlayer) {
            try {
                if (state.ytPlayer.getPlayerState?.() === 1) state.ytPlayer.pauseVideo();
                else state.ytPlayer.playVideo();
            } catch (error) { /* 操作できなくても画面はそのまま */ }
        }
    }

    document.addEventListener('keydown', event => {
        if (event.key !== 'Escape') return;
        // Lab の中で開いているパネルがあれば、それを閉じる（最前面を優先）
        const overlay = document.getElementById('ppt-inline-frame');
        if (overlay && !overlay.hidden) {
            closeInlineFrame();
            return;
        }
        closeTetofuMenu();
    });
    window.addEventListener('message', event => {
        const bridge = state.bridge;
        if (!bridge) return;
        if (event.origin !== VIEWER_ORIGIN) return;
        if (event.source !== bridge.frame.contentWindow) return;
        bridge.receive(event.data);
    });
    document.getElementById('app-logo').addEventListener('click', event => {
        event.preventDefault();
        location.hash = '#videos';
        route();
    });

    // ---- キャッシュの掃除 -------------------------------------------------
    // 同じオリジンの Service Worker（/sw.js）は、ビルドによっては
    // /ppt-lab/ の古い index.html・app.js・CSS を控えから返す。起動時に
    // コードだけ捨てて、次の読み込みで必ず取り直す（index.html の
    // インラインスクリプトでも同じことをしている。こちらは保険）。
    // 解析結果の JSON（/ppt-lab/data/）は重いので消さない。
    function isLabCode(path) {
        return path.startsWith('/ppt-lab/') && !path.startsWith('/ppt-lab/data/') &&
            /\.(html|js|css|webmanifest)$/.test(path);
    }

    function purgeStaleLabCaches() {
        if (!('caches' in window)) return;
        caches.keys().then(keys => Promise.all(keys.map(async key => {
            try {
                const cache = await caches.open(key);
                const requests = await cache.keys();
                await Promise.all(requests
                    .filter(request => isLabCode(new URL(request.url).pathname))
                    .map(request => cache.delete(request)));
            } catch (error) {
                /* 消せなくても表示は続ける */
            }
        }))).catch(() => { /* 失敗しても起動は続ける */ });
    }

    // ---- 起動 -------------------------------------------------------------
    (async function start() {
        purgeStaleLabCaches();
        // 起動時に URL に画面の指定があったか（無ければ設定「起動時に開く画面」に従う）
        const openedWithHash = Boolean(location.hash);
        // ビューワー（F/）は動画詳細を開くまで読み込まない。テト譜や一覧を軽く保つ。
        await loadEnvironment();
        try { await loadTags(); } catch (error) { /* 起動時の失敗は無視 */ }
        await loadSettings();
        if (!openedWithHash) {
            let last = '';
            try { last = localStorage.getItem('pptLastView') || ''; } catch (error) { last = ''; }
            const useLast = String(setting('startView') || 'videos') === 'last' && last;
            const target = useLast ? last : '#videos';
            // hashchange で route() が二重に走らないよう、履歴を置き換えるだけにする
            history.replaceState(null, '', target);
        }
        // 設定「既定の再生速度」（再生速度ボタンを押すまではこれを使う）
        if (!state.playbackRateChosen) state.playbackRate = Number(setting('playbackRate')) || 1;
        await route();
    })();
})();
