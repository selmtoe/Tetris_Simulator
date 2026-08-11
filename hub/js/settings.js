import { elements, hubSettings, state } from './state.js';
import { applyLayoutMode } from './layout.js';
import { toggleMenu } from './menu.js';
import { requestSnapshot } from './snapshot.js';
import { showToast } from './toast.js';

export function loadSettings() {
    try {
        const saved = localStorage.getItem('tetrisHubSettings');
        if (saved) {
            Object.assign(hubSettings, JSON.parse(saved));
        }
    } catch (error) {
        // Keep the default settings when local storage is invalid or unavailable.
    }

    applyLayoutMode();
    updateAutoSaveInterval();
}

export function updateAutoSaveInterval() {
    if (state.autoSaveTimer) {
        clearInterval(state.autoSaveTimer);
    }

    if (hubSettings.autoSaveInterval > 0) {
        state.autoSaveTimer = setInterval(() => {
            requestSnapshot('auto');
        }, hubSettings.autoSaveInterval);
    }
}

export function openSettings() {
    elements.settingsModal.style.display = 'flex';
    document.getElementById('setting-autosave').value = hubSettings.autoSaveInterval;
    document.getElementById('setting-editor').value = hubSettings.defaultEditor;
    document.getElementById('setting-layout').value = hubSettings.layoutMode;
    toggleMenu();
}

export function saveHubSettings() {
    hubSettings.autoSaveInterval = parseInt(document.getElementById('setting-autosave').value, 10);
    hubSettings.defaultEditor = document.getElementById('setting-editor').value;
    hubSettings.layoutMode = document.getElementById('setting-layout').value;

    localStorage.setItem('tetrisHubSettings', JSON.stringify(hubSettings));
    applyLayoutMode();
    updateAutoSaveInterval();
    elements.settingsModal.style.display = 'none';
    showToast('設定を保存しました');
}

export function triggerDevMode() {
    state.devModeCount += 1;
    if (state.devModeCount >= 10) {
        document.getElementById('row-layout').style.display = 'flex';
        document.getElementById('opt-layout-window').style.display = 'block';
        document.getElementById('btn-reset-layout').style.display = 'block';
        showToast('ウィンドウモードが有効になりました');
        state.devModeCount = 0;
    }
}
