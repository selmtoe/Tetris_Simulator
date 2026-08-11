// Hubからのメッセージ受信リスナー (DOMContentLoadedの外に出す)
window.addEventListener('message', (e) => {
    if (e.data && e.data.type === 'loadFumen') {
        const fumenData = e.data.data;
        if (typeof applyFumenData === 'function' && applyFumenData(fumenData)) {
           console.log("Applied fumen data from Hub");
        }
    } else if (e.data && e.data.type === 'requestState') {
        try {
            const fumenData = getFumenDataForExport();
            window.parent.postMessage({
                target: 'hub',
                type: 'saveSnapshotResponse',
                source: 'editor',
                data: fumenData
            }, '*');
        } catch (err) {
            console.error("Export Error:", err);
        }
    } else if (e.data && e.data.type === 'requestFumenUrl') {
        try {
            const fumenData = FumenCodec.export(fumenPages, 'p1');
            const url = `https://knewjade.github.io/fumen-for-mobile/#?d=${fumenData}`;
            window.parent.postMessage({
                target: 'hub',
                type: 'fumenUrlResponse',
                url: url
            }, '*');
        } catch (err) {
            console.error(err);
        }
    } else if (e.data && e.data.type === 'importUrlToSim') {
        const urlStr = e.data.url;
        try {
            const match = urlStr.match(/v115@[\w+/?]*/);
            if (match) {
                const fumenPagesData = FumenCodec.decode(match[0]);
                if (fumenPagesData && fumenPagesData.length > 0) {
                    fumenPages = [];
                    fumenPagesData.forEach(p => {
                        const newPage = createBlankPage();
                        newPage.p1 = { ...newPage.p1, board: p.board, hold: p.hold, next: p.next };
                        fumenPages.push(newPage);
                    });
                    gameMode = '1P';
                    document.getElementById('mode-1p').classList.add('active');
                    document.getElementById('mode-2p').classList.remove('active');
                    document.getElementById('p2-editor-col').style.display = 'none';
                    currentPageIndex = 0;
                    loadPage(0);
                    
                    const stateData = {
                        v: 2, m: gameMode,
                        p1: { 
                            b: boardToString(fumenPages[0].p1.board), 
                            n: (fumenPages[0].p1.next || '').replace(/[^IOTLSJZ]/gi, ''), 
                            h: (fumenPages[0].p1.hold || '').replace(/[^IOTLSJZ]/gi, '') 
                        }
                    };
                    window.parent.postMessage({
                        target: 'hub',
                        type: 'importUrlToSimResponse',
                        data: stateData
                    }, '*');
                }
            }
        } catch (err) {
            console.error(err);
        }
    }
});

