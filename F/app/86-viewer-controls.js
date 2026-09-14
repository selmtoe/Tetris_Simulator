/* A compact replay menu. Its expansion overlays the board without resizing it. */
document.addEventListener('DOMContentLoaded', () => {
    const controls = document.getElementById('viewer-controls');
    const trigger = document.getElementById('viewer-page-indicator');
    const panel = document.getElementById('viewer-controls-panel');
    const canvas = document.getElementById('viewerCanvas');
    const viewer = document.getElementById('viewer-container');
    // Keep long presses on the replay surface from opening native selection
    // controls. Share/export dialogs are outside this surface and retain copy.
    viewer.addEventListener('contextmenu', event => event.preventDefault());
    viewer.addEventListener('selectstart', event => event.preventDefault());
    let pinned = false;
    let persistent = false;
    let layoutFrame = 0;
    let leaveTimer;

    function open() {
        clearTimeout(leaveTimer);
        controls.classList.add('is-expanded');
        panel.inert = false;
        panel.setAttribute('aria-hidden', 'false');
        trigger.setAttribute('aria-expanded', 'true');
        trigger.title = persistent ? 'リプレイのページ' : 'リプレイの操作を閉じる';
    }
    function close(force = false) {
        if (persistent && !force) return;
        clearTimeout(leaveTimer);
        pinned = false;
        if (panel.contains(document.activeElement)) trigger.focus({ preventScroll: true });
        controls.classList.remove('is-expanded');
        panel.inert = true;
        panel.setAttribute('aria-hidden', 'true');
        trigger.setAttribute('aria-expanded', 'false');
        trigger.title = 'リプレイの操作を開く';
    }
    controls.addEventListener('pointerenter', event => {
        if (event.pointerType === 'mouse' || event.pointerType === 'pen') open();
    });
    controls.addEventListener('pointerleave', () => {
        leaveTimer = setTimeout(() => {
            if (!pinned && !panel.contains(document.activeElement) && !document.querySelector('#viewer-analysis-menu.is-open')) close();
        }, 150);
    });
    trigger.addEventListener('click', () => {
        if (persistent) return;
        if (pinned) close();
        else { pinned = true; open(); }
    });
    controls.addEventListener('focusout', () => {
        queueMicrotask(() => {
            if (!controls.contains(document.activeElement) && !controls.matches(':hover') && !document.querySelector('#viewer-analysis-menu.is-open')) close();
        });
    });
    controls.addEventListener('keydown', event => {
        // Replay shortcuts must not seek while operating buttons or the slider.
        event.stopPropagation();
        if (event.key === 'Escape') { event.preventDefault(); close(); }
        if (event.target === trigger && event.key === 'ArrowDown') {
            event.preventDefault();
            pinned = true;
            open();
            panel.querySelector('button:not([hidden])').focus();
        }
    });
    panel.addEventListener('click', event => {
        if (event.target.closest('button') && !event.target.closest('[data-search-menu]')) close();
    });
    document.addEventListener('pointerdown', event => {
        if (!controls.contains(event.target) && !event.target.closest('#viewer-analysis-menu')) close();
    });
    function updatePresentation() {
        layoutFrame = 0;
        if (!canvas.offsetWidth || !viewer.offsetWidth || !controls.offsetWidth) return;
        const board = canvas.getBoundingClientRect();
        const anchor = controls.getBoundingClientRect();
        const style = getComputedStyle(controls);
        const width = panel.getBoundingClientRect().width + 24;
        const height = parseFloat(style.getPropertyValue('--viewer-menu-height'));
        const left = anchor.left + anchor.width / 2 - width / 2;
        const bottom = anchor.top + height;
        // Protect the complete canvas (including HOLD/NEXT), with a little
        // breathing room. Do not shrink or shift the board to make the menu fit.
        const overlaps = left < board.right + 8 && left + width > board.left - 8 &&
            anchor.top < board.bottom + 8 && bottom > board.top - 8;
        const next = document.documentElement.dataset.labVideoViewer === 'true' || !overlaps && bottom + 8 <= innerHeight;
        const changed = controls.dataset.presentation !== (next ? 'persistent' : 'compact');
        persistent = next;
        controls.dataset.presentation = next ? 'persistent' : 'compact';
        trigger.disabled = next;
        if (next) open();
        else if (changed) close(true);
    }
    function scheduleLayout() {
        if (!layoutFrame) layoutFrame = requestAnimationFrame(updatePresentation);
    }
    const resize = new ResizeObserver(scheduleLayout);
    resize.observe(canvas);
    resize.observe(controls);
    new MutationObserver(scheduleLayout).observe(document.documentElement, {attributes:true, attributeFilter:['style']});
    new MutationObserver(scheduleLayout).observe(canvas, {attributes:true, attributeFilter:['width','height']});
    new MutationObserver(() => {
        if (viewer.style.display === 'none') close(true);
        scheduleLayout();
    }).observe(viewer, { attributes: true, attributeFilter: ['style'] });
    window.addEventListener('resize', scheduleLayout);
    document.fonts?.ready.then(scheduleLayout);
    scheduleLayout();
});
