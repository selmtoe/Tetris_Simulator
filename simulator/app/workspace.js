/* Hub navigation only. The simulator keeps its native controls and renderer. */
(() => {
    const embedded = window.parent !== window && new URLSearchParams(location.search).get('workspace') === '1';
    if (!embedded) return;
    const notify = (type, data = {}) => window.parent.postMessage({ target: 'hub', source: 'sim', type, ...data }, location.origin);
    let ready = false;
    let stopping = false;
    window.addEventListener('message', event => {
        if (event.source !== window.parent || event.origin !== location.origin) return;
        const message = event.data;
        if (message?.type !== 'workspaceRequest' || !ready) return;
        try {
            let value;
            switch (message.action) {
                case 'state': value = getGameStateForExport(); break;
                case 'recovery':
                    value = {
                        state: getGameStateForExport(),
                        configuration: { p1Ai: document.getElementById('p1-ai-toggle').checked, p2Ai: document.getElementById('p2-ai-toggle').checked },
                        replay: gameState === 'PLAYING' ? window.createRecordedReplayCollection?.() : null
                    };
                    break;
                case 'restore':
                    if (!applyGameState(message.data.state)) throw new Error('準備状態を復元できませんでした。');
                    if (message.data.configuration) {
                        for (const player of ['p1', 'p2']) {
                            const toggle = document.getElementById(`${player}-ai-toggle`);
                            toggle.checked = Boolean(message.data.configuration[`${player}Ai`]);
                            toggle.dispatchEvent(new Event('change', { bubbles: true }));
                        }
                    }
                    value = getGameStateForExport();
                    break;
                case 'apply':
                    autoStartParams = { ss: false, nh: false, hb: false };
                    if (!applyGameState(message.data)) throw new Error('局面を読み込めませんでした。');
                    value = getGameStateForExport();
                    break;
                case 'stop':
                    if (gameState === 'PLAYING') {
                        stopping = true;
                        document.getElementById('backToEditorBtn').click();
                        stopping = false;
                    }
                    value = getGameStateForExport();
                    break;
                case 'resize': updateScale(); break;
                default: return;
            }
            notify('workspaceResponse', { requestId: message.requestId, value });
        } catch (error) {
            stopping = false;
            notify('workspaceResponse', { requestId: message.requestId, error: error.message });
        }
    });
    document.addEventListener('DOMContentLoaded', () => {
        document.getElementById('exportFumenBtn').textContent = '記録を見る';
        document.getElementById('startGameBtn').addEventListener('click', () => {
            if (gameState === 'PLAYING') notify('workspaceStarted');
        });
        document.getElementById('backToEditorBtn').addEventListener('click', () => {
            if (!stopping) notify('workspaceReturned');
        });
        window.addEventListener('blur', () => {
            players.forEach(player => { player.keys = {}; });
        });
        ready = true;
        notify('workspaceReady');
    });
})();
