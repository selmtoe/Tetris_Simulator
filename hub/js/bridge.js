import { elements, hubSettings, state } from './state.js';
import { switchTab } from './layout.js';
import { finalizeSave } from './saves.js';
import { requestSnapshot } from './snapshot.js';
import { showToast } from './toast.js';

export function initializeBridge() {
    window.addEventListener('message', handleMessage);
}

export function handleMessage(event) {
    const data = event.data;
    if (!data) return;

    if (data.target === 'sim' && data.type === 'loadState') {
        elements.simFrame.contentWindow.postMessage(data, '*');
        if (hubSettings.layoutMode === 'tab') {
            switchTab('sim');
        }
        showToast('Editor -> Simulator');
        requestSnapshot('auto');
    } else if (data.target === 'editor' && data.type === 'loadFumen') {
        if (hubSettings.defaultEditor === 'official') {
            elements.editorCustomFrame.contentWindow.postMessage(data, '*');
            elements.editorCustomFrame.contentWindow.postMessage({ type: 'requestFumenUrl' }, '*');
        } else {
            elements.editorCustomFrame.contentWindow.postMessage(data, '*');
        }
        if (hubSettings.layoutMode === 'tab') {
            switchTab('editor');
        }
        showToast('Simulator -> Editor');
        requestSnapshot('auto');
    } else if (data.target === 'hub' && data.type === 'fumenUrlResponse') {
        elements.editorOfficialFrame.src = 'about:blank';
        setTimeout(() => {
            elements.editorOfficialFrame.src = data.url;
        }, 50);
    } else if (data.target === 'hub' && data.type === 'importUrlToSimResponse') {
        elements.simFrame.contentWindow.postMessage({ type: 'loadState', data: data.data }, '*');
        if (hubSettings.layoutMode === 'tab') {
            switchTab('sim');
        }
        showToast('Official -> Simulator');
        requestSnapshot('auto');
    } else if (data.target === 'hub' && data.type === 'saveSnapshotResponse') {
        if (data.source === 'editor') {
            state.pendingSnapshot.editor = data.data;
        }
        if (data.source === 'sim') {
            state.pendingSnapshot.sim = data.data;
        }
        if (state.pendingSnapshot.editor && state.pendingSnapshot.sim) {
            const mode = state.pendingSnapshot.mode;
            const snapshotData = { ...state.pendingSnapshot };
            state.pendingSnapshot = { editor: null, sim: null, mode: 'manual' };
            finalizeSave(snapshotData, mode);
        }
    } else if (data.target === 'hub' && data.type === 'switchTab') {
        if (hubSettings.layoutMode === 'tab') {
            switchTab(data.tab);
        }
    }
}

export async function importFromOfficial() {
    try {
        const url = await navigator.clipboard.readText();
        if (url && url.includes('v115@')) {
            elements.editorCustomFrame.contentWindow.postMessage({ type: 'importUrlToSim', url }, '*');
            showToast('インポートを処理中...');
        } else {
            showToast('クリップボードに有効なテト譜URLがありません');
        }
    } catch (error) {
        showToast('クリップボードの読み込みに失敗しました');
    }
}
