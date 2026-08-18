document.addEventListener('DOMContentLoaded', () => {
    document.getElementById('send-to-simulator').addEventListener('click', sendToSimulator);

    // 共通の共有ボタン処理
    const handleShareClick = () => {
        openShareModal();
        // モードに応じてボタン表示切り替え
        document.getElementById('export-fumen-p2-btn').style.display = (gameMode === '2P') ? 'inline-block' : 'none';
    };

    document.getElementById('share-btn').addEventListener('click', handleShareClick);
    
    // Viewer側のShareボタンにも同じ処理を割り当て
    const viewerShareBtn = document.getElementById('viewer-share-btn');
    if (viewerShareBtn) {
        viewerShareBtn.addEventListener('click', handleShareClick);
    }


    const copyToClipboard = (text) => {
        const fallbackCopy = () => {
            const helper = document.createElement('textarea');
            helper.value = text;
            helper.style.position = 'fixed';
            helper.style.opacity = '0';
            document.body.appendChild(helper);
            helper.select();
            const copied = document.execCommand('copy');
            helper.remove();
            if (!copied) throw new Error('copy command failed');
        };
        const copy = navigator.clipboard?.writeText
            ? navigator.clipboard.writeText(text).catch(() => fallbackCopy())
            : Promise.resolve().then(fallbackCopy);
        copy.then(() => alert('リンクをコピーしました。'))
            .catch(err => alert('コピーに失敗しました: ' + err));
    };

    const setOpenLink = (id, url) => {
        const link = document.getElementById(id);
        if (!link) return;
        link.href = url;
        link.hidden = false;
    };

    document.getElementById('export-fumen-p1-btn').addEventListener('click', () => {
        const fumenData = FumenCodec.export(fumenPages, 'p1');
        const url = `https://knewjade.github.io/fumen-for-mobile/#?d=${fumenData}`;
        setOpenLink('fumen-p1-open', url);
        copyToClipboard(url);
    });

    document.getElementById('export-fumen-p2-btn').addEventListener('click', () => {
        const fumenData = FumenCodec.export(fumenPages, 'p2');
        const url = `https://knewjade.github.io/fumen-for-mobile/#?d=${fumenData}`;
        setOpenLink('fumen-p2-open', url);
        copyToClipboard(url);
    });

    document.getElementById('share-close').addEventListener('click', () => {
        document.getElementById('share-modal').style.display = 'none';
    });

    document.getElementById('share-close').addEventListener('click', () => {
        document.getElementById('share-modal').style.display = 'none';
    });
    document.getElementById('copy-link-btn').addEventListener('click', () => {
        const input = document.getElementById('share-link-input');
        input.select();
        copyToClipboard(input.value);
    });
    document.getElementById('export-event-file-btn')?.addEventListener('click', () => {
        const data = getCollectionDataForExport();
        const url = URL.createObjectURL(new Blob([JSON.stringify(data)], { type: 'application/json' }));
        const link = document.createElement('a');
        link.href = url;
        link.download = `tetris_replay_${new Date().toISOString().replace(/[:.]/g, '-')}.tetrisevent.json`;
        link.click();
        URL.revokeObjectURL(url);
    });
    document.getElementById('import-from-data-btn').addEventListener('click', async () => {
        try {
            const text = await navigator.clipboard.readText();
            if (!text) {
                alert('クリップボードが空です。');
                return;
            }

            // テト譜 URL/データ チェック
            if (text.includes('v115@')) {
                const match = text.match(/v115@[\w+/?]*/);
                if (match) {
                    const fumenPagesData = FumenCodec.decode(match[0]);
                    if (fumenPagesData) {
                            fumenPages = [];
                            fumenCases = [createCase('Imported Fumen', 'snapshot')];
                            fumenCases[0].pages = fumenPages;
                            currentCaseIndex = 0;
                            fumenPagesData.forEach(p => {
                                const newPage = createBlankPage();
                            newPage.p1 = { ...newPage.p1, board: p.board, hold: p.hold, next: p.next, operation: p.operation || null };
                            fumenPages.push(newPage);
                        });
                        gameMode = '1P';
                        document.getElementById('mode-1p').click();
                        currentPageIndex = 0;
                        loadPage(0);
                        alert('テト譜データを読み込みました。');
                        document.getElementById('share-modal').style.display = 'none';
                        return;
                    }
                }
            }

            const data = typeof decodeSharedStateText === 'function'
                ? await decodeSharedStateText(text)
                : JSON.parse(text);

            
            const applied = data?.simulatorData || data?.eventReplay || data?.pageFormat === 'operation-pages/v1' || data?.version === 5 || data?.version === 6
                ? applyVideoRecoveryData(data)
                : (data?.v === 3 || (typeof TetrisEventCodec !== 'undefined' && TetrisEventCodec.isEventReplay(data))) && typeof applyCollectionData === 'function'
                    ? applyCollectionData(data)
                    : applyFumenData(data);
            if(applied) {
                 alert('クリップボードから譜面データを読み込みました。');
                 document.getElementById('share-modal').style.display = 'none';
            }

        } catch (e) {
            alert('クリップボードのデータが無効か、読み込みに失敗しました。');
            console.error('Failed to import from clipboard:', e);
        }
    });

    const eventFileInput = document.getElementById('import-event-file');
    document.getElementById('import-event-file-btn')?.addEventListener('click', () => eventFileInput?.click());
    eventFileInput?.addEventListener('change', async () => {
        const file = eventFileInput.files?.[0];
        if (!file) return;
        try {
            const data = typeof decodeSharedStateText === 'function'
                ? await decodeSharedStateText(await file.text())
                : JSON.parse(await file.text());
            const applied = data?.simulatorData || data?.eventReplay ||
                data?.pageFormat === 'operation-pages/v1' || data?.version === 5 || data?.version === 6
                ? applyVideoRecoveryData(data)
                : (data?.v === 3 || (typeof TetrisEventCodec !== 'undefined' && TetrisEventCodec.isEventReplay(data))) &&
                    typeof applyCollectionData === 'function'
                    ? applyCollectionData(data)
                    : applyFumenData(data);
            if (!applied) throw new Error('unsupported replay data');
            alert('リプレイファイルを読み込みました。');
            document.getElementById('share-modal').style.display = 'none';
        } catch (error) {
            console.error('Failed to import replay file:', error);
            alert('リプレイファイルが無効か、読み込みに失敗しました。');
        } finally {
            eventFileInput.value = '';
        }
    });

    // ダブルタップによる拡大防止
    document.addEventListener('dblclick', function(event) {
        event.preventDefault();
    }, { passive: false });
});
