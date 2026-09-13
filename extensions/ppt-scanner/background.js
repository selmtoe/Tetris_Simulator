const SIMULATOR_URL = 'https://selmtoe.github.io/Tetris_Simulator/hub/?entry=simulator';
const DEFAULT_TITLE = 'この動画の局面をシミュレータへ送る';
const activeCaptures = new Map();

// Runs in each accessible video frame. Return images to the worker so one
// click creates one tab, even when a page contains several embedded videos.
function captureFrame() {
    const videos = [...document.querySelectorAll('video')].filter(video => {
        const rect = video.getBoundingClientRect();
        return rect.width > 0 && rect.height > 0 && getComputedStyle(video).visibility !== 'hidden';
    }).sort((a, b) => {
        const area = video => { const r = video.getBoundingClientRect(); return r.width * r.height; };
        return area(b) - area(a);
    });
    if (!videos.length) return {error: 'no-video'};
    let error = 'not-ready';
    for (const video of videos) {
        if (video.readyState < 2 || !video.videoWidth || !video.videoHeight) continue;
        try {
            const scale = Math.min(1, 1920 / video.videoWidth, 1080 / video.videoHeight);
            const canvas = document.createElement('canvas');
            canvas.width = Math.round(video.videoWidth * scale);
            canvas.height = Math.round(video.videoHeight * scale);
            canvas.getContext('2d').drawImage(video, 0, 0, canvas.width, canvas.height);
            const rect = video.getBoundingClientRect();
            return {imageData: canvas.toDataURL('image/jpeg', 0.95), area: rect.width * rect.height};
        } catch (_) { error = 'capture-blocked'; }
    }
    return {error};
}

// Runs in MAIN, because the simulator exposes this receiver on its own window.
// Hub is a shell: its simulator receiver belongs to a same-origin child frame.
// Never pass an image to the viewer or to an unrelated embedded origin.
async function receiveInSimulator(imageData) {
    function locate(win) {
        try {
            if (typeof win.receiveExtensionImage === 'function') return win;
            const frames = [...win.document.querySelectorAll('iframe')];
            frames.sort((a, b) => Number(b.id === 'iframe-sim') - Number(a.id === 'iframe-sim'));
            for (const frame of frames) {
                const found = locate(frame.contentWindow);
                if (found) return found;
            }
        } catch (_) { /* Cross-origin frames are deliberately ignored. */ }
        return null;
    }
    const receiver = locate(window);
    if (!receiver) return {accepted: false};
    await receiver.receiveExtensionImage(imageData);
    // The receiver starts recognition asynchronously; this is an acknowledgement
    // of delivery, not a claim that the model has finished recognizing the board.
    return {accepted: true};
}

function showNotice(message, isError) {
    document.getElementById('ppt-scanner-notice')?.remove();
    const notice = document.createElement('div');
    notice.id = 'ppt-scanner-notice';
    notice.setAttribute('role', isError ? 'alert' : 'status');
    notice.textContent = message;
    Object.assign(notice.style, {
        position: 'fixed', bottom: '24px', left: '50%', transform: 'translateX(-50%)',
        zIndex: '2147483647', maxWidth: 'min(520px, 90vw)', padding: '12px 18px',
        borderRadius: '12px', background: isError ? '#732e37' : '#283d42',
        color: '#fff', font: '14px/1.6 system-ui, sans-serif', boxShadow: '0 4px 20px #0003',
        pointerEvents: 'none'
    });
    (document.body || document.documentElement).append(notice);
    setTimeout(() => notice.remove(), isError ? 12000 : 6000);
}

async function status(tabId, badge, title, color = '#426c72') {
    await Promise.allSettled([
        chrome.action.setBadgeText({tabId, text: badge}),
        chrome.action.setBadgeBackgroundColor({tabId, color}),
        chrome.action.setTitle({tabId, title})
    ]);
}

async function notify(tabId, message, isError = false) {
    try {
        await chrome.scripting.executeScript({target: {tabId}, func: showNotice, args: [message, isError]});
    } catch (_) { /* The toolbar title still explains errors on restricted pages. */ }
}

async function getVideoImage(tabId) {
    let results;
    try {
        results = await chrome.scripting.executeScript({target: {tabId, allFrames: true}, func: captureFrame});
    } catch (_) {
        // The legacy extension has narrower host permissions. An inaccessible
        // advertising iframe must not prevent capturing the top-level video.
        results = await chrome.scripting.executeScript({target: {tabId}, func: captureFrame});
    }
    const captures = results.map(item => item.result).filter(Boolean);
    const image = captures.filter(item => item.imageData).sort((a, b) => b.area - a.area)[0];
    if (image) return image.imageData;
    if (captures.some(item => item.error === 'capture-blocked')) {
        throw new Error('動画の画像を取得できませんでした。YouTubeの動画ページで試してください。');
    }
    if (captures.some(item => item.error === 'not-ready')) {
        throw new Error('動画の読み込みを待つか、一度再生してからもう一度押してください。');
    }
    throw new Error('動画が見つかりません。YouTubeの動画を開いてから押してください。');
}

async function sendImage(imageData) {
    const target = await chrome.tabs.create({url: SIMULATOR_URL});
    const deadline = Date.now() + 45000;
    while (Date.now() < deadline) {
        let tab;
        try { tab = await chrome.tabs.get(target.id); }
        catch (_) { throw new Error('送り先のタブが閉じられました。'); }
        const url = tab.url || tab.pendingUrl || '';
        if (url && !url.startsWith('https://selmtoe.github.io/Tetris_Simulator/')) {
            throw new Error('送り先のページが切り替わったため、画像の送信を中止しました。');
        }
        // Poll the receiver itself, rather than a single onUpdated event that
        // can race with tab creation or precede the Hub iframe loading.
        if (tab.status === 'complete') {
            let results;
            try {
                results = await chrome.scripting.executeScript({
                    target: {tabId: target.id}, world: 'MAIN',
                    func: receiveInSimulator, args: [imageData]
                });
            } catch (error) {
                // Retry only if the document is navigating; a receiver failure
                // must not send the same image repeatedly.
                const current = await chrome.tabs.get(target.id);
                if (current.status !== 'loading') throw error;
            }
            if (results?.some(item => item.result?.accepted)) {
                await notify(target.id, '動画の画像を送りました。認識結果はシミュレータに表示されます。');
                return target.id;
            }
        }
        // Each tabs/scripting call also keeps this finite operation alive in MV3.
        await new Promise(resolve => setTimeout(resolve, 350));
    }
    throw new Error('シミュレータの読み込みが完了しませんでした。通信状態を確認してもう一度押してください。');
}

function startCapture(tabId) {
    if (!Number.isInteger(tabId)) return Promise.resolve({ok: false});
    if (activeCaptures.has(tabId)) return activeCaptures.get(tabId);
    const job = (async () => {
        await status(tabId, '…', '動画の画像を取得しています');
        try {
            const imageData = await getVideoImage(tabId);
            await status(tabId, '…', 'シミュレータへ画像を送っています');
            const targetTabId = await sendImage(imageData);
            await status(tabId, '', DEFAULT_TITLE);
            return {ok: true, targetTabId};
        } catch (error) {
            const message = error.message || '画像を送れませんでした。動画ページを開き直して試してください。';
            console.error('PPT Scanner:', error);
            await status(tabId, '!', message, '#9b3c4a');
            await notify(tabId, message, true);
            return {ok: false, error: message};
        }
    })().finally(() => activeCaptures.delete(tabId));
    activeCaptures.set(tabId, job);
    return job;
}

chrome.action.onClicked.addListener(tab => { void startCapture(tab.id); });
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
    if (request?.action !== 'capture_current_video' || sender.id !== chrome.runtime.id || sender.frameId !== 0) return;
    startCapture(sender.tab?.id).then(sendResponse);
    return true;
});
