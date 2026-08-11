import { elements, hubSettings, state } from './state.js';
import { toggleMenu } from './menu.js';
import { showToast } from './toast.js';
import { renderTaskbar } from './windows.js';

export function applyLayoutMode() {
    const mainToggleRect = elements.mainToggle.getBoundingClientRect();
    elements.desktopArea.className = `${hubSettings.layoutMode}-mode`;

    const winSim = document.getElementById('win-sim');
    const winCustom = document.getElementById('win-custom-editor');
    const winOfficial = document.getElementById('win-official-editor');

    winSim.classList.remove('active');
    winCustom.classList.remove('active');
    winOfficial.classList.remove('active');

    const btnSim = document.getElementById('btn-sim');
    const btnEditor = document.getElementById('btn-editor');
    const btnSwapSide = document.getElementById('btn-swap-side');

    if (hubSettings.layoutMode === 'window') {
        if (elements.fabMenu.classList.contains('open')) {
            elements.taskbar.classList.remove('hidden');
        } else {
            elements.taskbar.classList.add('hidden');
        }
        renderTaskbar();
        btnSim.style.display = 'none';
        btnEditor.style.display = 'none';
        btnSwapSide.style.display = 'none';
    } else {
        elements.taskbar.classList.add('hidden');
        const isOfficial = hubSettings.defaultEditor === 'official';
        const activeEditor = isOfficial ? winOfficial : winCustom;

        if (hubSettings.layoutMode === 'split') {
            winSim.classList.add('active');
            activeEditor.classList.add('active');
            document.querySelector('#icon-split path').setAttribute(
                'd',
                'M5 5h14v14H5zm0-2h14c1.1 0 2 .9 2 2v14c0 1.1-.9 2-2 2H5c-1.1 0-2-.9-2-2V5c0-1.1.9-2 2-2z'
            );
            btnSim.style.display = 'none';
            btnEditor.style.display = 'none';
            btnSwapSide.style.display = 'flex';
        } else if (hubSettings.layoutMode === 'tab') {
            if (state.activeTab === 'sim') {
                winSim.classList.add('active');
            } else {
                activeEditor.classList.add('active');
            }
            document.querySelector('#icon-split path').setAttribute(
                'd',
                'M4 18h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2H4c-1.1 0-2 .9-2 2v8c0 1.1.9 2 2 2zM4 8h6v8H4V8zm8 0h8v8h-8V8z'
            );
            btnSim.style.display = 'flex';
            btnEditor.style.display = 'flex';
            btnSwapSide.style.display = 'none';
        }
    }

    btnEditor.classList.toggle('active', state.activeTab === 'editor');
    btnSim.classList.toggle('active', state.activeTab === 'sim');

    if (elements.fabMenu.style.top) {
        elements.fabMenu.offsetHeight;
        const newMainToggleRect = elements.mainToggle.getBoundingClientRect();
        const diffY = mainToggleRect.top - newMainToggleRect.top;
        const currentTop = parseFloat(elements.fabMenu.style.top || 0);

        if (!Number.isNaN(diffY) && Math.abs(diffY) < 1000) {
            elements.fabMenu.style.top = `${currentTop + diffY}px`;
        }
    }
}

export function switchTab(app) {
    state.activeTab = app;
    if (hubSettings.layoutMode === 'tab') {
        applyLayoutMode();
    }
}

export function toggleSplitMode() {
    hubSettings.layoutMode = hubSettings.layoutMode === 'split' ? 'tab' : 'split';
    localStorage.setItem('tetrisHubSettings', JSON.stringify(hubSettings));
    applyLayoutMode();
    showToast(hubSettings.layoutMode === 'split' ? '分割モード' : 'タブモード');
    toggleMenu();
}

export function swapSides() {
    elements.desktopArea.classList.toggle('reverse');
    showToast('左右入替');
}

export function initializeSplitter() {
    elements.splitter.addEventListener('mousedown', startSplitterDrag);
    elements.splitter.addEventListener('touchstart', startSplitterDrag, { passive: false });
}

export function startSplitterDrag() {
    state.isDraggingSplitter = true;
    document.addEventListener('mousemove', onSplitterDrag);
    document.addEventListener('touchmove', onSplitterDrag, { passive: false });
    document.addEventListener('mouseup', stopSplitterDrag);
    document.addEventListener('touchend', stopSplitterDrag);
    document.querySelectorAll('iframe').forEach((frame) => {
        frame.style.pointerEvents = 'none';
    });
}

export function onSplitterDrag(event) {
    if (!state.isDraggingSplitter || hubSettings.layoutMode !== 'split') return;

    event.preventDefault();
    const clientX = event.touches ? event.touches[0].clientX : event.clientX;
    const containerWidth = elements.desktopArea.clientWidth;
    let percent = (clientX / containerWidth) * 100;
    percent = Math.max(10, Math.min(90, percent));

    const isReverse = elements.desktopArea.classList.contains('reverse');
    const activeEditor = hubSettings.defaultEditor === 'official'
        ? document.getElementById('win-official-editor')
        : document.getElementById('win-custom-editor');
    const simWin = document.getElementById('win-sim');

    if (!isReverse) {
        simWin.style.flex = `0 0 ${percent}%`;
        activeEditor.style.flex = '1';
    } else {
        activeEditor.style.flex = `0 0 ${percent}%`;
        simWin.style.flex = '1';
    }
}

export function stopSplitterDrag() {
    state.isDraggingSplitter = false;
    document.removeEventListener('mousemove', onSplitterDrag);
    document.removeEventListener('touchmove', onSplitterDrag);
    document.removeEventListener('mouseup', stopSplitterDrag);
    document.removeEventListener('touchend', stopSplitterDrag);
    document.querySelectorAll('iframe').forEach((frame) => {
        frame.style.pointerEvents = '';
    });
}
