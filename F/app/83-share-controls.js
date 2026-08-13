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
        navigator.clipboard.writeText(text)
            .then(() => alert('テト譜リンクをコピーしました！'))
            .catch(err => alert('コピー失敗: ' + err));
    };

    document.getElementById('export-fumen-p1-btn').addEventListener('click', () => {
        const fumenData = FumenCodec.export(fumenPages, 'p1');
        const url = `https://knewjade.github.io/fumen-for-mobile/#?d=${fumenData}`;
        copyToClipboard(url);
    });

    document.getElementById('export-fumen-p2-btn').addEventListener('click', () => {
        const fumenData = FumenCodec.export(fumenPages, 'p2');
        const url = `https://knewjade.github.io/fumen-for-mobile/#?d=${fumenData}`;
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
        navigator.clipboard.writeText(input.value)
            .then(() => alert('共有リンクをクリップボードにコピーしました！'))
            .catch(err => alert('コピーに失敗しました: ' + err));
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

            let 
data;
            if (text.startsWith('http') && text.includes('#')) {
                // 自作URLの解析
                try {
                    const base64Data = text.substring(text.indexOf('#') + 1);
                    const binaryString = atob(base64Data);
                    const bytes = Uint8Array.from(binaryString, c => c.charCodeAt(0));
                    const jsonString = new TextDecoder().decode(bytes);
                    data = JSON.parse(jsonString);
                } catch(e) {
                     // 失敗したら通常JSONパースへ
                     data = JSON.parse(text);
                }
            } else {
                data = JSON.parse(text);
            }

            
            const applied = data?.v === 3 && typeof applyCollectionData === 'function'
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

    // ダブルタップによる拡大防止
    document.addEventListener('dblclick', function(event) {
        event.preventDefault();
    }, { passive: false });
});
