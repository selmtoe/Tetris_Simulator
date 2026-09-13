// Crash recovery of the current workspace, not a user-managed replay library.
const DATABASE = 'tetris-workspace-recovery';
let database;
function openDatabase() {
    if (!database) database = new Promise((resolve, reject) => {
        const request = indexedDB.open(DATABASE, 1);
        request.onupgradeneeded = () => request.result.createObjectStore('workspaces', { keyPath: 'id' });
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => { database = null; reject(request.error); };
    });
    return database;
}
export function workspaceIdentity() {
    try {
        let id = sessionStorage.getItem('tetrisWorkspaceTab');
        if (!id) { id = crypto.randomUUID(); sessionStorage.setItem('tetrisWorkspaceTab', id); }
        return id;
    } catch { return crypto.randomUUID(); }
}
export async function readRecovery(id) {
    const db = await openDatabase();
    return new Promise((resolve, reject) => {
        const transaction = db.transaction('workspaces', 'readonly');
        const request = transaction.objectStore('workspaces').getAll();
        request.onsuccess = () => {
            const records = request.result;
            resolve(records.find(record => record.id === id) || records.sort((a,b) => b.updatedAt - a.updatedAt)[0] || null);
        };
        request.onerror = () => reject(request.error);
    });
}
export async function writeRecovery(id, state, capturedAt = Date.now()) {
    const db = await openDatabase();
    return new Promise((resolve, reject) => {
        const transaction = db.transaction('workspaces', 'readwrite');
        const store = transaction.objectStore('workspaces');
        const previous = store.get(id);
        previous.onsuccess = () => {
            // A slow write from a page being closed must not replace a newer
            // snapshot written by its restored replacement.
            if (previous.result?.updatedAt > capturedAt) return;
            store.put({ id, updatedAt: capturedAt, state });
            const request = store.getAll();
            request.onsuccess = () => {
                const records = request.result.sort((a,b) => b.updatedAt - a.updatedAt);
                for (const record of records.slice(8)) store.delete(record.id);
            };
        };
        transaction.oncomplete = () => resolve();
        transaction.onerror = () => reject(transaction.error);
        transaction.onabort = () => reject(transaction.error || new Error('Recovery transaction aborted'));
    });
}
