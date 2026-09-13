/* URL-only preview: removing motion=1 restores the ordinary UI immediately
   on navigation. No preference or recovery data is written here. */
(() => {
    if (new URLSearchParams(location.search).get('motion') !== '1') return;
    const root = document.documentElement;
    const reduced = matchMedia('(prefers-reduced-motion: reduce)');
    const running = new Map();
    function preference() {
        root.dataset.uiMotion = reduced.matches ? 'off' : 'on';
        if (reduced.matches) for (const animation of running.values()) animation.cancel();
    }
    preference();
    reduced.addEventListener('change', preference);
    function animate(element, keyframes, duration) {
        if (reduced.matches || !element?.isConnected || typeof element.animate !== 'function') return;
        running.get(element)?.cancel();
        const animation = element.animate(keyframes, { duration, easing: 'cubic-bezier(.16, 1, .3, 1)' });
        running.set(element, animation);
        animation.finished.catch(() => {}).then(() => {
            if (running.get(element) === animation) running.delete(element);
        });
    }
    function start() {
        const overlays = 'dialog, #settings-modal, #share-modal, #rule-modal, #edit-menu-modal, #export-modal, #code-gen-modal, #analysis-modal, .ai-score-modal, .ai-model-modal';
        const watched = new WeakSet();
        const visible = element => element.getClientRects().length > 0 && getComputedStyle(element).visibility !== 'hidden';
        function watch(element) {
            if (watched.has(element)) return;
            watched.add(element);
            let wasVisible = visible(element);
            new MutationObserver(() => {
                const isVisible = visible(element);
                const card = element.querySelector('.modal-content, .ai-score-content, .ai-model-dialog, #code-gen-content, #analysis-content') || element;
                if (isVisible && !wasVisible) animate(card, [
                    { opacity: 0, translate: '0 6px' },
                    { opacity: 1, translate: '0 0' }
                ], 180);
                if (!isVisible) running.get(card)?.cancel();
                wasVisible = isVisible;
            }).observe(element, { attributes: true, attributeFilter: ['style', 'class', 'open', 'hidden', 'aria-hidden'] });
        }
        function scan(element) {
            if (!(element instanceof Element)) return;
            if (element.matches(overlays)) watch(element);
            element.querySelectorAll(overlays).forEach(watch);
        }
        scan(document.body);
        // Only newly attached top-level dialogs; no observer on board updates.
        new MutationObserver(records => records.forEach(record => record.addedNodes.forEach(scan)))
            .observe(document.body, { childList: true });
        if (document.getElementById('workspace-panes')) {
            let previous = document.body.dataset.mode + ':' + document.body.dataset.narrowPane;
            new MutationObserver(() => {
                const next = document.body.dataset.mode + ':' + document.body.dataset.narrowPane;
                if (next === previous) return;
                previous = next;
                const panes = document.querySelectorAll('.workspace-pane');
                for (const pane of panes) running.get(pane)?.cancel();
                // A started game responds immediately with no visual delay.
                if (document.body.dataset.mode === 'playing') return;
                requestAnimationFrame(() => {
                    if (document.body.dataset.mode === 'playing') return;
                    for (const pane of panes) if (visible(pane)) animate(pane, [{ opacity: .7 }, { opacity: 1 }], 160);
                });
            }).observe(document.body, { attributes: true, attributeFilter: ['data-mode', 'data-narrow-pane'] });
        }
    }
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start, { once: true });
    else start();
})();
