import { getAppWindow, hubSettings, state, winStates } from './state.js';
import { toggleMenu } from './menu.js';
import { showToast } from './toast.js';

export function saveWindowLayouts() {
    const layouts = {};

    document.querySelectorAll('.app-window').forEach((win) => {
        const id = win.getAttribute('data-id');
        layouts[id] = {
            left: win.style.left,
            top: win.style.top,
            width: win.style.width,
            height: win.style.height,
            zIndex: win.style.zIndex,
            maximized: winStates[id].maximized,
            prevRect: winStates[id].prevRect
        };
    });

    localStorage.setItem('tetrisHubWinLayouts', JSON.stringify(layouts));
}

export function resetWindowLayouts() {
    localStorage.removeItem('tetrisHubWinLayouts');
    initWindows(true);
    showToast('ウィンドウ配置を初期化しました');
    document.getElementById('hubSettingsModal').style.display = 'none';
    toggleMenu();
}

export function initWindows(forceReset = false) {
    const windows = document.querySelectorAll('.app-window');
    let savedLayouts = null;

    if (!forceReset) {
        try {
            savedLayouts = JSON.parse(localStorage.getItem('tetrisHubWinLayouts'));
        } catch (error) {
            // Preserve the prior fallback: invalid saved data means default positions.
        }
    }

    windows.forEach((win, index) => {
        const id = win.getAttribute('data-id');
        win.style.zIndex = ++state.maxZIndex;

        if (savedLayouts && savedLayouts[id]) {
            win.style.left = savedLayouts[id].left;
            win.style.top = savedLayouts[id].top;
            win.style.width = savedLayouts[id].width;
            win.style.height = savedLayouts[id].height;
            if (savedLayouts[id].zIndex) {
                win.style.zIndex = savedLayouts[id].zIndex;
            }
            winStates[id].maximized = savedLayouts[id].maximized;
            winStates[id].prevRect = savedLayouts[id].prevRect;
        } else {
            win.style.width = '400px';
            win.style.height = '600px';
            win.style.top = `${20 + index * 40}px`;
            win.style.left = `${20 + index * 40}px`;
            winStates[id].maximized = false;
            winStates[id].prevRect = null;
        }

        makeWindowDraggable(win);
        makeWindowResizable(win);

        win.addEventListener('mousedown', () => {
            if (hubSettings.layoutMode === 'window') {
                win.style.zIndex = ++state.maxZIndex;
                saveWindowLayouts();
            }
        });
    });
}

export function makeWindowDraggable(win) {
    const header = win.querySelector('.window-header');
    let isDrag = false;
    let startX;
    let startY;
    let initialLeft;
    let initialTop;

    header.addEventListener('mousedown', (event) => {
        if (event.target.tagName.toLowerCase() === 'button') return;
        if (hubSettings.layoutMode !== 'window') return;

        isDrag = true;
        startX = event.clientX;
        startY = event.clientY;
        initialLeft = win.offsetLeft;
        initialTop = win.offsetTop;
        win.style.zIndex = ++state.maxZIndex;
        win.querySelector('.window-body').style.pointerEvents = 'none';
    });

    document.addEventListener('mousemove', (event) => {
        if (!isDrag) return;
        win.style.left = `${initialLeft + (event.clientX - startX)}px`;
        win.style.top = `${initialTop + (event.clientY - startY)}px`;
    });

    document.addEventListener('mouseup', () => {
        if (!isDrag) return;

        isDrag = false;
        win.querySelector('.window-body').style.pointerEvents = '';
        if (winStates[win.getAttribute('data-id')].maximized) {
            toggleMaximizeWindow(win.getAttribute('data-id'));
        }
        saveWindowLayouts();
    });

    header.addEventListener('touchstart', (event) => {
        if (event.target.tagName.toLowerCase() === 'button' || hubSettings.layoutMode !== 'window') return;

        isDrag = true;
        startX = event.touches[0].clientX;
        startY = event.touches[0].clientY;
        initialLeft = win.offsetLeft;
        initialTop = win.offsetTop;
        win.style.zIndex = ++state.maxZIndex;
        win.querySelector('.window-body').style.pointerEvents = 'none';
    }, { passive: false });

    document.addEventListener('touchmove', (event) => {
        if (!isDrag) return;
        event.preventDefault();
        win.style.left = `${initialLeft + (event.touches[0].clientX - startX)}px`;
        win.style.top = `${initialTop + (event.touches[0].clientY - startY)}px`;
    }, { passive: false });

    document.addEventListener('touchend', () => {
        if (!isDrag) return;

        isDrag = false;
        win.querySelector('.window-body').style.pointerEvents = '';
        if (winStates[win.getAttribute('data-id')].maximized) {
            toggleMaximizeWindow(win.getAttribute('data-id'));
        }
        saveWindowLayouts();
    });
}

export function makeWindowResizable(win) {
    const resizer = win.querySelector('.window-resizer');
    let isResizing = false;
    let startX;
    let startY;
    let initialWidth;
    let initialHeight;

    resizer.addEventListener('mousedown', (event) => {
        if (hubSettings.layoutMode !== 'window') return;

        isResizing = true;
        startX = event.clientX;
        startY = event.clientY;
        initialWidth = win.offsetWidth;
        initialHeight = win.offsetHeight;
        win.querySelector('.window-body').style.pointerEvents = 'none';
        event.stopPropagation();
    });

    document.addEventListener('mousemove', (event) => {
        if (!isResizing) return;
        win.style.width = `${initialWidth + (event.clientX - startX)}px`;
        win.style.height = `${initialHeight + (event.clientY - startY)}px`;
    });

    document.addEventListener('mouseup', () => {
        if (!isResizing) return;

        isResizing = false;
        win.querySelector('.window-body').style.pointerEvents = '';
        saveWindowLayouts();
    });

    resizer.addEventListener('touchstart', (event) => {
        if (hubSettings.layoutMode !== 'window') return;

        isResizing = true;
        startX = event.touches[0].clientX;
        startY = event.touches[0].clientY;
        initialWidth = win.offsetWidth;
        initialHeight = win.offsetHeight;
        win.querySelector('.window-body').style.pointerEvents = 'none';
        event.stopPropagation();
    }, { passive: false });

    document.addEventListener('touchmove', (event) => {
        if (!isResizing) return;
        event.preventDefault();
        win.style.width = `${initialWidth + (event.touches[0].clientX - startX)}px`;
        win.style.height = `${initialHeight + (event.touches[0].clientY - startY)}px`;
    }, { passive: false });

    document.addEventListener('touchend', () => {
        if (!isResizing) return;

        isResizing = false;
        win.querySelector('.window-body').style.pointerEvents = '';
        saveWindowLayouts();
    });
}

export function minimizeWindow(id) {
    winStates[id].minimized = true;
    getAppWindow(id).classList.add('minimized');
    renderTaskbar();
}

export function restoreWindow(id) {
    winStates[id].minimized = false;
    const win = getAppWindow(id);
    win.classList.remove('minimized');
    win.style.zIndex = ++state.maxZIndex;
    renderTaskbar();
    saveWindowLayouts();
}

export function toggleMaximizeWindow(id) {
    const win = getAppWindow(id);

    if (winStates[id].maximized) {
        winStates[id].maximized = false;
        if (winStates[id].prevRect) {
            win.style.left = winStates[id].prevRect.left;
            win.style.top = winStates[id].prevRect.top;
            win.style.width = winStates[id].prevRect.width;
            win.style.height = winStates[id].prevRect.height;
        }
    } else {
        winStates[id].maximized = true;
        winStates[id].prevRect = {
            left: win.style.left,
            top: win.style.top,
            width: win.style.width,
            height: win.style.height
        };
        win.style.left = '0';
        win.style.top = '0';
        win.style.width = '100%';
        win.style.height = '100%';
    }

    win.style.zIndex = ++state.maxZIndex;
    saveWindowLayouts();
}

export function renderTaskbar() {
    const taskbar = document.getElementById('taskbar');
    taskbar.innerHTML = '';

    Object.keys(winStates).forEach((id) => {
        const button = document.createElement('div');
        button.className = `taskbar-item ${winStates[id].minimized ? '' : 'active'}`;
        button.textContent = winStates[id].name;
        button.onclick = () => {
            if (winStates[id].minimized) {
                restoreWindow(id);
            } else {
                minimizeWindow(id);
            }
        };
        taskbar.appendChild(button);
    });
}
