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
export async function workspaceIdentity({ fresh = false } = {}) {
    const navigation = performance.getEntriesByType('navigation')[0]?.type;
    const resuming = !fresh && ['reload', 'back_forward'].includes(navigation);
    let id = resuming ? history.state?.tetrisWorkspaceId : null;
    try {
        // sessionStorage may be copied by window.open or Duplicate Tab. Only
        // use it to resume this history entry, never for an ordinary new launch.
        if (resuming && !id) {
            id = sessionStorage.getItem('tetrisWorkspaceTab');
        }
    } catch { /* A storage-restricted tab can still run a new workspace. */ }
    if (typeof id !== 'string' || !id) id = crypto.randomUUID();
    if (navigator.locks?.request) {
        const claim = key => new Promise(resolve => {
            navigator.locks.request(`tetris-workspace:${key}`, { ifAvailable: true }, lock => {
                resolve(Boolean(lock));
                // The browser releases this lease when the document is gone.
                // A copied history/session identity cannot share its writer.
                if (lock) return new Promise(() => {});
            }).catch(() => resolve(null));
        });
        while (await claim(id) === false) id = crypto.randomUUID();
    }
    try { sessionStorage.setItem('tetrisWorkspaceTab', id); } catch { /* Optional legacy fallback. */ }
    history.replaceState({ ...history.state, tetrisWorkspaceId: id }, '');
    return id;
}
export async function readRecovery(id) {
    const db = await openDatabase();
    return new Promise((resolve, reject) => {
        const transaction = db.transaction('workspaces', 'readonly');
        const request = transaction.objectStore('workspaces').get(id);
        request.onsuccess = () => resolve(request.result || null);
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
            // Opening more tabs must not evict an older tab's recovery state.
        };
        transaction.oncomplete = () => resolve();
        transaction.onerror = () => reject(transaction.error);
        transaction.onabort = () => reject(transaction.error || new Error('Recovery transaction aborted'));
    });
}
