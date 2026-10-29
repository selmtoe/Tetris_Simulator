/* 閲覧専用版（GitHub Pages）で読み込む。
   ローカル（127.0.0.1 / localhost）では何もしない。 */
(function () {
    // 閲覧専用版の index.html が window.PPT_STATIC = true を先に立てる
    if (window.PPT_STATIC !== true) return;

    document.documentElement.dataset.readonly = 'true';

    const routes = url => {
        const path = url.split('?')[0];
        if (path === '/api/videos') return 'videos.json';
        if (path === '/api/tags') return 'tags.json';
        if (path === '/api/tetofu') return 'tetofu.json';
        if (path === '/api/names/mappings') return 'name-mappings.json';
        if (path === '/api/settings') return 'settings.json';
        if (path === '/api/env') return 'env.json';
        const match = path.match(/^\/api\/videos\/([^/]+)\/matches$/);
        if (match) return 'matches/' + match[1] + '.json';
        return null;
    };

    const replayTarget = url => {
        const match = url.split('?')[0].match(/^\/api\/videos\/([^/]+)\/replay\/(\d+)$/);
        return match ? { id: match[1], index: match[2] } : null;
    };
    const viewerUrl = hash => new URL('../F/index.html?page=1#' + hash, location.href).href;

    const original = window.fetch.bind(window);
    window.fetch = async (input, init) => {
        const url = typeof input === 'string' ? input : input.url;
        if (!url.startsWith('/api/')) return original(input, init);
        const replay = replayTarget(url);
        if (replay) {
            const response = await original('data/replays/' + replay.id + '.json', init);
            if (!response.ok) return response;
            const hashes = await response.json();
            const hash = hashes[replay.index] || '';
            const body = hash
                ? { urls: { combined: viewerUrl(hash), p1: viewerUrl(hash), p2: viewerUrl(hash) }, hash, imported: true }
                : { error: 'リプレイがありません' };
            return new Response(JSON.stringify(body), {
                status: hash ? 200 : 404,
                headers: { 'Content-Type': 'application/json; charset=utf-8' }
            });
        }
        const target = routes(url);
        if (!target) {
            return new Response(JSON.stringify({ error: '閲覧専用のため変更できません' }), {
                status: 403,
                headers: { 'Content-Type': 'application/json; charset=utf-8' }
            });
        }
        return original('data/' + target, init);
    };
})();
