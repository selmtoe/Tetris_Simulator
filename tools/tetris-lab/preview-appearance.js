/* Local presentation only: theme, viewport fitting, and the incoming meter. */
(() => {
    'use strict';
    const palettes = {
        light: { muted: '#717171', track: '#d3d5d5', tick: '#b3b8b6', pending: '#c97e77', queued: '#c4ab71' },
        dark: { muted: '#aaaab1', track: '#414347', tick: '#61676b', pending: '#d9958c', queued: '#ccb77f' }
    };

    function editorFit(viewWidth, viewHeight, layoutWidth, layoutHeight) {
        // One scale for both axes: the approved UI keeps its proportions.
        return Math.min(viewWidth / layoutWidth, viewHeight / layoutHeight);
    }

    function drawPlacement(ctx, x, y, size) {
        // An inset light edge with a dark backing remains visible on every
        // piece color. Keep both strokes inside the cell and retain its fill.
        const inset = size * 0.13;
        ctx.save();
        ctx.globalAlpha = 1;
        ctx.setLineDash([]);
        ctx.lineJoin = 'round';
        ctx.strokeStyle = '#253039';
        ctx.lineWidth = size * 0.13;
        ctx.strokeRect(x + inset, y + inset, size - inset * 2, size - inset * 2);
        ctx.strokeStyle = '#fffdf7';
        ctx.lineWidth = size * 0.065;
        ctx.strokeRect(x + inset, y + inset, size - inset * 2, size - inset * 2);
        ctx.restore();
    }

    function drawMeter(ctx, x, y, unit, rows, pending, queued, colors) {
        const width = 8, height = rows * unit;
        const ready = Math.min(rows, Math.max(0, pending));
        const waiting = Math.min(rows - ready, Math.max(0, queued));
        ctx.save();
        ctx.beginPath();
        ctx.roundRect(x, y, width, height, width / 2);
        ctx.fillStyle = colors.track;
        ctx.fill();
        ctx.clip();
        // One segment per row. Never allow a large queue to extend above the well.
        for (let row = 0; row < ready + waiting; row++) {
            ctx.fillStyle = row < ready ? colors.pending : colors.queued;
            ctx.fillRect(x, y + height - (row + 1) * unit + 1, width, unit - 2);
        }
        ctx.fillStyle = colors.tick;
        for (let row = 5; row < rows; row += 5) {
            if (row >= ready + waiting) ctx.fillRect(x + 2, y + height - row * unit, width - 4, 1);
        }
        ctx.restore();
    }

    if (typeof module !== 'undefined' && module.exports) {
        module.exports = { editorFit, drawMeter, drawPlacement, palettes };
        return;
    }
    const root = document.documentElement;
    const key = 'lab-appearance-mode';
    let mode = 'light';
    try { if (localStorage.getItem(key) === 'dark') mode = 'dark'; } catch (_) { /* Private storage may be unavailable. */ }
    const appearance = window.LabAppearance = { colors: palettes[mode], drawMeter, drawPlacement };
    root.dataset.labTheme = mode;

    function applyTheme(next, save = false) {
        mode = next === 'dark' ? 'dark' : 'light';
        root.dataset.labTheme = mode;
        appearance.colors = palettes[mode];
        for (const select of document.querySelectorAll('.lab-appearance-select')) select.value = mode;
        if (save) { try { localStorage.setItem(key, mode); } catch (_) { /* The current tab still switches. */ } }
        // F's viewer is event-driven; the simulator already draws every frame.
        if (document.querySelector('#viewer-container')?.offsetWidth && typeof window.drawViewer === 'function') window.drawViewer();
    }

    function appearanceSetting() {
        const row = document.createElement('div');
        row.className = 'setting-item lab-appearance-setting';
        const label = document.createElement('label');
        label.htmlFor = 'lab-appearance-mode';
        label.textContent = '表示モード';
        const select = document.createElement('select');
        select.id = 'lab-appearance-mode';
        select.className = 'lab-appearance-select';
        select.innerHTML = '<option value="light">ライト</option><option value="dark">ダーク</option>';
        select.value = mode;
        select.addEventListener('change', () => applyTheme(select.value, true));
        row.append(label, select);
        return row;
    }

    function start() {
        const general = document.querySelector('#tab-content-general');
        if (general) {
            // The native settings list is rebuilt on every open; keep this
            // preference in its general tab, outside that transient list.
            general.prepend(appearanceSetting());
        } else {
            const dialog = document.createElement('dialog');
            dialog.className = 'lab-settings-dialog';
            dialog.setAttribute('aria-labelledby', 'lab-settings-heading');
            dialog.innerHTML = '<h2 id="lab-settings-heading">設定</h2><h3>一般</h3>';
            dialog.append(appearanceSetting());
            const footer = document.createElement('div');
            footer.className = 'lab-settings-footer';
            const close = document.createElement('button');
            close.type = 'button';
            close.className = 'button';
            close.textContent = '閉じる';
            close.addEventListener('click', () => dialog.close());
            footer.append(close);
            dialog.append(footer);
            document.body.append(dialog);
            let opener;
            dialog.addEventListener('close', () => opener?.focus({ preventScroll: true }));
            // Native dialog traps focus and handles Escape. Keep editor/viewer
            // shortcuts from also reacting to keys used inside its controls.
            for (const type of ['keydown', 'keyup']) dialog.addEventListener(type, event => event.stopPropagation());
            for (const group of document.querySelectorAll('#editor-container .mode-selection, #viewer-controls .viewer-top-row')) {
                const button = document.createElement('button');
                button.type = 'button';
                button.className = 'button lab-settings-open';
                button.textContent = '設定';
                button.addEventListener('click', () => { opener = button; dialog.showModal(); });
                group.append(button);
            }
        }
        applyTheme(mode);
        const editor = document.querySelector('#editor-container');
        const main = editor?.closest('.main-container');
        if (!main) return;
        const gameControls = document.querySelector('#game-controls');
        const viewer = document.querySelector('#viewer-container');
        const viewerControls = document.querySelector('#viewer-controls');
        const viewerCanvas = document.querySelector('#viewerCanvas');
        let frame = 0;
        function schedule() { if (!frame) frame = requestAnimationFrame(fit); }
        function set(name, value, target = main) { if (target.style.getPropertyValue(name) !== value) target.style.setProperty(name, value); }
        function fit() {
            frame = 0;
            if (!editor.offsetWidth) {
                if (viewer?.offsetWidth && viewerCanvas.width && viewerCanvas.height) {
                    root.dataset.labViewerFit = 'true';
                    const top = viewerControls.getBoundingClientRect().bottom + 12;
                    const height = Math.max(1, innerHeight - top - 10);
                    const width = Math.min(Math.max(1, innerWidth - 20), height * viewerCanvas.width / viewerCanvas.height);
                    set('--lab-viewer-center', (top + height / 2) + 'px', root);
                    set('--lab-viewer-width', width + 'px', root);
                } else delete root.dataset.labViewerFit;
                if (gameControls?.offsetWidth && document.querySelector('#game-container')?.offsetWidth) {
                    root.dataset.labPlayFit = 'true';
                    const top = gameControls.getBoundingClientRect().bottom + 12;
                    const height = Math.max(1, innerHeight - top - 10);
                    set('--lab-game-center', (top + height / 2) + 'px');
                    set('--lab-game-scale', String(editorFit(Math.max(1, innerWidth - 20), height, main.offsetWidth, main.offsetHeight)));
                } else delete root.dataset.labPlayFit;
                return;
            }
            delete root.dataset.labPlayFit;
            delete root.dataset.labViewerFit;
            const columns = [...editor.querySelectorAll('.editor-column')].filter(column => column.offsetWidth);
            const viewWidth = Math.max(1, innerWidth - 20), viewHeight = Math.max(1, innerHeight - 20);
            const minWidth = columns.length > 1 ? 720 : (document.querySelector('#case-selector') ? 720 : 440);
            const previousWidth = columns.length > 1 || document.querySelector('#case-selector') ? 1160 : 720;
            const layoutWidth = Math.max(minWidth, Math.min(previousWidth, viewWidth));
            set('--lab-editor-width', layoutWidth + 'px');
            set('--lab-editor-scale', '1');
            // Keep the prior card, field, spacing and button proportions intact.
            // Measure actual controls after wrapping, including case metadata.
            const scale = editorFit(viewWidth, viewHeight, main.offsetWidth, main.offsetHeight);
            set('--lab-editor-scale', String(scale));
        }
        new MutationObserver(schedule).observe(editor, { childList: true, subtree: true, attributes: true, attributeFilter: ['style', 'class'] });
        new ResizeObserver(schedule).observe(editor);
        new ResizeObserver(schedule).observe(main);
        if (gameControls) {
            new MutationObserver(schedule).observe(gameControls, { attributes: true, attributeFilter: ['style'] });
            new ResizeObserver(schedule).observe(gameControls);
        }
        if (viewerControls) {
            new MutationObserver(schedule).observe(viewerControls, { attributes: true, attributeFilter: ['style'] });
            new ResizeObserver(schedule).observe(viewerControls);
            new MutationObserver(schedule).observe(viewerCanvas, { attributes: true, attributeFilter: ['width', 'height'] });
        }
        window.addEventListener('resize', schedule);
        document.fonts?.ready.then(schedule);
        schedule();
    }
    window.addEventListener('storage', event => { if (event.key === key) applyTheme(event.newValue); });
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
    else start();
})();
