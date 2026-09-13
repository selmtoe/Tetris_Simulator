/* A saved link contains the same URL as Copy, not another replay format. */
(() => {
    const app = document.currentScript?.dataset.app;
    const escape = value => String(value).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    function extract(input) {
        const text = String(input || '').trim().replace(/^\uFEFF/, '');
        if (/^<!doctype html|^<html/i.test(text)) {
            const doc = new DOMParser().parseFromString(text, 'text/html');
            const link = doc.querySelector('meta[name="tetris-link"]')?.content;
            if (!link || !/^https?:\/\//i.test(link)) throw new Error('保存したリンクを読み込めませんでした。');
            return link;
        }
        return text.match(/^\[InternetShortcut\]\s*URL=(\S+)/i)?.[1] || text;
    }
    function html(url, title) {
        const parsed = new URL(url);
        if (!['http:', 'https:'].includes(parsed.protocol)) throw new Error('このリンクは保存できません。');
        return `<!doctype html><html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><meta name="tetris-link" content="${escape(url)}"><title>${escape(title)}</title><style>body{margin:0;min-height:100vh;display:grid;place-items:center;background:#e9e9e9;color:#343a3c;font:16px system-ui,sans-serif}main{margin:24px;padding:32px;max-width:440px;background:#f4f4f3;border:1px solid #d6dcda;border-radius:18px;text-align:center}h1{font-size:20px;font-weight:500;overflow-wrap:anywhere}a{display:inline-block;margin:16px 0 8px;padding:12px 24px;border-radius:24px;background:#dce8e2;color:inherit;text-decoration:none}p{font-size:13px;color:#67716c}</style></head><body><main><h1>${escape(title)}</h1><a href="${escape(url)}">${app === 'simulator' ? 'シミュレータで開く' : '再生する'}</a><p>保存したリンクをブラウザで開きます。</p></main></body></html>`;
    }
    function save(url, title) {
        const stamp = new Date().toLocaleString('sv-SE').replace(/[: ]/g, '-');
        const blob = URL.createObjectURL(new Blob([html(url, title)], {type:'text/html;charset=utf-8'}));
        const anchor = document.createElement('a');
        anchor.href = blob;
        anchor.download = `${title}-${stamp}.html`.replace(/[\\/:*?"<>|]/g, '_');
        document.body.append(anchor); anchor.click(); anchor.remove();
        setTimeout(() => URL.revokeObjectURL(blob), 30000);
    }
    window.TetrisLinkFile = {extract, html, save};
    document.addEventListener('DOMContentLoaded', () => {
        document.getElementById('save-link-btn')?.addEventListener('click', async event => {
            const button = event.currentTarget;
            button.disabled = true;
            try {
                await generateAndDisplayLink();
                const title = app === 'viewer' && typeof currentCase === 'function' ? currentCase()?.name || 'リプレイ' : 'シミュレータ';
                save(document.getElementById('share-link-input').value, title);
            } finally { button.disabled = false; }
        });
    });
    if (app === 'workspace') return;
    async function open(input, name) {
        const text = extract(input);
        if (window.parent !== window && new URLSearchParams(location.search).get('workspace') === '1') {
            window.parent.postMessage({type:'workspaceImport', input:text, name}, location.origin);
        } else if (app === 'viewer') {
            await window.TetrisWorkspace.import(text);
        } else {
            let data = text;
            if (/^https?:/i.test(data)) data = new URL(data).hash.slice(1);
            if (!data.startsWith('{')) data = new TextDecoder().decode(Uint8Array.from(atob(data), c => c.charCodeAt(0)));
            if (!applyGameState(JSON.parse(data))) throw new Error('リンクを読み込めませんでした。');
        }
    }
    window.addEventListener('dragover', event => { if (event.dataTransfer.types.includes('Files')) event.preventDefault(); });
    window.addEventListener('drop', event => {
        const file = event.dataTransfer.files[0];
        if (!file || !/\.(?:html?|url|json|txt|tetrisevent)$/i.test(file.name)) return;
        event.preventDefault(); event.stopImmediatePropagation();
        file.text().then(text => open(text, file.name)).catch(error => alert(error.message));
    }, true);
    document.addEventListener('paste', event => {
        if (event.target.closest('input,textarea,[contenteditable="true"]')) return;
        const text = event.clipboardData?.getData('text/plain')?.trim();
        if (!text || !/^(?:https?:\/\/|\{)/i.test(text)) return;
        event.preventDefault(); open(text).catch(error => alert(error.message));
    });
})();
