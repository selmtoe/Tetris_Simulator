document.addEventListener('DOMContentLoaded', () => {
    // --- Export Logic ---
    const exportModal = document.getElementById('export-modal');
    const exportFormatRadios = document.getElementsByName('export-format');
    const exportRangeRadios = document.getElementsByName('export-range');
    
    // UI制御
    document.getElementById('viewer-export-btn').addEventListener('click', () => {
        exportModal.style.display = 'flex';
    });
    document.getElementById('export-close-btn').addEventListener('click', () => {
        exportModal.style.display = 'none';
    });
    
    // ラジオボタンの変更検知
    exportFormatRadios.forEach(r => r.addEventListener('change', () => {
        document.getElementById('export-gif-settings').style.display = (r.value === 'gif') ? 'block' : 'none';
    }));
    exportRangeRadios.forEach(r => r.addEventListener('change', () => {
        document.getElementById('export-field-settings').style.display = (r.value === 'field') ? 'block' : 'none';
    }));

    // 指定された設定でCanvasを描画して返す関数
    function renderExportCanvas(pageIdx, range, heightBlocks) {
        // 現在の状態を保存
        const savedCtx = viewerCtx;
        const savedCanvas = viewerCanvas;
        const savedMode = gameMode;
        
        // 一時的なCanvasを作成
        const tempCanvas = document.createElement('canvas');
        const ctx = tempCanvas.getContext('2d');
        
        const scale = RESOLUTION_SCALE; // 高画質維持
        
        if (range === 'field') {
            // 盤面のみ (1Pのみ対応、2Pの場合はP1だけ出力するか、並べるか要検討だが一旦P1前提)
            // 幅: 10ブロック, 高さ: ユーザー指定
            const w = 10 * BLOCK_SIZE;
            const h = heightBlocks * BLOCK_SIZE;
            
            tempCanvas.width = w * scale;
            tempCanvas.height = h * scale;
            ctx.scale(scale, scale);
            
            // 背景塗りつぶし (黒っぽい色)
            ctx.fillStyle = '#0f0f18'; 
            ctx.fillRect(0, 0, w, h);
            
            // 描画位置の調整
            // drawViewerは通常位置に描画するので、Contextをずらして
            // 盤面の欲しい部分が(0,0)に来るようにする
            // 通常の盤面左上: PLAYFIELD_X_OFFSET
            // 通常の盤面下端: (BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT) * BLOCK_SIZE + 20 * BLOCK_SIZE (20はVisible)
            // ちょっと計算が複雑なので、fumenPagesのデータを使って自前で描画したほうが早いが、
            // drawViewerのロジックを再利用するためにTranslate技を使う
            
            // 譜面データ上のY座標で言うと、底辺は y=0。
            // 欲しいのは y=0 から y=heightBlocks-1 まで。
            // 画面上のY座標は、上の方が値が小さい。
            // drawViewerでは、BOARD_VISIBLE_HEIGHT(20) 分を描画している。
            
            // シンプルに、「盤面部分」だけを切り抜くアプローチをとる
            // まず全体を描画させる
            
            // 1. ページをセット
            currentPageIndex = pageIdx;
            
            // 2. 仮想Canvasに全体を描画 (背景不透明)
            const fullW = (gameMode === '1P' ? PLAYER_CANVAS_WIDTH : PLAYER_CANVAS_WIDTH * 2);
            const fullH = CANVAS_HEIGHT;
            const fullCanvas = document.createElement('canvas');
            fullCanvas.width = fullW * scale;
            fullCanvas.height = fullH * scale;
            const fullCtx = fullCanvas.getContext('2d');
            fullCtx.scale(scale, scale);
            
            // 背景 (#1a1a2e)
            fullCtx.fillStyle = '#1a1a2e';
            fullCtx.fillRect(0, 0, fullW, fullH);
            
            // DrawViewerを乗っ取る
            viewerCanvas = fullCanvas;
            viewerCtx = fullCtx;
            drawViewer();
            
            // 3. 必要な部分を切り出してtempCanvasに描画
            // P1の盤面位置: x = PLAYFIELD_X_OFFSET, y = 0.5 * BLOCK_SIZE (枠線考慮)
            // ただし、drawViewerは BOARD_VISIBLE_HEIGHT(20) 固定で描画している。
            // ユーザー指定の高さが20を超える場合、drawViewerが描画していない部分は白紙になる。
            // これを解決するには drawViewer の BOARD_VISIBLE_HEIGHT 依存を修正する必要があるが、
            // 今回は「デフォルト20」で、既存表示領域(20)の切り抜きを行う実装とする。
            // ※もし20以上描画したい場合は drawViewer 自体の改修が必要。
            
            // 切り抜くソース座標 (論理座標)
            // 左: PLAYFIELD_X_OFFSET
            // 下: (BOARD_VISIBLE_HEIGHT + 0.5) * BLOCK_SIZE - (marginなど)
            // 実際にはボードは (PLAYFIELD_X_OFFSET, 0.5*BLOCK_SIZE) から
            // 幅 PLAYFIELD_WIDTH, 高さ BOARD_VISIBLE_HEIGHT*BLOCK_SIZE
            
            // 高さ制限: 最大でも20 (今の仕様)
            const visibleH = Math.min(heightBlocks, BOARD_VISIBLE_HEIGHT);
            
            const srcX = PLAYFIELD_X_OFFSET * scale;
            // 下からvisibleH分取得したい。
            // 盤面上端Y: 0.5 * BLOCK_SIZE
            // 盤面下端Y: (0.5 + BOARD_VISIBLE_HEIGHT) * BLOCK_SIZE
            // 切り出し開始Y: 盤面下端 - (visibleH * BLOCK_SIZE)
            const boardBottomY = (0.5 + BOARD_VISIBLE_HEIGHT) * BLOCK_SIZE;
            const srcY = (boardBottomY - (visibleH * BLOCK_SIZE)) * scale;
            const srcW = 10 * BLOCK_SIZE * scale;
            const srcH = visibleH * BLOCK_SIZE * scale;
                        // 出力先
            // tempCanvasのサイズは heightBlocksに合わせてあるが、
            // ソースが足りない場合は下詰めにする
            const dstY = (heightBlocks - visibleH) * BLOCK_SIZE * scale;
            ctx.setTransform(1, 0, 0, 1, 0, 0); // スケールリセットしてピクセル転送
            ctx.drawImage(fullCanvas, srcX, srcY, srcW, srcH, 0, dstY, srcW, srcH);
            
            // 背景色を合成 (destination-over: 既存の絵の後ろに描画)
            ctx.globalCompositeOperation = 'destination-over';
            ctx.fillStyle = '#0f0f18'; // 盤面のみの場合は黒っぽい色
            ctx.fillRect(0, 0, tempCanvas.width, tempCanvas.height);
            ctx.globalCompositeOperation = 'source-over'; // 戻す

            // 復元
            viewerCanvas = savedCanvas;
            viewerCtx = savedCtx;
        } else {
            // 全体 (背景不透明)
            const w = (gameMode === '1P' ? PLAYER_CANVAS_WIDTH : PLAYER_CANVAS_WIDTH * 2);
            const h = CANVAS_HEIGHT;
            
            tempCanvas.width = w * scale;
            tempCanvas.height = h * scale;
            ctx.scale(scale, scale);
            // ※ drawViewer内でclearRectされるため、事前のfillRectは意味がない
            // DrawViewerを一時的に乗っ取る
            viewerCanvas = tempCanvas;
            viewerCtx = ctx;
            
            currentPageIndex = pageIdx;
            drawViewer();

            // 背景色を合成 (destination-over: 既存の絵の後ろに描画)
            ctx.globalCompositeOperation = 'destination-over';
            ctx.fillStyle = '#1a1a2e'; // エディタ背景色
            ctx.fillRect(0, 0, w, h);
            ctx.globalCompositeOperation = 'source-over'; // 戻す
            
            // 復元
            viewerCanvas = savedCanvas;
            viewerCtx = savedCtx;
        }
        
        return tempCanvas;

    }

    document.getElementById('do-export-btn').addEventListener('click', async () => {
        const btn = document.getElementById('do-export-btn');
        const originalText = btn.textContent;
        btn.textContent = '処理中...';
        btn.disabled = true;

        try {
            const format = Array.from(exportFormatRadios).find(r => r.checked).value;
            const range = Array.from(exportRangeRadios).find(r => r.checked).value;
            const height = parseInt(document.getElementById('export-height').value, 10) || 20;
            const delay = parseInt(document.getElementById('export-delay').value, 10) || 500;

            if (format === 'png') {
                const canvas = renderExportCanvas(currentPageIndex, range, height);
                const link = document.createElement('a');
                link.download = `tetofu_export_${currentPageIndex + 1}.png`;
                link.href = canvas.toDataURL();
                link.click();
            } else {
                // GIF
                const response = await fetch('https://cdnjs.cloudflare.com/ajax/libs/gif.js/0.2.0/gif.worker.js');
                if (!response.ok) throw new Error('Worker fetch failed');
                const workerBlob = await response.blob();
                const workerUrl = URL.createObjectURL(workerBlob);

                const gif = new GIF({
                    workers: 2,
                    quality: 10,
                    workerScript: workerUrl,
                    background: (range === 'field') ? '#0f0f18' : '#1a1a2e' // GIFの背景色設定
                });

                const originalIndex = currentPageIndex;

                for (let i = 0; i < fumenPages.length; i++) {
                    const canvas = renderExportCanvas(i, range, height);
                    gif.addFrame(canvas, {copy: true, delay: delay});
                }
                
                // ページを戻す
                currentPageIndex = originalIndex;
                drawViewer();

                gif.on('finished', function(blob) {
                    const link = document.createElement('a');
                    link.href = URL.createObjectURL(blob);
                    link.download = 'tetofu_anim.gif';
                    link.click();
                    URL.revokeObjectURL(workerUrl);
                    
                    btn.textContent = originalText;
                    btn.disabled = false;
                    exportModal.style.display = 'none';
                });

                gif.render();
                return; // 終了待ちのためここでリターンしない
            }

        } catch (e) {
            console.error(e);
            alert('出力に失敗しました: ' + e);
        }
        
        btn.textContent = originalText;
        btn.disabled = false;
        if (exportFormatRadios[0].checked) { // PNGの場合は閉じる
             exportModal.style.display = 'none';
        }
    });

    const viewerPageSlider = document.getElementById('viewer-page-slider');
    let pendingSliderIndex = currentPageIndex;
    let pendingSliderFrame = 0;
    const commitSliderPage = () => {
        pendingSliderFrame = 0;
        if (pendingSliderIndex !== currentPageIndex) loadPage(pendingSliderIndex);
    };
    viewerPageSlider.addEventListener('input', event => {
        pendingSliderIndex = parseInt(event.target.value, 10);
        if (!pendingSliderFrame) pendingSliderFrame = requestAnimationFrame(commitSliderPage);
    });
    viewerPageSlider.addEventListener('change', event => {
        pendingSliderIndex = parseInt(event.target.value, 10);
        if (pendingSliderFrame) cancelAnimationFrame(pendingSliderFrame);
        commitSliderPage();
    });

});
