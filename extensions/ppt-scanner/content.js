(() => {
    // Retain the personal Lab's existing scan button. The toolbar itself uses
    // fresh script injection, so reloading an extension does not require a
    // reload of every already-open YouTube tab.
    if (window !== window.top || window.__pptScannerBridgeInstalled) return;
    const allowed = ['localhost', '127.0.0.1', '[::1]', 'selmtoe.github.io'];
    if (!allowed.includes(location.hostname)) return;
    window.__pptScannerBridgeInstalled = true;
    const notify = () => window.postMessage({type: 'PPT_EXTENSION_DETECTED'}, location.origin);
    notify();
    setTimeout(notify, 1000);
    window.addEventListener('message', event => {
        if (event.source !== window || event.origin !== location.origin || event.data?.type !== 'PPT_TRIGGER_SCAN') return;
        chrome.runtime.sendMessage({action: 'capture_current_video'}, response => {
            const error = chrome.runtime.lastError;
            window.postMessage({
                type: 'PPT_SCAN_RESULT', ok: !error && Boolean(response?.ok),
                error: error ? 'PPT Scannerを再読み込みした場合、このページも再読み込みしてください。' : response?.error
            }, location.origin);
        });
    });
})();
