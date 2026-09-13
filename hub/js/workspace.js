import { createPanes } from './panes.js';
import { initialWorkflow, transition } from './workflow.js';
import { workspaceIdentity, readRecovery, writeRecovery } from './recovery.js';

const $ = id => document.getElementById(id);
const frames = { sim: $('iframe-sim'), editor: $('iframe-editor-custom') };
const initialized = {};
const readiness = Object.fromEntries(Object.keys(frames).map(role => [role, new Promise(resolve => { initialized[role] = resolve; })]));
const ready = new Set();
const requests = new Map();
const clone = value => value == null ? value : structuredClone(value);
let flow = initialWorkflow();
let currentDocument = null;
let practice = null;
let normalDraft = null;
let recordReturn = null;
let interruptedDismissed = false;
let serial = Promise.resolve();
let noticeTimer;
const parameters = new URLSearchParams(location.search);
const recoveryId = await workspaceIdentity({
    fresh: parameters.has('fresh') || parameters.has('entry') || parameters.has('practice') || Boolean(location.hash)
});
let recoveryEnabled = false;
let recoveryQueued = false;
let recoveryErrorShown = false;
let interruptedRecord = null;

function queueRecovery() {
    if (!recoveryEnabled || recoveryQueued) return;
    recoveryQueued = true;
    enqueue(async () => {
        try {
            const capturedAt = Date.now();
            const [sim, viewer] = await Promise.all([request('sim', 'recovery'), request('editor', 'recovery')]);
            if (currentDocument) currentDocument.context = viewer.context;
            const recoveryRecord = flow.mode === 'playing'
                ? (sim.replay ? { data: sim.replay, source: flow.playOrigin === 'practice' ? clone(practice?.source) : null } : null)
                : interruptedRecord;
            await writeRecovery(recoveryId, {
                version: 1, flow: clone(flow), currentDocument: clone(currentDocument),
                practice: clone(practice), normalDraft: clone(normalDraft), recordReturn: clone(recordReturn), sim: sim.state,
                simConfiguration: sim.configuration,
                interruptedRecord: clone(recoveryRecord),
                splitWidth: Number($('workspace-divider').getAttribute('aria-valuenow'))
            }, capturedAt);
        } catch (error) {
            if (!recoveryErrorShown) notice('作業状態の自動復元用データを保存できません。必要な記録は「共有」から書き出してください。');
            recoveryErrorShown = true;
            console.warn('Workspace recovery:', error);
        } finally { recoveryQueued = false; }
    });
}

function notice(text) {
    clearTimeout(noticeTimer);
    $('workspace-status').textContent = text;
    $('workspace-status').hidden = !text;
    if (text) noticeTimer = setTimeout(() => { $('workspace-status').hidden = true; }, 9000);
}
function enqueue(action) {
    serial = serial.then(action).catch(error => { console.error(error); notice(error.message || '操作を完了できませんでした。'); });
    return serial;
}
async function request(role, action, data) {
    await readiness[role];
    const requestId = crypto.randomUUID();
    return new Promise((resolve, reject) => {
        const timeout = setTimeout(() => { requests.delete(requestId); reject(new Error('画面から応答がありません。再読み込みしてお試しください。')); }, 15000);
        requests.set(requestId, { role, resolve, reject, timeout });
        frames[role].contentWindow.postMessage({ type: 'workspaceRequest', requestId, action, data }, location.origin);
    });
}
function inform(role, action, data) {
    if (ready.has(role)) frames[role].contentWindow.postMessage({ type: 'workspaceRequest', action, data }, location.origin);
}
function render() {
    document.body.dataset.mode = flow.mode;
    document.body.dataset.narrowPane = flow.narrowPane;
    panes.render();
    document.title = flow.mode === 'viewer' ? `${currentDocument?.title || 'リプレイ'} — Viewer` : 'Simulator';
    inform('editor', 'layout', { mode: flow.mode });
    $('interrupted-notice').hidden = !interruptedRecord || interruptedDismissed || flow.mode === 'playing' || flow.mode === 'viewer';
    requestAnimationFrame(() => {
        inform('sim', 'resize');
        inform('editor', 'resize');
        if (flow.mode === 'playing') frames.sim.contentWindow.focus();
    });
}
function move(event) { flow = transition(flow, event); render(); queueRecovery(); }
function showInterrupted() {
    if (interruptedRecord) return openReplay(interruptedRecord.data, { title: '中断前のプレイ記録', source: interruptedRecord.source, recording: true });
}
function newDocument(value, title) {
    return { id: crypto.randomUUID(), ...clone(value), title: title || value.title || 'リプレイ' };
}
async function displayDocument(document) {
    await request('editor', 'load', { input: document.data, context: document.context });
    currentDocument = clone(document);
}
async function openReplay(input, options = {}) {
    const returning = options.recording ? {
        flow: flow.mode === 'playing' ? transition(flow, 'return') : clone(flow),
        document: currentDocument ? { ...clone(currentDocument), ...await request('editor', 'document') } : null
    } : null;
    if (flow.mode === 'playing') await request('sim', 'stop');
    const loaded = await request('editor', 'load', { input, context: options.context });
    currentDocument = newDocument(loaded, options.title);
    recordReturn = returning;
    if (options.external) {
        if (normalDraft) await request('sim', 'apply', normalDraft);
        normalDraft = null;
        practice = null;
        flow = { ...flow, hasPractice: false };
    }
    if (options.source) currentDocument.source = clone(options.source);
    move('replay');
}
async function beginPractice(state, source) {
    recordReturn = null;
    if (flow.mode === 'playing') await request('sim', 'stop');
    if (!practice) normalDraft = await request('sim', 'state');
    const origin = newDocument(source, currentDocument?.title || source.title);
    origin.id = currentDocument?.id || origin.id;
    // Copy at the click, not when an asynchronous snapshot happens to finish.
    practice = { source: clone(origin), initialState: clone(state) };
    currentDocument = clone(origin);
    await request('sim', 'apply', state);
    move('practice');
}
async function returnFromRecord() {
    if (!recordReturn) return;
    const target = recordReturn;
    if (target.document) await displayDocument(target.document);
    else currentDocument = null;
    flow = clone(target.flow);
    recordReturn = null;
    render(); queueRecovery();
}
async function openInput(input, name = '') {
    const text = TetrisLinkFile.extract(input);
    let data;
    if (text.startsWith('{')) { try { data = JSON.parse(text); } catch { /* Viewer decoder supplies the error. */ } }
    else if (/^https?:/i.test(text)) {
        try {
            const url = new URL(text);
            if (!/\/F(?:\/|$)/i.test(url.pathname) && url.searchParams.get('entry') !== 'viewer') {
                data = JSON.parse(new TextDecoder().decode(Uint8Array.from(atob(url.hash.slice(1)), c => c.charCodeAt(0))));
            }
        } catch { /* Compressed viewer links are decoded by the viewer. */ }
    }
    if (data?.v === 2 && data.p1) {
        if (flow.mode === 'playing') await request('sim', 'stop');
        await request('sim', 'apply', data);
        normalDraft = null; practice = null; recordReturn = null;
        move('home');
    } else await openReplay(text, {external:true, title:name.replace(/\.tetrisevent\.json$|\.(?:json|url|html?|txt)$/i, '') || undefined});
}

window.addEventListener('message', event => {
    if (event.origin !== location.origin) return;
    const role = Object.keys(frames).find(key => event.source === frames[key].contentWindow);
    if (!role) return;
    const message = event.data;
    if (!message || typeof message !== 'object') return;
    if (message.type === 'workspaceResponse') {
        const pending = requests.get(message.requestId);
        if (!pending || pending.role !== role) return;
        clearTimeout(pending.timeout);
        requests.delete(message.requestId);
        message.error ? pending.reject(new Error(message.error)) : pending.resolve(message.value);
    } else if (message.type === 'workspaceReady') {
        ready.add(role);
        initialized[role]();
        render();
    } else if (role === 'sim' && message.type === 'workspaceStarted') {
        interruptedRecord = null;
        move('start');
    } else if (role === 'sim' && message.type === 'workspaceReturned') {
        move('return');
    } else if (message.type === 'workspaceImport' && typeof message.input === 'string') {
        enqueue(() => openInput(message.input, message.name));
    } else if (role === 'sim' && message.target === 'editor' && message.type === 'loadFumen') {
        const source = flow.playOrigin === 'practice' ? practice?.source : null;
        enqueue(() => openReplay(message.data, { title: '今回のプレイ記録', source, recording: true }));
    } else if (role === 'editor' && message.target === 'sim' && message.type === 'loadState') {
        enqueue(async () => beginPractice(message.data, message.practice || await request('editor', 'document')));
    }
});

const panes = createPanes({
    getFlow: () => flow,
    canReturn: () => Boolean(recordReturn),
    focus: role => move('focus-' + role),
    reveal: role => {
        if (flow.mode === 'viewer' && recordReturn) enqueue(returnFromRecord);
        else move(role ? 'focus-' + role : 'unfold');
    },
    changed: queueRecovery,
    resized: () => { inform('sim', 'resize'); inform('editor', 'resize'); }
});
window.addEventListener('resize', render);
$('open-interrupted').addEventListener('click', () => enqueue(showInterrupted));
$('dismiss-interrupted').addEventListener('click', () => { interruptedDismissed = true; render(); });
window.addEventListener('dragover', event => { if (event.dataTransfer.types.includes('Files')) event.preventDefault(); });
window.addEventListener('drop', event => {
    const file = event.dataTransfer.files[0];
    if (!file || !/\.(?:html?|url|json|txt|tetrisevent)$/i.test(file.name)) return;
    event.preventDefault(); enqueue(async () => openInput(await file.text(), file.name));
});

function applyTheme() {
    try { document.documentElement.dataset.labTheme = localStorage.getItem('lab-appearance-mode') === 'dark' ? 'dark' : 'light'; }
    catch { document.documentElement.dataset.labTheme = 'light'; }
}
applyTheme();
window.addEventListener('storage', event => { if (event.key === 'lab-appearance-mode') applyTheme(); });
document.addEventListener('keydown', event => {
    if (event.defaultPrevented || event.repeat || event.target.closest('input,textarea,select,button,[contenteditable]')) return;
    if (flow.mode === 'viewer') return;
    if (frames.sim.contentWindow?.PCFinder?.searchIfBoundKey?.(event.key)) event.preventDefault();
});

const entry = parameters.get('entry') || parameters.get('view') || 'simulator';
const simUrl = new URL('../index.html', location.href);
const viewerUrl = new URL('../F/index.html', location.href);
simUrl.search = parameters.toString();
simUrl.searchParams.set('workspace', '1');
simUrl.searchParams.delete('fresh');
viewerUrl.searchParams.set('workspace', '1');
if (parameters.get('motion') === '1') viewerUrl.searchParams.set('motion', '1');
const cleanWorkspaceLocation = location.pathname + (parameters.get('motion') === '1' ? '?motion=1' : '');
const isViewerEntry = entry === 'viewer' || location.hash.startsWith('#data=');
if (isViewerEntry) flow = transition(flow, 'replay');
if (!isViewerEntry && location.hash) simUrl.hash = location.hash;
frames.sim.src = simUrl.href;
frames.editor.src = viewerUrl.href;
render();
const startupTimeout = setTimeout(() => notice('画面の読み込みに時間がかかっています。通信状態を確認してください。'), 20000);
enqueue(async () => {
    await Promise.all(Object.values(readiness));
    clearTimeout(startupTimeout);
    if (parameters.has('practice')) {
        const key = parameters.get('practice');
        if (!key.startsWith('tetrisHubTransfer:')) throw new Error('練習の引き継ぎ情報が無効です。');
        const transfer = JSON.parse(sessionStorage.getItem(key) || 'null');
        if (!transfer) throw new Error('元のビューワーから、もう一度「この局面から練習」を押してください。');
        await openReplay(transfer.source.data, { title: transfer.source.title, context: transfer.source.context });
        await beginPractice(transfer.state, transfer.source);
        sessionStorage.removeItem(key);
        history.replaceState(history.state, '', cleanWorkspaceLocation);
    } else if (isViewerEntry) {
        if (location.hash) {
            const page = Number.parseInt(parameters.get('page'), 10);
            await openReplay(location.hash, { context: Number.isInteger(page) ? { pageIndex: page - 1 } : undefined });
        } else {
            currentDocument = newDocument(await request('editor', 'document'));
            move('replay');
        }
    } else if (!location.hash && !parameters.has('fresh')) {
        try {
            const recovery = await readRecovery(recoveryId);
            const saved = recovery?.state;
            if (saved?.version === 1 && saved.sim && ['simulator', 'viewer', 'split', 'playing'].includes(saved.flow?.mode)) {
                await request('sim', 'restore', { state: saved.sim, configuration: saved.simConfiguration });
                if (saved.currentDocument) await displayDocument(saved.currentDocument);
                practice = clone(saved.practice);
                normalDraft = clone(saved.normalDraft);
                recordReturn = clone(saved.recordReturn);
                interruptedRecord = clone(saved.interruptedRecord);
                flow = saved.flow.mode === 'playing' ? transition(saved.flow, 'return') : saved.flow;
                if (flow.mode === 'split' && !practice) flow = initialWorkflow();
                if (Number.isFinite(saved.splitWidth)) panes.setRatio(saved.splitWidth);
                render();
                if (!interruptedRecord) notice(saved.flow.mode === 'playing' ? 'このタブの作業を復元しました。試合は停止した準備画面に戻しています。' : 'このタブの作業を復元しました。');
            }
        } catch (error) {
            console.warn('Workspace recovery could not be read:', error);
            notice('このタブの状態を読み込めませんでした。共有リンクから開き直せます。');
        }
    }
    // Consume incoming links once, so a reload restores subsequent work instead
    // of importing the original link over the current practice draft.
    if (location.hash || isViewerEntry || parameters.has('fresh') || parameters.has('entry')) {
        history.replaceState(history.state, '', cleanWorkspaceLocation);
    }
    recoveryEnabled = true;
    document.body.dataset.workspaceReady = 'true';
    queueRecovery();
});
setInterval(queueRecovery, 2000);
document.addEventListener('visibilitychange', queueRecovery);
window.addEventListener('pagehide', queueRecovery);
if ('serviceWorker' in navigator) navigator.serviceWorker.register('./sw.js').catch(error => console.warn('Hub offline cache:', error));
