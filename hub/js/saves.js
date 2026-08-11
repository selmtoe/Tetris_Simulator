import { elements, state } from './state.js';
import { toggleMenu } from './menu.js';
import { requestSnapshot } from './snapshot.js';
import { showToast } from './toast.js';

const MANUAL_STORAGE_KEY = 'tetrisHubData';
const AUTO_STORAGE_KEY = 'tetrisHubAutoData';

function currentStorageKey() {
    return state.currentFileTab === 'auto' ? AUTO_STORAGE_KEY : MANUAL_STORAGE_KEY;
}

function readSavedItems(storageKey) {
    return JSON.parse(localStorage.getItem(storageKey) || '[]');
}

export function saveCurrentState() {
    showToast('状態を取得中...');
    requestSnapshot('manual');
    toggleMenu();
}

export function finalizeSave(snapshot, mode) {
    const timestamp = new Date().toLocaleString();
    let name = timestamp;

    if (mode === 'manual') {
        name = prompt('保存名を入力してください:', timestamp);
        if (!name) return;
    } else {
        name = `[Auto] ${timestamp}`;
    }

    const saveItem = {
        id: Date.now(),
        name,
        date: timestamp,
        editor: snapshot.editor,
        sim: snapshot.sim
    };
    const storageKey = mode === 'auto' ? AUTO_STORAGE_KEY : MANUAL_STORAGE_KEY;

    try {
        let saved = readSavedItems(storageKey);
        saved.unshift(saveItem);
        if (mode === 'auto' && saved.length > 20) {
            saved = saved.slice(0, 20);
        }
        localStorage.setItem(storageKey, JSON.stringify(saved));
        if (mode === 'manual') {
            showToast('保存しました');
        }
    } catch (error) {
        if (mode === 'manual') {
            alert('保存に失敗しました');
        }
    }
}

export function switchSaveTab(tab) {
    state.currentFileTab = tab;
    document.getElementById('tab-manual').classList.toggle('active', tab === 'manual');
    document.getElementById('tab-auto').classList.toggle('active', tab === 'auto');
    renderFileList();
}

export function openFileManager() {
    renderFileList();
    elements.fileModal.style.display = 'flex';
    toggleMenu();
}

export function renderFileList() {
    const list = document.getElementById('fileList');
    list.innerHTML = '';

    const saved = readSavedItems(currentStorageKey());
    if (saved.length === 0) {
        list.innerHTML = '<div style="padding:10px;text-align:center;color:#888;">データがありません</div>';
        return;
    }

    saved.forEach((item) => {
        const div = document.createElement('div');
        div.className = 'file-item';
        div.innerHTML = `
                    <div class="file-info" onclick="loadFile(${item.id})">
                        <div style="font-weight:bold;">${escapeHtml(item.name)}</div>
                        <div class="file-date">${item.date}</div>
                    </div>
                    <button class="rename-btn" onclick="shareIndividual(${item.id})" title="共有用URLコピー">
                        <svg viewBox="0 0 24 24"><path d="M3.9 12c0-1.71 1.39-3.1 3.1-3.1h4V7H7c-2.76 0-5 2.24-5 5s2.24 5 5 5h4v-1.9H7c-1.71 0-3.1-1.39-3.1-3.1zM8 13h8v-2H8v2zm9-6h-4v1.9h4c1.71 0 3.1 1.39 3.1 3.1s-1.39 3.1-3.1 3.1h-4V17h4c2.76 0 5-2.24 5-5s-2.24-5-5-5z"/></svg>
                    </button>
                    <button class="rename-btn" onclick="exportIndividual(${item.id})" title="個別ダウンロード">
                        <svg viewBox="0 0 24 24"><path d="M19 9h-4V3H9v6H5l7 7 7-7zm-14 9v2h14v-2H5z"/></svg>
                    </button>
                    <button class="rename-btn" onclick="renameFile(${item.id})">：</button>
                    <button class="delete-btn" onclick="deleteFile(${item.id})">×</button>
                `;
        list.appendChild(div);
    });
}

export function loadFile(id) {
    const saved = readSavedItems(currentStorageKey());
    const item = saved.find((entry) => entry.id === id);
    if (!item) return;

    if (confirm(`「${item.name}」を読み込みますか？\n現在の作業内容は上書きされます。`)) {
        if (item.editor) {
            elements.editorCustomFrame.contentWindow.postMessage({ type: 'loadFumen', data: item.editor }, '*');
        }
        if (item.sim) {
            elements.simFrame.contentWindow.postMessage({ type: 'loadState', data: item.sim }, '*');
        }
        elements.fileModal.style.display = 'none';
        showToast('読み込みました');
    }
}

export function deleteFile(id) {
    if (!confirm('削除しますか？')) return;

    let saved = readSavedItems(currentStorageKey());
    saved = saved.filter((item) => item.id !== id);
    localStorage.setItem(currentStorageKey(), JSON.stringify(saved));
    renderFileList();
}

export function renameFile(id) {
    const storageKey = currentStorageKey();
    const saved = readSavedItems(storageKey);
    const index = saved.findIndex((item) => item.id === id);
    if (index === -1) return;

    const newName = prompt('新しい名前を入力してください:', saved[index].name);
    if (newName && newName.trim() !== '') {
        saved[index].name = newName.trim();
        localStorage.setItem(storageKey, JSON.stringify(saved));
        renderFileList();
    }
}

export function deleteAllFiles() {
    const storageKey = currentStorageKey();
    if (confirm('表示中のタブのデータをすべて削除しますか？')) {
        localStorage.removeItem(storageKey);
        renderFileList();
        showToast('全削除しました');
    }
}

export function exportIndividual(id) {
    const item = readSavedItems(currentStorageKey()).find((entry) => entry.id === id);
    if (!item) return;

    const blob = new Blob([JSON.stringify([item])], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = `${item.name}.json`;
    anchor.click();
    URL.revokeObjectURL(url);
}

export function shareIndividual(id) {
    const item = readSavedItems(currentStorageKey()).find((entry) => entry.id === id);
    if (!item) return;

    const dataString = btoa(unescape(encodeURIComponent(JSON.stringify(item))));
    const shareUrl = `${window.location.origin}${window.location.pathname}#data=${dataString}`;
    navigator.clipboard.writeText(shareUrl).then(() => {
        showToast('共有URLをコピーしました');
    });
}

export function exportData() {
    const saved = localStorage.getItem(MANUAL_STORAGE_KEY) || '[]';
    if (saved === '[]') {
        alert('保存されたデータがありません。');
        return;
    }

    const blob = new Blob([saved], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = `tetris_hub_backup_${new Date().toISOString().slice(0, 10)}.json`;
    document.body.appendChild(anchor);
    anchor.click();
    document.body.removeChild(anchor);
    URL.revokeObjectURL(url);
    showToast('書き出しました');
}

export function triggerImport() {
    document.getElementById('importFile').click();
}

export function importData(input) {
    const file = input.files[0];
    if (!file) return;

    const reader = new FileReader();
    reader.onload = (event) => {
        try {
            const imported = JSON.parse(event.target.result);
            if (!Array.isArray(imported)) {
                throw new Error('Invalid format');
            }
            const current = readSavedItems(MANUAL_STORAGE_KEY);
            const newItems = imported.map((item) => ({
                ...item,
                id: Date.now() + Math.floor(Math.random() * 100000)
            }));
            const merged = [...newItems, ...current];
            localStorage.setItem(MANUAL_STORAGE_KEY, JSON.stringify(merged));
            openFileManager();
            showToast('読み込みました');
        } catch (error) {
            alert(`ファイルの読み込みに失敗しました: ${error.message}`);
        }
        input.value = '';
    };
    reader.readAsText(file);
}

export function loadFromUrlInput() {
    const input = document.getElementById('importUrlInput');
    const urlString = input.value.trim();
    if (!urlString) return;

    try {
        const hashIndex = urlString.indexOf('#data=');
        if (hashIndex === -1) {
            showToast('有効なデータURLではありません');
            return;
        }
        const dataString = decodeURIComponent(escape(atob(urlString.substring(hashIndex + 6))));
        const item = JSON.parse(dataString);
        if (item.editor) {
            elements.editorCustomFrame.contentWindow.postMessage({ type: 'loadFumen', data: item.editor }, '*');
        }
        if (item.sim) {
            elements.simFrame.contentWindow.postMessage({ type: 'loadState', data: item.sim }, '*');
        }
        showToast('URLから読み込みました');
        elements.fileModal.style.display = 'none';
        input.value = '';
    } catch (error) {
        showToast('データの読み込みに失敗しました');
    }
}

export function loadStateFromSharedHash() {
    const hash = window.location.hash;
    if (!hash.startsWith('#data=')) return;

    try {
        const dataString = decodeURIComponent(escape(atob(hash.substring(6))));
        const item = JSON.parse(dataString);
        if (item.editor) {
            elements.editorCustomFrame.contentWindow.postMessage({ type: 'loadFumen', data: item.editor }, '*');
        }
        if (item.sim) {
            elements.simFrame.contentWindow.postMessage({ type: 'loadState', data: item.sim }, '*');
        }
        showToast('URLから読み込みました');
        history.replaceState(null, null, ' ');
    } catch (error) {
        console.error('URLデータのパース失敗', error);
    }
}

export function escapeHtml(value) {
    return value.replace(/[&<>"']/g, (match) => ({
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#039;'
    })[match]);
}
