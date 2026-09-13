/* Hub navigation only. The simulator keeps its native controls and renderer. */
(() => {
    const embedded = window.parent !== window && new URLSearchParams(location.search).get('workspace') === '1';
    if (!embedded) return;
    const notify = (type, data = {}) => window.parent.postMessage({ target: 'hub', source: 'sim', type, ...data }, location.origin);
    let ready = false;
    let stopping = false;
    let referenceButton;
    let recoveryButton;
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
                case 'layout':
                    referenceButton.style.display = message.data.mode === 'split' ? '' : 'none';
                    recoveryButton.style.display = message.data.interrupted ? '' : 'none';
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
        document.getElementById('exportFumenBtn').title = '今回のプレイ記録をビューワーで開く';
        const actionButton = (id, text, action) => {
            const button = document.createElement('button');
            button.id = id;
            button.type = 'button';
            button.className = 'button';
            button.textContent = text;
            button.addEventListener('click', () => {
                document.getElementById('share-close').click();
                notify('workspaceAction', { action });
            });
            return button;
        };
        // Keep the native toolbar's geometry. Additional actions live in the
        // existing Share dialog, including reference navigation in practice.
        const importSection = document.getElementById('import-from-data-btn').parentElement;
        importSection.append(actionButton('workspace-open-replay', 'リプレイを開く', 'records'));
        recoveryButton = actionButton('workspace-interrupted', '中断前の記録を見る', 'interrupted');
        recoveryButton.style.display = 'none';
        importSection.append(recoveryButton);
        referenceButton = actionButton('workspace-reference', 'ビューワー', 'reference');
        referenceButton.title = '元リプレイを表示する';
        referenceButton.style.display = 'none';
        importSection.append(referenceButton);
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
