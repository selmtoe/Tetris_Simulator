/* The same viewer can follow a published Lab video or a private local study. */
(() => {
    if (new URLSearchParams(location.search).get('labViewer') !== '1' || window.parent === window) return;
    let parentOrigin;
    try {
        const source = new URL(document.referrer);
        if (source.origin !== 'https://selmtoe.github.io' && !(source.protocol === 'http:' && ['127.0.0.1', 'localhost'].includes(source.hostname))) return;
        parentOrigin = source.origin;
    } catch { return; }
    document.documentElement.dataset.labVideoViewer = 'true';
    let ready = false, key = null, applying = false, resizeFrame = 0;
    const notify = (type, data = {}) => window.parent.postMessage({type, key, ...data}, parentOrigin);
    const fit = () => {
        resizeFrame = 0;
        const canvas = document.getElementById('viewerCanvas'), controls = document.getElementById('viewer-controls');
        if (!canvas?.width || !canvas.offsetWidth) return;
        const top = controls.querySelector('.viewer-controls-shell').getBoundingClientRect().bottom + 8;
        const height = Math.max(1, innerHeight - top - 6), width = Math.min(Math.max(1, innerWidth - 12), height * canvas.width / canvas.height);
        const root = document.documentElement;root.dataset.labViewerFit = 'true';
        root.style.setProperty('--lab-viewer-width', width + 'px');
        root.style.setProperty('--lab-viewer-center', (top + (width * canvas.height / canvas.width) / 2) + 'px');
    };
    const schedule = () => { if (!resizeFrame) resizeFrame = requestAnimationFrame(fit); };
    window.addEventListener('tetris:viewer-ready', () => {
        const original = loadPage;
        loadPage = function(index) {
            const before = currentPageIndex, result = original(index);
            if (!applying && key && before !== currentPageIndex) notify('labViewerPage', {index: currentPageIndex});
            return result;
        };
        const style = document.createElement('style');
        style.textContent = `
            html[data-lab-video-viewer] #back-to-editor-btn,html[data-lab-video-viewer] .lab-settings-open{display:none!important}
            html[data-lab-video-viewer] #viewer-container{user-select:none;-webkit-user-select:none;-webkit-touch-callout:none}
            html[data-lab-video-viewer] #viewer-container #viewer-controls{top:2px;--viewer-menu-height:108px}
            html[data-lab-video-viewer] #viewer-controls .viewer-controls-shell{width:calc(100vw - 12px);height:108px;border-radius:12px;box-shadow:none;transition:none}
            html[data-lab-video-viewer] #viewer-controls #viewer-page-indicator{height:30px;line-height:30px}
            html[data-lab-video-viewer] #viewer-controls-panel{top:32px;width:calc(100vw - 28px)}
            html[data-lab-video-viewer] #viewer-controls .viewer-top-row .button{padding:5px 8px}
            html[data-lab-video-viewer] #viewer-controls .viewer-slider-container{margin-top:4px}
            @media(max-width:320px){html[data-lab-video-viewer] #viewer-controls .viewer-top-row{gap:3px}html[data-lab-video-viewer] #viewer-controls .viewer-top-row .button{padding:5px 4px;font-size:11px}}
        `;
        document.head.append(style);
        document.getElementById('viewerCanvas').setAttribute('aria-label', 'P1・P2の盤面。各盤面の左がHOLD、右がNEXTです。');
        document.getElementById('viewer-simulator-btn').textContent = 'シミュレータ';
        if (!document.getElementById('viewer-analysis-btn')) document.getElementById('viewer-ai-score-btn').textContent = '分析';
        window.TetrisWorkspace.practice = () => notify('labViewerPractice');
        for (const id of ['viewer-share-btn', 'viewer-export-btn', 'viewer-ai-score-btn', 'viewer-analysis-btn']) document.getElementById(id)?.addEventListener('click', () => notify('labViewerPause'), true);
        new ResizeObserver(schedule).observe(document.getElementById('viewer-container'));
        new ResizeObserver(schedule).observe(document.getElementById('viewer-controls'));
        new MutationObserver(schedule).observe(document.getElementById('viewerCanvas'), {attributes:true, attributeFilter:['width','height']});
        window.addEventListener('resize', schedule);
        ready = true;notify('labViewerReady');schedule();
    });
    window.addEventListener('message', event => {
        if (event.origin !== parentOrigin || event.source !== window.parent || event.data?.type !== 'labViewerRequest' || !ready) return;
        const message = event.data;
        try {
            if (message.action === 'ready') { notify('labViewerReady');return; }
            if (message.action === 'theme') { window.LabAppearance?.setTheme(message.theme);return; }
            if (message.action === 'load') {
                if (typeof message.key !== 'string' || !Array.isArray(message.pages) || !message.pages.length || message.pages.length > 50000) return;
                applying = true;key = message.key;
                applyCollectionData({v:3,m:'2P',cases:[{name:String(message.title||'動画解析'),kind:'snapshot',gameMode:'2P',pages:message.pages}]});
                loadPage(Math.max(0, Math.min(fumenPages.length - 1, Number(message.index) || 0)));
                document.getElementById('view-mode-btn').click();
                window.LabAppearance?.setTheme(message.theme);notify('labViewerLoaded', {index:currentPageIndex});schedule();
            } else if (message.action === 'seek' && message.key === key && Number.isInteger(message.index)) {
                applying = true;loadPage(message.index);
            }
        } catch (error) { notify('labViewerError', {message:error.message}); }
        finally { applying = false; }
    });
})();
