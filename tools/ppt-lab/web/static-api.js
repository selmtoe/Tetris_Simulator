/* ブラウザ版（閲覧専用・GitHub Pages など）で読み込む。手元の PPT Lab では何もしない。

   - 解析データは静的な JSON（既定は data/。window.PPT_DATA_BASE で別の場所にできる）
   - 見る人が保存するテト譜・タグは、その人のブラウザ（IndexedDB）に入る
   - 設定は表示の項目だけ。その人のブラウザ（localStorage）に入る
   画面（app.js）は手元と同じ /api/... を呼ぶので、ここで静的データとブラウザ保存に振り分ける。 */
(function () {
    // ブラウザ版の index.html が window.PPT_STATIC = true を先に立てる
    if (window.PPT_STATIC !== true) return;

    document.documentElement.dataset.readonly = 'true';
    document.documentElement.dataset.localSaves = 'true';

    const BASE = String(window.PPT_DATA_BASE || 'data/').replace(/\/?$/, '/');
    const original = window.fetch.bind(window);
    const json = (body, status) => new Response(JSON.stringify(body), {
        status: status || 200,
        headers: { 'Content-Type': 'application/json; charset=utf-8' }
    });
    const cache = new Map();
    async function data(path) {
        if (!cache.has(path)) {
            // 同期のたびにデータが変わるので、毎回サーバーに新しいか確かめてから使う
            cache.set(path, original(BASE + path, { cache: 'no-cache' }).then(response => {
                if (!response.ok) {
                    // ページ（古いまま覚えられている）とデータ（新しい）の作りが合わないとき
                    throw new Error('サイトが更新されました。ページを再読み込みしてください。');
                }
                return response.json();
            }).catch(error => { cache.delete(path); throw error; }));
        }
        return cache.get(path);
    }

    // ---- 見る人の保存（IndexedDB に1件のまとまりとして置く）----------------
    const EMPTY = () => ({ version: 1, tetofu: [], tags: {}, links: {} });
    let local = null;
    function openDb() {
        return new Promise((resolve, reject) => {
            const request = indexedDB.open('ppt-lab-viewer', 1);
            request.onupgradeneeded = () => request.result.createObjectStore('store');
            request.onsuccess = () => resolve(request.result);
            request.onerror = () => reject(request.error);
        });
    }
    async function loadLocal() {
        if (local) return local;
        try {
            const db = await openDb();
            local = await new Promise(resolve => {
                const request = db.transaction('store').objectStore('store').get('user');
                request.onsuccess = () => resolve(request.result || EMPTY());
                request.onerror = () => resolve(EMPTY());
            });
        } catch (error) {
            local = EMPTY();          // 保存できないブラウザ（プライベートモードなど）でも表示はする
        }
        local.tetofu = local.tetofu || [];
        local.tags = local.tags || {};
        local.links = local.links || {};
        return local;
    }
    let persistAsked = false;
    async function saveLocal() {
        try {
            const db = await openDb();
            await new Promise((resolve, reject) => {
                const tx = db.transaction('store', 'readwrite');
                tx.objectStore('store').put(local, 'user');
                tx.oncomplete = resolve;
                tx.onerror = () => reject(tx.error);
            });
        } catch (error) {
            throw new Error('このブラウザには保存できません（プライベートモードなど）');
        }
        // ブラウザに「勝手に消さないで」と頼む（初めて保存したときだけ）
        if (!persistAsked && navigator.storage && navigator.storage.persist) {
            persistAsked = true;
            try { await navigator.storage.persist(); } catch (error) { /* 断られても保存はできている */ }
        }
    }
    const newId = () => 'local-' + Date.now().toString(36) + Math.random().toString(36).slice(2, 8);

    // テト譜は YouTube の動画ID と時刻で動画・試合に結び直す（解析し直して試合番号が変わってもずれない）
    async function resolveTetofu(item) {
        const out = Object.assign({}, item, { tags: (local.links[item.id] || []).slice() });
        if (!item.youtubeId) return out;
        const videos = (await data('videos.json')).videos || [];
        const video = videos.find(entry => entry.youtubeId === item.youtubeId);
        if (!video) {
            delete out.videoId;
            return out;
        }
        out.videoId = video.id;
        const seconds = Number(item.seconds);
        if (Number.isFinite(seconds)) {
            try {
                const rows = (await data('matches/' + video.id + '.json')).matches || [];
                const row = rows.find(entry => seconds >= Number(entry.start) - 1 && seconds <= Number(entry.end) + 1);
                if (row) out.matchIndex = row.index;
            } catch (error) { /* 試合が読めなくても動画には飛べる */ }
        }
        return out;
    }

    async function tagList() {
        const base = (await data('tags.json')).tags || [];
        const counts = new Map(base.map(tag => [tag.name, Object.assign({}, tag)]));
        Object.entries(local.tags).forEach(([name, info]) => {
            if (!counts.has(name)) counts.set(name, { name, count: 0, kind: info.kind || 'free' });
            else counts.get(name).kind = info.kind || counts.get(name).kind;
        });
        Object.values(local.links).forEach(tags => tags.forEach(name => {
            if (!counts.has(name)) counts.set(name, { name, count: 0, kind: 'free' });
            counts.get(name).count += 1;
        }));
        return [...counts.values()].sort((a, b) => a.name.localeCompare(b.name, 'ja'));
    }

    // ---- 設定（表示の項目だけ・localStorage）----------------------------------
    function savedSettings() {
        try { return JSON.parse(localStorage.getItem('ppt-lab-viewer-settings') || '{}'); } catch (error) { return {}; }
    }

    async function handle(method, path, query, body) {
        let match = null;
        await loadLocal();
        // 読むだけ（解析データ）
        if (method === 'GET') {
            if (path === '/api/videos') {
                const payload = await data('videos.json');
                const videos = (payload.videos || []).map(video => {
                    const extra = local.links[video.id] || [];
                    return extra.length ? Object.assign({}, video, { tags: [...new Set([...(video.tags || []), ...extra])] }) : video;
                });
                return json({ videos });
            }
            match = path.match(/^\/api\/videos\/([^/]+)\/matches$/);
            if (match) return json(await data('matches/' + match[1] + '.json'));
            match = path.match(/^\/api\/videos\/([^/]+)\/replay\/(\d+)$/);
            if (match) {
                const hashes = await data('replays/' + match[1] + '.json');
                const hash = hashes[match[2]] || '';
                if (!hash) return json({ error: 'リプレイがありません' }, 404);
                const viewer = new URL((window.PPT_SITE_BASE || '../') + 'F/index.html?page=1#' + hash, location.href).href;
                return json({ urls: { combined: viewer, p1: viewer, p2: viewer }, hash, imported: true });
            }
            if (path === '/api/players/stats') {
                const index = await data('players/index.json');
                const stem = index[query.get('name') || ''];
                if (!stem) return json({ error: 'このプレイヤーの統計はありません' }, 404);
                const period = ['all', 'half', '30d', '7d'].includes(query.get('period')) ? query.get('period') : 'all';
                return json(await data('players/' + stem + '-' + period + '.json'));
            }
            if (path === '/api/tags') return json({ tags: await tagList() });
            if (path === '/api/tetofu') {
                const items = await Promise.all(local.tetofu.map(resolveTetofu));
                return json({ tetofu: items.sort((a, b) => String(b.createdAt).localeCompare(String(a.createdAt))) });
            }
            if (path === '/api/settings') {
                const base = (await data('settings.json')).settings || {};
                return json({ settings: Object.assign({}, base, savedSettings()) });
            }
            if (path === '/api/settings/schema') return json(await data('settings-schema.json'));
            if (path === '/api/env') return json(await data('env.json'));
        }
        // 見る人の保存
        if (method === 'POST' && path === '/api/tetofu') {
            const kind = ['simulator', 'editor', 'official'].includes(body.kind) ? body.kind : 'simulator';
            const title = String(body.title || '').trim().slice(0, 200);
            const url = String(body.url || '').trim();
            if (!title || !url) return json({ error: '名前とリンクを入れてください' }, 400);
            const item = { id: newId(), kind, title, url, createdAt: new Date().toISOString().slice(0, 19) };
            if (body.videoId) {
                const videos = (await data('videos.json')).videos || [];
                const video = videos.find(entry => entry.id === body.videoId);
                if (video) {
                    item.youtubeId = video.youtubeId;
                    item.scope = String(body.scope || '');
                    if (Number.isFinite(Number(body.seconds))) item.seconds = Number(body.seconds);
                }
            }
            local.tetofu.push(item);
            await saveLocal();
            return json(await resolveTetofu(item));
        }
        match = path.match(/^\/api\/tetofu\/([^/]+)\/title$/);
        if (method === 'POST' && match) {
            const item = local.tetofu.find(entry => entry.id === match[1]);
            if (!item) return json({ error: 'テト譜が見つかりません' }, 404);
            const title = String(body.title || '').trim();
            if (!title) return json({ error: 'タイトルを入力してください' }, 400);
            item.title = title.slice(0, 200);
            await saveLocal();
            return json({ ok: true });
        }
        match = path.match(/^\/api\/tetofu\/([^/]+)$/);
        if (method === 'DELETE' && match) {
            local.tetofu = local.tetofu.filter(entry => entry.id !== match[1]);
            delete local.links[match[1]];
            await saveLocal();
            return json({ ok: true, id: match[1] });
        }
        if (method === 'POST' && path === '/api/tags/apply') {
            const target = String(body.target || '');
            const players = new Set(((await data('videos.json')).videos || []).flatMap(video => video.players || []));
            const base = new Set(((await data('videos.json')).videos || []).find(video => video.id === target)?.tags || []);
            // プレイヤー名や公開データにもともと付いているタグは、見る人の保存に入れない
            const tags = [...new Set((body.tags || []).map(String).map(tag => tag.trim()).filter(Boolean))]
                .filter(tag => !players.has(tag) && !base.has(tag));
            if (tags.length) local.links[target] = tags;
            else delete local.links[target];
            tags.forEach(tag => { if (!local.tags[tag]) local.tags[tag] = { kind: 'free' }; });
            await saveLocal();
            return json({ tags: [...base, ...tags] });
        }
        if (method === 'POST' && path === '/api/tags') {
            const name = String(body.name || '').trim();
            if (!name) return json({ error: 'タグ名を入力してください' }, 400);
            local.tags[name] = { kind: body.kind || (local.tags[name] || {}).kind || 'free' };
            await saveLocal();
            return json({ name, count: 0, kind: local.tags[name].kind });
        }
        if (method === 'POST' && path === '/api/tags/kind') {
            const name = String(body.name || '');
            local.tags[name] = { kind: String(body.kind || 'free') };
            await saveLocal();
            return json({ ok: true });
        }
        if (method === 'DELETE' && path === '/api/tags') {
            const name = String(body.name || '');
            delete local.tags[name];
            Object.keys(local.links).forEach(target => {
                local.links[target] = local.links[target].filter(tag => tag !== name);
                if (!local.links[target].length) delete local.links[target];
            });
            await saveLocal();
            return json({ ok: true });
        }
        if (method === 'POST' && path === '/api/settings') {
            const schema = (await data('settings-schema.json')).schema || [];
            const allowed = new Set(schema.map(item => item.key));
            const next = savedSettings();
            Object.entries(body || {}).forEach(([key, value]) => { if (allowed.has(key)) next[key] = value; });
            try { localStorage.setItem('ppt-lab-viewer-settings', JSON.stringify(next)); } catch (error) { /* 保存できなくても今回は効く */ }
            const base = (await data('settings.json')).settings || {};
            return json({ ok: true, settings: Object.assign({}, base, next) });
        }
        return json({ error: 'ブラウザ版ではこの操作はできません' }, 403);
    }

    window.fetch = async (input, init) => {
        const raw = typeof input === 'string' ? input : input.url;
        const url = new URL(raw, location.href);
        if (url.origin !== location.origin || !url.pathname.startsWith('/api/')) return original(input, init);
        const method = String((init && init.method) || 'GET').toUpperCase();
        let body = {};
        if (init && init.body && typeof init.body === 'string') {
            try { body = JSON.parse(init.body); } catch (error) { body = {}; }
        }
        try {
            return await handle(method, url.pathname, url.searchParams, body);
        } catch (error) {
            return json({ error: error.message || '読み込めませんでした' }, 500);
        }
    };

    // ---- バックアップ（設定の「自分のデータ」から使う）--------------------------
    window.PPTLocal = {
        async exportData() {
            await loadLocal();
            return { format: 'ppt-lab-viewer/v1', exportedAt: new Date().toISOString(),
                     tetofu: local.tetofu, tags: local.tags, links: local.links,
                     settings: savedSettings() };
        },
        async importData(payload, replace) {
            if (!payload || payload.format !== 'ppt-lab-viewer/v1') throw new Error('PPT Lab の保存データではありません');
            await loadLocal();
            if (replace) local = EMPTY();
            const known = new Set(local.tetofu.map(item => item.id));
            (payload.tetofu || []).forEach(item => { if (item && item.id && !known.has(item.id)) local.tetofu.push(item); });
            Object.assign(local.tags, payload.tags || {});
            Object.entries(payload.links || {}).forEach(([target, tags]) => {
                local.links[target] = [...new Set([...(local.links[target] || []), ...tags])];
            });
            await saveLocal();
            if (payload.settings) {
                try { localStorage.setItem('ppt-lab-viewer-settings', JSON.stringify(payload.settings)); } catch (error) { /* 無視 */ }
            }
            return { tetofu: local.tetofu.length };
        },
        async summary() {
            await loadLocal();
            let estimate = null;
            try { estimate = navigator.storage && navigator.storage.estimate ? await navigator.storage.estimate() : null; } catch (error) { estimate = null; }
            let persisted = null;
            try { persisted = navigator.storage && navigator.storage.persisted ? await navigator.storage.persisted() : null; } catch (error) { persisted = null; }
            return { tetofu: local.tetofu.length, tags: Object.keys(local.tags).length,
                     usage: estimate ? estimate.usage : null, persisted };
        },
        async clear() {
            local = EMPTY();
            await saveLocal();
        }
    };
})();
