/* Replay/practice navigation; all board drawing stays in the existing viewer. */
(() => {
    const embedded = window.parent !== window && new URLSearchParams(location.search).get('workspace') === '1';
    const notify = (type, data = {}) => window.parent.postMessage({ target: 'hub', source: 'editor', type, ...data }, location.origin);
    let ready = false;
    let workspaceActions;
    const context = () => ({ caseIndex: currentCaseIndex, pageIndex: currentPageIndex });
    function documentState() {
        return { data: getCollectionDataForExport(), context: context(), title: currentCase()?.name || 'リプレイ' };
    }
    function showViewer() {
        document.getElementById('view-mode-btn').click();
    }
    function restoreContext(value = {}) {
        if (Number.isInteger(value.caseIndex)) switchCase(value.caseIndex);
        if (Number.isInteger(value.pageIndex)) loadPage(Math.max(0, Math.min(fumenPages.length - 1, value.pageIndex)));
        showViewer();
    }
    async function loadDocument(input) {
        let data = input;
        if (typeof input === 'string') {
            const fumen = input.match(/v115@[\w+/?]*/);
            if (fumen) {
                const pages = FumenCodec.decode(fumen[0]).map(value => {
                    const page = createBlankPage();
                    page.p1 = { ...page.p1, ...value };
                    return page;
                });
                data = { v: 3, m: '1P', cases: [{ name: 'テト譜', kind: 'snapshot', gameMode: '1P', pages }] };
            } else {
                const oldHubHash = input.indexOf('#data=');
                const text = oldHubHash >= 0 ? input.slice(oldHubHash + 6) : input.replace(/^#?data=/, '');
                data = await decodeSharedStateText(text);
            }
        }
        if (data?.editor) data = data.editor; // Existing Hub files and shared links.
        let applied = false;
        if (data?.v === 3 || TetrisEventCodec.isEventReplay(data)) applied = applyCollectionData(data);
        else if (['f1', 'f2'].includes(data?.v)) applied = applyFumenData(data);
        else if (data?.v === 2 && data.p1) {
            const page = createBlankPage();
            for (const player of ['p1', 'p2']) {
                if (data[player]) page[player] = { ...page[player], board: stringToBoard(data[player].b), next: data[player].n || '', hold: data[player].h || '' };
            }
            applied = applyCollectionData({ v: 3, m: data.m || '1P', cases: [{ name: '局面', kind: 'snapshot', gameMode: data.m || '1P', pages: [page] }] });
        } else if (typeof applyVideoRecoveryData === 'function') applied = applyVideoRecoveryData(data);
        if (!applied) throw new Error('対応するリプレイファイル・共有リンクを選んでください。');
        showViewer();
        return documentState();
    }
    window.TetrisWorkspace = {
        practice(stateData) {
            const source = documentState();
            if (window.parent !== window) {
                window.parent.postMessage({ target: 'sim', type: 'loadState', data: stateData, practice: source }, embedded ? location.origin : '*');
            } else {
                // A standalone shared viewer can enter the same workflow without
                // putting a replay-sized payload into a query string.
                const key = `tetrisHubTransfer:${crypto.randomUUID()}`;
                try {
                    sessionStorage.setItem(key, JSON.stringify({ state: stateData, source }));
                    const url = new URL('../hub/', location.href);
                    url.searchParams.set('practice', key);
                    location.assign(url);
                } catch (error) {
                    alert('練習への引き継ぎを保存できませんでした。ブラウザの保存領域を確認してください。');
                }
            }
        }
    };
    window.addEventListener('message', async event => {
        if (!embedded || event.source !== window.parent || event.origin !== location.origin || !ready) return;
        const message = event.data;
        if (message?.type !== 'workspaceRequest') return;
        try {
            let value;
            switch (message.action) {
                case 'document': value = documentState(); break;
                case 'recovery': value = { context: context() }; break;
                case 'load':
                    value = await loadDocument(message.data.input);
                    restoreContext(message.data.context);
                    value = documentState();
                    break;
                case 'context': restoreContext(message.data); value = context(); break;
                case 'layout':
                    document.getElementById('viewer-simulator-btn').title = message.data.mode === 'split'
                        ? 'この局面を左に反映' : 'この局面から練習';
                    for (const [id, label, visible] of message.data.actions) {
                        let button = workspaceActions.querySelector(`#${id}`);
                        if (!button) {
                            button = document.createElement('button');
                            button.id = id;
                            button.type = 'button';
                            button.className = 'button';
                            button.textContent = label;
                            button.addEventListener('click', () => {
                                document.getElementById('share-close').click();
                                notify('workspaceAction', { action: id });
                            });
                            workspaceActions.append(button);
                        }
                        button.style.display = visible ? '' : 'none';
                    }
                    updateScale();
                    break;
                case 'resize': updateScale(); break;
                default: return;
            }
            notify('workspaceResponse', { requestId: message.requestId, value });
        } catch (error) {
            notify('workspaceResponse', { requestId: message.requestId, error: error.message });
        }
    });
    window.addEventListener('tetris:viewer-ready', () => {
        // Appearance remains shared with the simulator via the existing key.
        document.querySelector('#viewer-controls .lab-settings-open')?.remove();
        ready = true;
        if (embedded) {
            document.getElementById('back-to-editor-btn').style.display = 'none';
            const section = document.createElement('div');
            section.className = 'share-section';
            const heading = document.createElement('h3');
            heading.textContent = 'リプレイと練習';
            workspaceActions = document.createElement('div');
            workspaceActions.style.cssText = 'display:flex;flex-wrap:wrap;gap:8px';
            section.append(heading, workspaceActions);
            document.querySelector('#share-modal .modal-controls').before(section);
            document.getElementById('viewer-simulator-btn').title = 'この局面から練習';
            notify('workspaceReady');
        }
    });
})();
