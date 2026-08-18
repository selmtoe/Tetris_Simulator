document.addEventListener('DOMContentLoaded', async () => {
    // 隠し機能: cleanモード (URLパラメータに clean があればUIを隠す)


    const urlParams = new URLSearchParams(window.location.search);
    const requestedView = urlParams.get('view');
    const requestedPageNumber = Number.parseInt(urlParams.get('page'), 10);
    if (urlParams.has('clean')) {
        const viewerControls = document.getElementById('viewer-controls');
        if (viewerControls) viewerControls.style.display = 'none';
    }

    fumenPages.push(createBlankPage());
    fumenCases = [createCase('Case 1', 'snapshot')];
    fumenCases[0].pages = fumenPages;
    currentCaseIndex = 0;
    setupEditors();
    loadPage(0);
    await loadStateFromURL();
    if (Number.isInteger(requestedPageNumber) && fumenPages.length) {
        const requestedPageIndex = Math.max(0, Math.min(fumenPages.length - 1, requestedPageNumber - 1));
        loadPage(requestedPageIndex);
    }
viewerCanvas = document.getElementById('viewerCanvas');
    viewerCtx = viewerCanvas.getContext('2d');

    document.getElementById('editor-container').style.display = 'none';
    document.getElementById('viewer-container').style.display = 'flex';
    

        viewerCtx.setTransform(1, 0, 0, 1, 0, 0);
    if (gameMode === '1P') {
        viewerCanvas.width = PLAYER_CANVAS_WIDTH * RESOLUTION_SCALE;
        viewerCanvas.height = CANVAS_HEIGHT * RESOLUTION_SCALE;
      
  
        
    } else {
        const totalWidth = PLAYER_CANVAS_WIDTH * 2;
        viewerCanvas.width = totalWidth * RESOLUTION_SCALE; 
        viewerCanvas.height = CANVAS_HEIGHT * RESOLUTION_SCALE;
        
        
}
    viewerCtx.scale(RESOLUTION_SCALE, RESOLUTION_SCALE);

    updatePageControls();
    updateCaseControls();
    drawViewer();
    
    // Init History
    historyStack = [captureHistoryState()];
    historyIndex = 0;
    updateUndoRedoButtons();

    document.getElementById('mode-1p').addEventListener('click', () => {
        pushHistory();
        gameMode = '1P';
        document.getElementById('mode-1p').classList.add('active');
        document.getElementById('mode-2p').classList.remove('active');
        document.getElementById('p2-editor-col').style.display = 'none';
        updateCaseControls();
        if (currentDisplayMode === 'editor') renderEditorPage();
        setTimeout(updateScale, 0);
    });
        document.getElementById('mode-2p').addEventListener('click', () => {
        pushHistory();
        gameMode = '2P';
        document.getElementById('mode-2p').classList.add('active');
        document.getElementById('mode-1p').classList.remove('active');
        document.getElementById('p2-editor-col').style.display = 'flex';
        updateCaseControls();
        drawEditorField('p2');
        updateNextQueueDisplay('p2');
        
        setTimeout(updateScale, 0);
    });

    
    // Hidden Gen Code Logic
    let modeClickCount = 0;
    document.getElementById('mode-header').addEventListener('click', () => {
        modeClickCount++;
        if (modeClickCount >= 10) {
            document.getElementById('gen-code').style.display = 'inline-block';
            alert('Gen Code button enabled.');
            modeClickCount = 0;
        }
    });

    document.getElementById('undo-btn').addEventListener('click', undo);
    document.getElementById('redo-btn').addEventListener('click', redo);

    document.getElementById('prev-page').addEventListener('click', () => {
        if (currentPageIndex > 0) {
            loadPage(currentPageIndex - 1);
        }
    });
    document.getElementById('next-page').addEventListener('click', () => {
        if (currentPageIndex < fumenPages.length - 1) {
            loadPage(currentPageIndex + 1);
        } else {
            pushHistory();
            const currentPage = fumenPages[currentPageIndex];
            const newPage = typeof createPageAfterOperation === 'function' && currentCaseIsReplay()
                ? createPageAfterOperation(currentPage)
                : JSON.parse(JSON.stringify(currentPage));
            fumenPages.push(newPage);
            invalidateReplayCase();
            saveCurrentCase();
            loadPage(currentPageIndex + 1);
             updatePageControls();
        }
    });
    document.getElementById('new-page').addEventListener('click', () => {
        pushHistory();
        fumenPages.splice(currentPageIndex + 1, 0, createBlankPage());
        invalidateReplayCase();
        loadPage(currentPageIndex + 1);
    });
    document.getElementById('delete-page').addEventListener('click', () => {
        if (fumenPages.length > 1) {
            pushHistory();
            fumenPages.splice(currentPageIndex, 1);
            invalidateReplayCase();
            if (currentPageIndex >= fumenPages.length) {
                currentPageIndex = fumenPages.length - 1;
            }
            loadPage(currentPageIndex);
         }
    });
    
    // Edit Menu Logic
    const editMenuModal = document.getElementById('edit-menu-modal');
    document.getElementById('open-edit-menu-btn').addEventListener('click', () => {
        editMenuModal.style.display = 'flex';
    });
    document.getElementById('edit-menu-close').addEventListener('click', () => {
        editMenuModal.style.display = 'none';
    });
    
    let pageClipboard = null;
    document.getElementById('copy-page-btn').addEventListener('click', () => {
        pageClipboard = JSON.parse(JSON.stringify(fumenPages[currentPageIndex]));
        alert('現在のページをコピーしました');
        editMenuModal.style.display = 'none';
    });
    
    document.getElementById('paste-page-btn').addEventListener('click', async () => {
        editMenuModal.style.display = 'none';
        // Try clipboard text first for Fumen links
        try {
            const text = await navigator.clipboard.readText();
            if (text && text.includes('v115@')) {
                const match = text.match(/v115@[\w+/?]*/);
                if (match) {
                    const pages = FumenCodec.decode(match[0]);
                    if (pages && pages.length > 0) {
                        pushHistory();
                        pages.forEach(p => {
                            const newPage = createBlankPage();
                            newPage.p1 = { ...newPage.p1, board: p.board, hold: p.hold, next: p.next };
                            fumenPages.splice(currentPageIndex + 1, 0, newPage);
                            invalidateReplayCase();
                            currentPageIndex++;
                        });
                        loadPage(currentPageIndex);
                        alert('テト譜リンクからページを挿入しました');
                        return;
                    }
                }
            }
        } catch(e) {}

        if (pageClipboard) {
            pushHistory();
            const newPage = JSON.parse(JSON.stringify(pageClipboard));
            fumenPages.splice(currentPageIndex + 1, 0, newPage);
            invalidateReplayCase();
            loadPage(currentPageIndex + 1);
            alert('コピーしたページを挿入しました');
        } else {
            alert('クリップボードにページがなく、有効なテト譜リンクも検出されませんでした');
        }
    });

    document.getElementById('swap-p1p2-curr-btn').addEventListener('click', () => {
        pushHistory();
        const p = fumenPages[currentPageIndex];
        [p.p1, p.p2] = [p.p2, p.p1];
        invalidateReplayCase();
        markHistoryPageChanged(p);
        loadPage(currentPageIndex);
        editMenuModal.style.display = 'none';
    });
    
document.getElementById('swap-p1p2-all-btn').addEventListener('click', () => {
        pushHistory();
        fumenPages.forEach(p => {
             [p.p1, p.p2] = [p.p2, p.p1];
        });
        invalidateReplayCase();
        markAllHistoryPagesChanged();
        loadPage(currentPageIndex);
        editMenuModal.style.display = 'none';
    });
// Loaders
    document.getElementById('load-img-p1-btn').addEventListener('click', () => {
        const loader = document.getElementById('hidden-img-loader');
        loader.dataset.target = 'p1';
        loader.click();
    });
    document.getElementById('load-img-p2-btn').addEventListener('click', () => {
        const loader = document.getElementById('hidden-img-loader');
        loader.dataset.target = 'p2';
        loader.click();
    });
    document.getElementById('load-ppt-btn').addEventListener('click', () => document.getElementById('hidden-ppt-loader').click());
document.getElementById('hidden-img-loader').addEventListener('change', (e) => {
        if (e.target.files && e.target.files[0]) {
             startScanProcess(e.target.files[0], e.target.dataset.target || 'p1');
             editMenuModal.style.display = 'none';
        }
        e.target.value = '';
    });
document.getElementById('hidden-ppt-loader').addEventListener('change', (e) => {
        if (e.target.files && e.target.files[0]) {
             processPptImage(e.target.files[0]);

             editMenuModal.style.display = 'none';
        }
        e.target.value = '';
    });


    document.getElementById('view-mode-btn').addEventListener('click', () => {
        currentDisplayMode = 'viewer';
        document.getElementById('editor-container').style.display = 'none';
        document.getElementById('viewer-container').style.display = 'flex';

                viewerCtx.setTransform(1, 0, 0, 1, 0, 0);
        if (gameMode === '1P') {
            viewerCanvas.width = PLAYER_CANVAS_WIDTH * RESOLUTION_SCALE;
            viewerCanvas.height = CANVAS_HEIGHT * RESOLUTION_SCALE;
          
  
            
        } else {
            const totalWidth = PLAYER_CANVAS_WIDTH * 2;
            viewerCanvas.width = totalWidth * RESOLUTION_SCALE; 
            viewerCanvas.height = CANVAS_HEIGHT * RESOLUTION_SCALE;
        }
        viewerCtx.scale(RESOLUTION_SCALE, RESOLUTION_SCALE);

        updatePageControls();
        drawViewer();
        setTimeout(updateScale, 0);
    });

    document.getElementById('back-to-editor-btn').addEventListener('click', () => {

        currentDisplayMode = 'editor';
        document.getElementById('editor-container').style.display = 'flex';
        document.getElementById('viewer-container').style.display = 'none';
        renderEditorPage();
        updateScale();
    });

    if (requestedView === 'editor') {
        document.getElementById('back-to-editor-btn').click();
    }
    
    document.getElementById('viewer-container').addEventListener('click', (e) => {
        if (e.target.closest('#viewer-controls')) {
            return;
        }

        const clickX = e.clientX;
        const screenWidth = window.innerWidth;
        
        if (clickX < screenWidth / 2) {
            
            if (currentPageIndex > 0) {
                loadPage(currentPageIndex - 1);
            }
        } else {
            
            if (currentPageIndex < fumenPages.length - 1) {
                loadPage(currentPageIndex + 1);
            }
        }
    });

    document.getElementById('viewer-simulator-btn').addEventListener('click', sendToSimulator);

});
