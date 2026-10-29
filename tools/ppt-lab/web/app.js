/* PPT Lab の画面。見た目は Lab（base.css / lab-enhancements.css）の
   クラスだけで組み立て、独自の色や角丸は増やさない。 */
(function () {
    'use strict';

    const mainView = document.getElementById('main-view');
    const statusLine = document.getElementById('lab-status');
    const searchBox = document.getElementById('global-search');
    const dialog = document.getElementById('entry-dialog');
    const dialogTitle = document.getElementById('entry-dialog-title');
    const dialogNote = document.getElementById('entry-dialog-note');
    const entryTitle = document.getElementById('entry-title');
    const entryUrl = document.getElementById('entry-url');

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
        matchCache: new Map(),
        matchLoading: false,
        matchFailed: new Set(),
        pendingMatch: 0,
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
        matches: [],
        replays: new Map(),
        edits: new Map(),
        editsVideoId: '',
        nameGroups: new Map(),
        settings: {},
        settingsTab: '一般',
        settingsSchema: [],
        groups: new Set()
    };

    const STATUS_LABEL = {
        queued: '待機中',
        analyzing: '解析中',
        'needs-names': 'プレイヤー名を確認',
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
        return Array.from(state.tagSelected || []);
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
    function tagLink(name) {
        const link = node('a', name, 'tag');
        link.href = '#tags/' + encodeURIComponent(name);
        link.addEventListener('click', event => {
            event.stopPropagation();
            event.preventDefault();
            location.hash = '#tags/' + encodeURIComponent(name);
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

    function playerNames(match) {
        const wrap = node('span', null, 'lab-match-players');
        const names = [match.p1, match.p2].filter(Boolean);
        names.forEach((name, index) => {
            if (index) wrap.append(document.createTextNode(' / '));
            if (match.winner === (index === 0 ? 'p1' : 'p2')) {
                wrap.append(crownMark(match.winnerStatus !== 'ok'));
            }
            wrap.append(document.createTextNode(name));
        });
        return wrap;
    }
    const DEFAULT_THUMB = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(
        '<svg xmlns="http://www.w3.org/2000/svg" width="640" height="360" viewBox="0 0 640 360">' +
        '<rect width="640" height="360" fill="white"/>' +
        '<text x="320" y="198" text-anchor="middle" font-family="Arial,sans-serif" font-size="72" ' +
        'font-weight="700" letter-spacing="9" fill="#79877f">LAB</text></svg>');

    function labThumbnail(video) {
        const id = youtubeId(video.source);
        return id ? 'https://i.ytimg.com/vi/' + id + '/mqdefault.jpg' : DEFAULT_THUMB;
    }

    // 動画もテト譜と同じ「正方形タイル」で並べる
    function renderLabCard(container, video, matchCount) {
        const card = node('article', null, 'card ppt-tile ppt-video-tile');
        const thumb = node('div', null, 'ppt-tile-preview');
        const image = node('img', null, 'ppt-tile-thumb');
        image.src = labThumbnail(video);
        image.alt = '';
        image.loading = 'lazy';
        image.decoding = 'async';
        image.onerror = () => { image.onerror = null; image.src = DEFAULT_THUMB; };
        thumb.append(image);
        if (video.status === 'needs-names') {
            thumb.append(node('span', 'プレイヤー名を確認', 'ppt-tile-badge'));
        } else if (video.status === 'queued' || video.status === 'analyzing') {
            thumb.append(node('span', '解析中 ' + Math.round(video.progress || 0) + '%', 'ppt-tile-badge'));
        } else if (video.status === 'failed') {
            thumb.append(node('span', '失敗', 'ppt-tile-badge ppt-tile-badge-error'));
        }
        card.append(thumb);

        const title = node('h3', null, 'card-title');
        const link = node('a', video.title, 'card-title-action');
        link.href = '#video/' + video.id;
        link.addEventListener('click', event => event.stopPropagation());
        title.append(link);
        card.append(title);

        const players = (video.players || []).slice(0, 3).join(' / ');
        card.append(node('p', players || 'プレイヤー未登録', 'ppt-tile-players'));

        const tagRow = node('div', null, 'lab-video-tags');
        const allTags = video.tags || [];
        allTags.slice(0, 8).forEach(tag => tagRow.append(tagLink(tag)));
        if (allTags.length > 8) tagRow.append(node('span', '+' + (allTags.length - 8), 'ppt-tile-more'));
        card.append(tagRow);

        const shown = Number.isFinite(matchCount) ? matchCount : (video.matchCount || 0);
        const note = video.status === 'failed' ? '解析に失敗'
            : shown + '試合';
        card.append(node('div', note, 'lab-analysis-count'));

        card.addEventListener('click', event => {
            if (event.target.closest('a,button,input,select,textarea')) return;
            if (window.getSelection()?.toString()) return;
            location.hash = '#video/' + video.id;
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
            const text = [video.title, ...tags, ...players].join(' ');
            if (!matchQuery(ast, tags, text)) return false;
            if (!hasAllTags(tags, wanted)) return false;
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

    function sortEntries(entries) {
        const mode = state.filters.sort;
        return entries.slice().sort((a, b) => {
            if (mode === 'title') return String(a.title).localeCompare(String(b.title), 'ja');
            if (mode === 'oldest') return String(a.createdAt).localeCompare(String(b.createdAt));
            return String(b.createdAt).localeCompare(String(a.createdAt));
        });
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
        if (one && two && one === two) return row.p1 === one && row.p2 === two;
        const names = [row.p1, row.p2].filter(Boolean);
        if (one && !names.includes(one)) return false;
        if (two && !names.includes(two)) return false;
        return true;
    }

    // Lab と同じ勝敗判定: プレイヤー未指定なら素通し、未確定は winner 無し or status !== ok
    function resultOk(row) {
        const want = state.filters.result;
        const player = state.filters.player;
        if (!want || !player) return true;
        const names = [row.p1, row.p2].filter(Boolean);
        const side = names[0] === player ? 'p1' : names[1] === player ? 'p2' : '';
        if (!side) return false;
        if (want === 'review') return !row.winner || row.winnerStatus !== 'ok';
        const won = row.winner === side;
        return want === 'win' ? won : !won;
    }

    // 試合一覧と連続再生で同じ絞り込みを使う（件数がずれないように）
    function matchFilterOk(row) {
        return pairOk(row) && resultOk(row);
    }

    function renderMatchCard(container, video, row, order) {
        const card = node('article', null, 'card lab-video-match-card');
        const meta = node('div', null, 'card-meta');
        const badge = node('span', '動画' + circled(order), 'lab-source-badge');
        badge.title = '出典: ' + video.title;
        meta.append(badge, node('span', video.title), node('span', formatDate(video.createdAt)));
        card.append(meta);

        const title = node('h3', null, 'card-title');
        const link = node('a', null, 'card-title-action');
        link.href = '#video/' + video.id;
        link.append(document.createTextNode('試合 ' + row.index + ' · '));
        const names = [row.p1, row.p2].filter(Boolean);
        if (!names.length) link.append(document.createTextNode('プレイヤー未登録'));
        names.forEach((name, index) => {
            if (index) link.append(document.createTextNode(' / '));
            if (row.winner === (index === 0 ? 'p1' : 'p2')) {
                link.append(crownMark(row.winnerStatus !== 'ok'));
            }
            link.append(document.createTextNode(name));
        });
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
        (video.tags || []).filter(tag => tag !== '動画解析' && tag !== row.p1 && tag !== row.p2)
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
        header.append(node('h2', '動画研究', 'section-title'));
        // 動画の追加は Lab の「動画の管理」と同じ位置（見出しの右）に置く
        const addBar = node('div', null, 'lab-inline-actions');
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
        bind('勝敗', 'result', [['', 'すべて'], ['win', '勝ち'], ['lost', '負け'], ['review', '未確定']]);
        const sortOptions = [['newest', '新しい順'], ['oldest', '古い順'], ['title', 'タイトル順'], ['matches', '試合数順']];
        const sortDefault = String(setting('sortOrder') || 'new');
        bind('並び順', 'sort', sortOptions, ['newest', 'oldest', 'title', 'matches'].includes(sortDefault) ? sortDefault : 'newest');

        const clear = node('button', '絞り込みを解除', 'btn-outline');
        clear.type = 'button';
        clear.addEventListener('click', () => {
            state.filters = { query: '', player: '', opponent: '', tag: '', result: '', sort: 'newest' };
            state.page = 0;
            renderVideos();
        });
        controls.append(clear);
        root.append(controls);

        const featureBar = node('div', null, 'lab-inline-actions');
        const feature = node('button', '連続再生', 'btn-outline');
        feature.type = 'button';
        feature.title = '絞り込み結果を先頭から続けて再生します';
        feature.addEventListener('click', () => playSequentially());
        featureBar.append(feature);
        root.append(featureBar);

        const entries = sortEntries(filterVideos(state.videos));
        // プレイヤーで絞るときは、動画一覧でも試合データが要る
        const pairWanted = Boolean(state.filters.player || state.filters.opponent);

        // 試合一覧は全動画を横断してフラットに並べる
        let rows;
        if (state.mode === 'matches') {
            rows = [];
            entries.forEach(video => {
                const cached = state.matchCache.get(video.id);
                if (!cached) return;
                cached.forEach(row => {
                    if (!matchFilterOk(row)) return;
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
                    : 'まだ動画がありません。上のボタンから追加してください。', 'lab-help-text'));
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
    const BOARD_COLORS = {
        I: '#43d3d3', O: '#e8c53a', T: '#a45fd3', S: '#57c85a',
        Z: '#e05a5a', J: '#4a7fe0', L: '#e0913a', G: '#8a8f92'
    };

    function boardCells(board) {
        if (!board) return '';
        if (Array.isArray(board)) {
            if (Array.isArray(board[0])) return board.map(row => row.join('')).join('');
            return board.join('');
        }
        return String(board);
    }

    function tetofuPages(item) {
        const hash = String(item.url || '').split('#')[1];
        if (!hash) return null;
        try {
            const binary = atob(hash.replace(/-/g, '+').replace(/_/g, '/'));
            const json = JSON.parse(new TextDecoder().decode(Uint8Array.from(binary, ch => ch.charCodeAt(0))));
            if (!window.TetrisEventCodec || !window.TetrisEventCodec.isEventReplay(json)) return null;
            const decoded = window.TetrisEventCodec.decodeCollection(json, { compactBoards: true });
            const target = decoded.cases[decoded.currentCase || 0];
            return (target && target.pages) || null;
        } catch (error) {
            return null;
        }
    }

    function drawBoard(ctx, cells, x, y, size) {
        const width = 10;
        // 盤面は 40 行。見えるのは下 20 行だけ。
        const rows = 40;
        const offset = (rows - 20) * width;
        const height = 20;
        ctx.fillStyle = '#101c20';
        ctx.fillRect(x, y, width * size, height * size);
        for (let row = 0; row < height; row += 1) {
            for (let col = 0; col < width; col += 1) {
                const value = (cells[offset + row * width + col] || '').toUpperCase();
                if (!value || value === '_' || value === '.' || value === ' ') continue;
                ctx.fillStyle = BOARD_COLORS[value] || '#6b7c7e';
                ctx.fillRect(x + col * size + 1, y + row * size + 1, size - 2, size - 2);
            }
        }
    }

    // 保存したURLを、そのままビューワーで開ける形にする（操作は無効）
    function viewerThumbUrl(url) {
        const value = siteUrl(url);
        if (!value) return 'about:blank';
        const parts = String(value).split('#');
        if (parts.length < 2) return value;
        const base = parts[0].replace(/\?.*$/, '');
        const page = (String(value).match(/[?&]page=(\d+)/) || [])[1] || '1';
        return base + '?page=' + page + '&labViewer=1#' + parts[1];
    }

    // ビューワーの実寸（1200x574）を枠に合わせて縮小する
    function fitTetofuPreview(wrap, frame) {
        const apply = () => {
            const width = wrap.clientWidth || 280;
            const scale = width / 1200;
            frame.style.transform = 'scale(' + scale + ')';
            wrap.style.height = Math.round(574 * scale) + 'px';
        };
        apply();
        if (window.ResizeObserver) new ResizeObserver(apply).observe(wrap);
    }

    function drawTetofuPreview(canvas, item) {
        const pages = tetofuPages(item);
        const requested = Number((String(item.url).match(/[?&]page=(\d+)/) || [])[1] || 1);
        const page = pages && pages.length
            ? pages[Math.max(0, Math.min(requested - 1, pages.length - 1))]
            : null;
        if (window.PPTViewerRender) {
            // エディタと同じ描画（F/app/30-viewer.js の手順）で描く
            window.PPTViewerRender.render(canvas, page, { players: ['p1', 'p2'] });
            return;
        }
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        ctx.fillStyle = '#eef3f2';
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.fillStyle = '#8aa0a2';
        ctx.font = '13px system-ui, sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText(pages ? 'プレビューなし' : 'プレビューなし', canvas.width / 2, canvas.height / 2);
    }

    function renderTetofuCard(container, item) {
        const card = node('article', null, 'card ppt-tetofu-card');
        const previewWrap = node('div', null, 'ppt-tetofu-preview-wrap');
        const preview = node('iframe', null, 'ppt-tetofu-preview');
        preview.setAttribute('tabindex', '-1');
        preview.setAttribute('aria-hidden', 'true');
        preview.setAttribute('loading', 'lazy');
        preview.setAttribute('scrolling', 'no');
        preview.src = viewerThumbUrl(item.url);
        previewWrap.append(preview);
        card.append(previewWrap);
        requestAnimationFrame(() => fitTetofuPreview(previewWrap, preview));

        const title = node('h3', null, 'card-title');
        const link = node('a', item.title, 'card-title-action');
        link.href = item.url;
        link.target = '_blank';
        link.rel = 'noopener';
        title.append(link);
        card.append(title);

        const kind = item.kind === 'official' ? '本家テト譜'
            : item.kind === 'editor' ? 'エディタ' : 'シミュレータ';
        card.append(node('div', kind, 'card-desc'));

        const tagRow = node('div', null, 'lab-video-tags');
        (item.tags || []).forEach(tag => tagRow.append(tagLink(tag)));
        card.append(tagRow);

        const actions = node('div', null, 'lab-inline-actions');
        const tagButton = node('button', 'タグ', 'btn-outline');
        tagButton.type = 'button';
        tagButton.addEventListener('click', event => {
            event.stopPropagation();
            openTagDialog({ id: item.id, kind: 'tetofu', title: item.title, tags: item.tags || [], autoTags: [] });
        });
        actions.append(tagButton);
        const removeButton = node('button', '削除', 'btn-outline');
        removeButton.type = 'button';
        removeButton.addEventListener('click', async event => {
            event.stopPropagation();
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
        actions.append(removeButton);
        card.append(actions);
        container.append(card);
    }
    function renderTetofu() {
        const root = node('section', null, 'lab-video-library');
        mainView.replaceChildren(root);

        const header = node('div', null, 'lab-video-library-heading');
        header.append(node('h2', 'テト譜・盤面アーカイブ', 'section-title'));
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
            state.tetofuQuery = search.value;
            renderTetofu();
        });
        searchWrap.append(search);
        controls.append(searchWrap);

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
            renderTetofu();
        });
        controls.append(clear);
        root.append(controls);

        const ast = parseQuery(state.tetofuQuery);
        const wanted = state.view === 'tags' ? selectedTags() : [];
        const items = state.tetofu.filter(item => {
            const tags = item.tags || [];
            if (state.tetofuTag && !tags.includes(state.tetofuTag)) return false;
            if (!hasAllTags(tags, wanted)) return false;
            return matchQuery(ast, tags, item.title + ' ' + item.url);
        });
        root.append(node('p', items.length + 'テト譜', 'lab-video-count'));

        const list = node('div', null, 'ppt-tile-grid');
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
        header.append(node('h2', 'タグ一覧', 'section-title'));
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

        const selected = state.tagSelected || new Set();
        const query = (state.tagQuery || '').trim().toLowerCase();
        // 検索欄が空なら全タグを出す
        const simple = query && !/[()]|\\band\\b|\\bor\\b|&&|\\|\\|/.test(query) ? query : '';
        const tags = state.tags.filter(tag => !simple || tag.name.toLowerCase().includes(simple));
        const list = node('div', null, 'ppt-tag-cloud');
        if (!tags.length) {
            list.append(node('p', state.tags.length
                ? '条件に合うタグはありません。'
                : 'タグがありません。動画やテト譜に付けるラベルを作れます。', 'lab-help-text'));
        }
        tags.forEach(tag => {
            const active = selected.has(tag.name);
            const pill = node('button', tag.name + ' (' + (tag.count || 0) + ')',
                'tag ppt-tag-pill' + (active ? ' active' : ''));
            pill.type = 'button';
            pill.title = 'クリックで絞り込み（複数選択でAND）';
            pill.addEventListener('click', () => {
                if (selected.has(tag.name)) selected.delete(tag.name);
                else selected.add(tag.name);
                state.tagSelected = selected;
                const names = Array.from(selected);
                history.replaceState(null, '', '#tags' + (names.length ? '/' + names.map(encodeURIComponent).join('+') : ''));
                render();
            });
            list.append(pill);
        });
        root.append(list);

        if (!selected.size && !query) return;

        // 選んだタグ（AND）と条件式に合う項目を並べる
        const ast = parseQuery(state.tagQuery);
        const wanted = state.view === 'tags' ? selectedTags() : [];
        const videos = state.videos.filter(video => {
            const tags = video.tags || [];
            const text = [video.title, ...(video.players || [])].join(' ');
            return hasAllTags(tags, wanted) && matchQuery(ast, tags, text);
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
            searchBox.value = '';
            render();
        });
        const bar = node('div', null, 'lab-inline-actions');
        bar.append(node('span', '該当 ' + total + ' 件', 'lab-video-count'), clear);
        root.append(bar);

        const items = node('div', null, 'ppt-tile-grid');
        if (!total) {
            items.append(node('p', '条件に合う項目はありません。', 'lab-help-text'));
        }
        videos.forEach(video => renderLabCard(items, video));
        tetofu.forEach(item => renderTetofuCard(items, item));
        root.append(items);
    }

    // ---- 設定 -------------------------------------------------------------
    const SETTING_DEFAULTS = {
        theme: 'light',
        simulatorBase: 'https://selmtoe.github.io/Tetris_Simulator/',
        openLinks: 'newTab',
        analysisWorkers: '1'
    };

    function setting(key) {
        return (state.settings && state.settings[key]) || SETTING_DEFAULTS[key] || '';
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

    const SETTINGS_TABS = ['一般', '表示', '解析', '名前とタグ', 'データ管理'];

    function settingsTab() {
        return state.settingsTab && SETTINGS_TABS.includes(state.settingsTab) ? state.settingsTab : '一般';
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
            select.addEventListener('change', () => {
                saveSetting(item.key, item.type === 'int' ? Number(select.value) : select.value);
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

    function renderSettings() {
        const root = node('section', null, 'lab-video-library');
        mainView.replaceChildren(root);
        const header = node('div', null, 'lab-video-library-heading');
        header.append(node('h2', '設定', 'section-title'));
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
            schema.filter(item => item.group === group).forEach(item => {
                const row = node('label', null, 'ppt-setting-row');
                row.append(node('span', item.label, 'ppt-setting-label'));
                row.append(settingControl(item));
                if (item.note) row.append(node('span', item.note, 'ppt-setting-note'));
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
        const auto = new Set(target.autoTags || []);
        picker.replaceChildren();
        // 既存タグ＋この対象が今持っているタグ（プレイヤー名など）をまとめて出す
        const names = new Map();
        state.tags.forEach(tag => names.set(tag.name, tag.count || 0));
        chosen.forEach(name => { if (!names.has(name)) names.set(name, 0); });
        Array.from(names.keys()).sort().forEach(name => {
            const chip = node('button', name, 'tag' + (chosen.has(name) ? ' active' : ''));
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
    const SITE_BASE = new URL('../', location.href).href;
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
    function simulatorLink(url) {
        if (!url || !url.includes('#')) return '';
        return SIMULATOR_BASE + 'hub/?entry=simulator#' + url.split('#')[1];
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

    function setViewerPage(page) {
        const viewer = document.getElementById('detail-viewer');
        if (!viewer || !state.currentUrls || !state.currentUrls.combined) return;
        if (viewer.dataset.page === String(page)) return;
        // postMessage が使えるなら、そちらが来るまで待つ（二重読み込みを避ける）
        if (state.bridge && !state.bridge.ready && Date.now() < (state.bridgeGrace || 0)) return;
        const now = Date.now();
        if (now - (state.lastReloadAt || 0) < 700) return;   // 連続リロードを避ける
        state.lastReloadAt = now;
        viewer.dataset.page = String(page);
        viewer.src = withPage(siteUrl(state.currentUrls.combined), page + 1);
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
        if (match && state.bridge && state.bridge.ready) {
            state.bridge.show(match, pages, page);
            return;
        }
        if (match && state.currentUrls && state.currentUrls.combined) {
            setViewerPage(page);
            return;
        }
        const viewer = document.getElementById('detail-viewer');
        if (viewer && state.currentUrls && state.currentUrls.combined) {
            viewer.src = withPage(siteUrl(state.currentUrls.combined), page + 1);
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
        const links = document.querySelectorAll('.lab-match-tools > a.btn-outline');
        if (links[0]) links[0].href = simulatorLink(urls.combined) || '#';
        if (links[1]) links[1].href = siteUrl(urls.combined) || '#';
        if (state.bridge) state.bridge.show(row, state.pages, 0);
        setTimeout(() => {
            const frame = document.getElementById('detail-viewer');
            if (!frame || frame.dataset.loaded === 'true') return;
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
            const page = kind === 'all' ? pages : current;
            const url = pinPage(target, page);
            if (!url) throw new Error('この盤面のリンクを作れませんでした');
            const label = kind === 'match' ? '試合' : kind === 'all' ? '全体盤面'
                : kind.toUpperCase() + '盤面';
            const note = kind === 'all' ? '（決着時）' : '（局面 ' + page + '/' + pages + '）';
            const title = ((row.p1 || '?') + ' / ' + (row.p2 || '?') + ' · ' + label + note).slice(0, 200);
            // 先にタグを選んでもらってから保存する
            openTagDialog({
                id: '',
                kind: 'tetofu',
                title: label + note + ' を保存',
                tags: [],
                autoTags: []
            }, {
                onSave: async tags => {
                    const created = await api('/api/tetofu', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ kind: 'simulator', title, url })
                    });
                    if (tags.length) {
                        await api('/api/tags/apply', {
                            method: 'POST',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({ target: created.id, tags })
                        });
                    }
                    await loadTetofu();
                    notify(label + note + ' をテト譜に保存しました。');
                }
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
            wrapper.append(media);
        } else {
            wrapper.append(node('p', 'この動画はこの画面では再生できません。', 'lab-help-text'));
        }
        left.append(wrapper);

        const controls = node('div', null, 'video-controls');
        [['x0.25', 0.25], ['x0.5', 0.5], ['x1.0', 1], ['x1.5', 1.5]].forEach(([label, rate]) => {
            const button = node('button', label, 'rate-btn' + (rate === 1 ? ' active' : ''));
            button.type = 'button';
            button.addEventListener('click', () => {
                controls.querySelectorAll('.rate-btn').forEach(item => item.classList.remove('active'));
                button.classList.add('active');
                const media = document.getElementById('detail-video');
                if (media) media.playbackRate = Number(rate) || Number(setting('playbackRate')) || 1;
            });
            controls.append(button);
        });
        const simulatorButton = node('button', 'シミュレータを開く', 'rate-btn');
        simulatorButton.type = 'button';
        simulatorButton.style.borderColor = 'var(--accent-color)';
        simulatorButton.style.color = 'var(--accent-color)';
        simulatorButton.style.fontWeight = 'bold';
        simulatorButton.addEventListener('click', async () => {
            if (!state.currentMatch) return;
            try {
                const urls = await replayUrls(state.currentMatch.index);
                window.open(simulatorLink(urls.combined), '_blank', 'noopener');
            } catch (error) {
                toast(error.message);
            }
        });
        controls.append(simulatorButton);
        left.append(controls);

        const heading = node('div', null, 'lab-video-heading');
        const identity = node('div', null, 'lab-video-identity');
        identity.append(node('h1', video ? video.title : '動画', 'article-title'));
        heading.append(identity);
        const meta = node('div', null, 'lab-video-meta');
        meta.append(node('span', formatDate(video && video.createdAt)));
        if (video && video.matchCount) meta.append(node('span', '解析 ' + video.matchCount + '試合'));
        heading.append(meta);
        const tagRow = node('div', null, 'lab-video-tags');
        (video && video.tags || []).forEach(tag => tagRow.append(tagLink(tag)));
        heading.append(tagRow);
        const headingActions = node('div', null, 'lab-video-heading-actions');
        // 編集メニュー（鉛筆アイコン）: 名前 / タグ / タイトル
        const editMenu = node('div', null, 'lab-save-menu ppt-edit-menu');
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
        const tagAction = node('button', 'タグ', 'btn-outline');
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
        const deleteTagAction = node('button', 'タグを削除', 'btn-outline');
        deleteTagAction.type = 'button';
        deleteTagAction.addEventListener('click', () => {
            editMenu.classList.remove('open');
            openTagDialog({
                id: state.videoId,
                kind: 'video',
                title: video ? video.title : '',
                tags: (video && video.tags) || [],
                autoTags: (video && video.autoTags) || []
            }, { mode: 'delete' });
        });
        const deleteVideoAction = node('button', '動画を削除', 'btn-outline');
        deleteVideoAction.type = 'button';
        deleteVideoAction.addEventListener('click', async () => {
            editMenu.classList.remove('open');
            if (!window.confirm('この動画を一覧から削除しますか？（解析ファイルは残ります）')) return;
            try {
                await api('/api/videos/' + state.videoId, { method: 'DELETE' });
                state.videoId = '';
                await loadVideos();
                location.hash = '#videos';
                toast('動画を削除しました。');
            } catch (error) {
                toast(error.message);
            }
        });
        editList.append(nameAction, tagAction, deleteTagAction, renameAction, deleteVideoAction);
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
        const rail = node('div', null, 'lab-match-timeline');
        const railList = node('div', null, 'lab-match-list');
        matches.forEach(row => {
            const item = node('button', null, 'lab-match-row');
            item.type = 'button';
            item.dataset.index = String(row.index);
            const jump = node('span', null, 'lab-match-jump');
            const label = node('span', null, 'lab-match-label');
            label.append(node('strong', '試合 ' + row.index),
                node('span', '▶ ' + timeText(row.start), 'ts-link'));
            jump.append(label, playerNames(row));
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
        if (!matches.length) railList.append(node('p', '解析結果がまだありません。', 'lab-help-text'));
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
        viewerFrame.hidden = false;
        viewerFrame.dataset.status = 'loading';
        viewerFrame.removeAttribute('data-ready');
        viewerFrame.dataset.status = 'loading';
        boards.append(viewerFrame);
        if (!state.bridge) state.bridge = createViewerBridge(viewerFrame);
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
        const next = node('button', '次の試合', 'btn-outline');
        next.type = 'button';
        const step = delta => {
            const index = matches.findIndex(row => row === state.currentMatch);
            const target = matches[Math.max(0, Math.min(matches.length - 1, index + delta))];
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
        pagePrev.addEventListener('click', () => { fadeBoards(); showPage(state.pageIndex - 1, { seek: follow.checked }); });
        const pageNext = node('button', '次の局面', 'btn-outline');
        pageNext.type = 'button';
        pageNext.addEventListener('click', () => { fadeBoards(); showPage(state.pageIndex + 1, { seek: follow.checked }); });
        const pageNav = node('div', null, 'lab-inline-actions');
        pageNav.append(pagePrev, pageNext);
        work.append(pageNav);
        nav.append(prev, next);
        work.append(nav);

        comparison.append(left, work);
        container.append(comparison);
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
            const first = matches.find(row => row.index === wanted) || matches[0];
            state.pendingMatch = 0;
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

    function renderNames() {
        const root = node('section', null, 'lab-video-library');
        mainView.replaceChildren(root);

        const header = node('div', null, 'lab-video-library-heading');
        const videoInfo = state.videos.find(item => item.id === state.videoId);
        header.append(node('h2', 'プレイヤー名の確認' + (videoInfo ? ' — ' + videoInfo.title : ''), 'section-title'));
        const headingActions = node('div', null, 'lab-inline-actions');
        const back = node('a', '← 動画一覧に戻る', 'btn-outline');
        back.href = '#videos';
        const register = node('button', '登録', 'btn-outline');
        register.type = 'button';
        register.addEventListener('click', () => saveNames(register));
        headingActions.append(back, register);
        header.append(headingActions);
        root.append(header);

        const changedCount = () => state.matches.filter(row => {
            const edit = state.edits.get(row.index);
            return edit && (edit.p1 !== row.p1 || edit.p2 !== row.p2);
        }).length;

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
        groups.length = 0;
        merged.forEach(entry => groups.push(entry));

        groups.sort((a, b) => b.rows.length - a.rows.length || a.rows[0].index - b.rows[0].index);

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
            input.value = firstRow ? (editsFor(firstRow)[[...entry.sides][0]] || '') : '';
            input.placeholder = '名前を入力';
            input.addEventListener('input', () => {
                entry.rows.forEach(record => {
                    const row = state.matches.find(item => item.index === record.index);
                    if (!row) return;
                    record.sides.forEach(side => applyName(row, side, input.value));
                });
                updateSummary();
            });
            card.append(input);
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
    async function saveNames(button) {
        const updates = state.matches
            .map(row => {
                const edit = state.edits.get(row.index);
                if (!edit) return null;
                if (edit.p1 === row.p1 && edit.p2 === row.p2) return null;
                return { index: row.index, p1: edit.p1, p2: edit.p2 };
            })
            .filter(Boolean);
        if (!updates.length) {
            toast('変更された名前がありません。');
            return;
        }
        button.disabled = true;
        button.textContent = '保存中…';
        try {
            const overrides = [];
            (state.nameGroups || new Map()).forEach(entry => {
                if (!entry.only || !entry.fingerprint) return;
                const box = document.querySelector('.ppt-name-groups .ppt-name-input[data-key="' + entry.key + '"]');
                const value = box ? box.value.trim() : '';
                if (value) overrides.push({ fingerprint: entry.fingerprint, name: value });
            });
            const result = await api('/api/videos/' + state.videoId + '/names', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ updates, overrides })
            });
            toast(result.changed + '試合の名前を保存しました。');
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
        videos.forEach(item => {
            const previous = before.get(item.id);
            if (previous && previous !== 'needs-names' && item.status === 'needs-names') {
                notify('解析が終わりました（' + (item.matchCount || 0) + '試合）。プレイヤー名を確認してください。');
            }
        });
        state.videos = videos;
        state.lastVideosJson = serialized;
        return changed;
    }

    async function loadTetofu() {
        const body = await api('/api/tetofu');
        state.tetofu = Array.isArray(body.tetofu) ? body.tetofu : [];
    }

    async function loadTags() {
        const body = await api('/api/tags');
        state.tags = Array.isArray(body.tags) ? body.tags : [];
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
        entryTitle.required = !isYoutube;
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
        if (!url && state.entryMode !== 'rename') {
            entryUrl.focus();
            return;
        }
        try {
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
                    body: JSON.stringify({ source: url })
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
                const videoId = parts[1] || state.videoId;
                if (videoId !== state.editsVideoId) {
                    state.edits = new Map();
                    state.editsVideoId = videoId;
                    state.matches = [];
                    state.currentMatch = null;
                    state.pages = [];
                    state.pageIndex = 0;
                }
                if (!videoId) {
                    state.view = 'videos';
                    await loadVideos();
                } else {
                    state.videoId = videoId;
                    if (state.featureList.length && !state.featureList.includes(videoId)) {
                        stopFeature();
                    }
                    await loadVideos();
                    await loadMatches(videoId);
                }
            } else if (state.view === 'names') {
                const videoId = parts[1] || state.videoId;
                if (videoId !== state.editsVideoId) {
                    state.edits = new Map();
                    state.editsVideoId = videoId;
                    state.matches = [];
                    state.currentMatch = null;
                    state.pages = [];
                    state.pageIndex = 0;
                }
                if (!videoId) {
                    state.view = 'videos';
                    await loadVideos();
                } else {
                    state.videoId = videoId;
                    await loadMatches(videoId);
                }
            } else if (state.view === 'tetofu') await loadTetofu();
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

    searchBox.addEventListener('input', () => {
        const value = searchBox.value.trim();
        state.query = value;
        if (state.view === 'tags') state.tagQuery = value;
        else if (state.view === 'tetofu') state.tetofuQuery = value;
        else state.filters.query = value;
        render();
    });
    window.addEventListener('hashchange', route);
    // 動作確認用: 連続再生と盤面同期の状態をまとめて見る
    window.__pptDebug = () => ({
        view: state.view,
        videoId: state.videoId,
        matches: state.matches.length,
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
    // メニューの外をクリックしたら閉じる
    document.addEventListener('click', event => {
        document.querySelectorAll('.lab-save-menu.open, .ppt-edit-menu.open').forEach(menu => {
            if (!menu.contains(event.target)) menu.classList.remove('open');
        });
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

    // ---- 起動 -------------------------------------------------------------
    (async function start() {
        if (!location.hash) location.hash = '#videos';
        const initialFrame = document.getElementById('detail-viewer');
        // ビューワーは同じサイトの F/ を開く（ローカル版はネットに取りに行かない）
        if (initialFrame && !initialFrame.getAttribute('src')) {
            initialFrame.src = VIEWER_BASE + '?labViewer=1';
        }
        if (initialFrame) state.bridge = createViewerBridge(initialFrame);
        state.bridgeGrace = Date.now() + 6000;
        await loadEnvironment();
        try { await loadTags(); } catch (error) { /* 起動時の失敗は無視 */ }
        await loadSettings();
        if (!location.hash) {
            const last = localStorage.getItem('pptLastView');
            const useLast = String(setting('startView') || 'videos') === 'last' && last;
            location.hash = useLast ? last : '#videos';
        }
        await route();
    })();
})();
