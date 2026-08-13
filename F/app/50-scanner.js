const ONNX_CLASS_NAMES = ['null', 'G', 'S', 'Z', 'L', 'J', 'O', 'I', 'T'];
// シミュレータと同じパレット定義
const SCAN_COLOR_PALETTE = {
    'NULL': ['#000000', '#302838'],
    'G':    ['#999999', '#D8D8D8'],
    'I':    ['#019899', '#0199D5'],
    'O':    ['#999A02', '#F9B900'],
    'T':    ['#980099', '#871E88'],
    'L':    ['#996700', '#F56100'],
    'J':    ['#0000BB', '#004BA5'],
    'S':    ['#10971F', '#5CB523'],
    'Z':    ['#990000', '#DA1822']
};
const hexToRgb = (hex) => {
    const r = parseInt(hex.slice(1, 3), 16);
    const g = parseInt(hex.slice(3, 5), 16);
    const b = parseInt(hex.slice(5, 7), 16);
    return { r, g, b };
};
const PARSED_SCAN_COLORS = {};
for (const key in SCAN_COLOR_PALETTE) {
    PARSED_SCAN_COLORS[key] = SCAN_COLOR_PALETTE[key].map(hexToRgb);
}

// Scanner State for manual selection
let scanState = { image: null, bottomLeft: null, topRight: null, targetPlayerId: 'p1', step: 0 };
let isScanning = false;

async function initOnnxModel() {
    try {
        // Warningログを抑制
        ort.env.logLevel = 'fatal';
        onnxSession = await ort.InferenceSession.create('../Load%20PPT/tetris.onnx', { executionProviders: ['wasm'] });
console.log("ONNX Model loaded.");
    } catch (e) {
        console.error("Failed to load ONNX model:", e);
    }
}


async function processPptImage(file) {
    if (!onnxSession) await initOnnxModel();
    if (!onnxSession) { alert("AIモデルの読み込みに失敗しました。"); return; }

    const btn = document.getElementById('load-ppt-btn');
    const originalText = btn.textContent;
    btn.textContent = "処理中...";
    btn.disabled = true;

    const reader = new FileReader();
    reader.onload = async (e) => {
        const img = new Image();
        img.onload = async () => {
            try {
                pushHistory();
                const imgBitmap = await createImageBitmap(img);
                await runHighPrecisionAnalysis(imgBitmap);
                
                // 描画更新を確実にするため requestAnimationFrame でラップ
                requestAnimationFrame(() => {
                    drawEditorField('p1');
                    if(gameMode === '2P') drawEditorField('p2');
                    updateNextQueueDisplay('p1');
                    updateNextQueueDisplay('p2');
                    alert("読み込みが完了しました。");
                });
            } catch (err) {
                console.error(err);
                alert("処理中にエラーが発生しました。");
            } finally {
                btn.textContent = originalText;
                btn.disabled = false;
            }
        };
        img.src = e.target.result;
    };
    reader.readAsDataURL(file);
}


async function runHighPrecisionAnalysis(imgBitmap) {
    try {
        const { canvas: processedCanvas, cropData } = processImageTo1080p(imgBitmap);
        const p1Config = { boardRect: { x: 304, y: 157, w: 670 - 304, h: 882 - 157 }, nextCoords: [ {x:160, y:155}, {x:500, y:122}, {x:500, y:175}, {x:500, y:225}, {x:500, y:275}, {x:500, y:325} ] };
        const p2Config = { boardRect: { x: 1257, y: 157, w: 1620 - 1257, h: 882 - 157 }, nextCoords: [ {x:790, y:155}, {x:1130, y:122}, {x:1130, y:175}, {x:1130, y:225}, {x:1130, y:275}, {x:1130, y:325} ] };

        const p1Data = await analyzePlayerHighPrecision(processedCanvas, imgBitmap, cropData, p1Config);
        fumenPages[currentPageIndex].p1.board = p1Data.board;
        if (typeof currentCaseIsReplay !== 'function' || !currentCaseIsReplay()) {
            fumenPages[currentPageIndex].p1.next = p1Data.nextQueue.join('');
            fumenPages[currentPageIndex].p1.hold = p1Data.holdMino || '';
        } else if (currentPageIndex === 0 && !currentCase().initial.p1.sequence) {
            currentCase().initial.p1.sequence = p1Data.nextQueue.join('');
            currentCase().initial.p1.hold = p1Data.holdMino || '';
            normalizeReplayCase(currentCase());
        }
        
        // 常にP2も解析してデータを格納する（1Pモードでも内部データは保持）
        const p2Data = await analyzePlayerHighPrecision(processedCanvas, imgBitmap, cropData, p2Config);
        fumenPages[currentPageIndex].p2.board = p2Data.board;
        if (typeof currentCaseIsReplay !== 'function' || !currentCaseIsReplay()) {
            fumenPages[currentPageIndex].p2.next = p2Data.nextQueue.join('');
            fumenPages[currentPageIndex].p2.hold = p2Data.holdMino || '';
        } else if (currentPageIndex === 0 && !currentCase().initial.p2.sequence) {
            currentCase().initial.p2.sequence = p2Data.nextQueue.join('');
            currentCase().initial.p2.hold = p2Data.holdMino || '';
            normalizeReplayCase(currentCase());
        }

    } catch (err) {
        console.error("Analysis Error:", err);

        alert("解析エラー: " + err.message);
    }
}

function processImageTo1080p(imgBitmap) {
    const srcW = imgBitmap.width, srcH = imgBitmap.height;
    const targetAspect = 16 / 9;
    const srcAspect = srcW / srcH;
    let cropW, cropH, cropX, cropY;
    if (srcAspect > targetAspect) { cropH = srcH; cropW = srcH * targetAspect; cropX = (srcW - cropW) / 2; cropY = 0; }
    else { cropW = srcW; cropH = srcW / targetAspect; cropX = 0; cropY = (srcH - cropH) / 2; }
    const canvas = document.createElement('canvas');
    canvas.width = 1920; canvas.height = 1080;
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    ctx.drawImage(imgBitmap, cropX, cropY, cropW, cropH, 0, 0, 1920, 1080);
    return { canvas, cropData: { cropX, cropY, cropW, cropH } };
}

async function analyzePlayerHighPrecision(canvas, originalBitmap, cropData, config) {
    const ctx = canvas.getContext('2d');
    const boardRect = config.boardRect;
    const boardImgData = ctx.getImageData(boardRect.x, boardRect.y, boardRect.w, boardRect.h);
    const cellW = boardRect.w / 10;
    const cellH = boardRect.h / 20;
    const recognizedBoard = []; 

    // (A) Classic Scan (Correction)
    const classicBoard = [];
    for (let r = 0; r < 20; r++) {
        const row = [];
        for (let c = 0; c < 10; c++) {
            const sampleX = boardRect.x + (c + 0.5) * cellW;
            const sampleY = boardRect.y + (r + 0.5) * cellH;
            const sampleSize = Math.max(1, Math.floor(cellW * 0.25));
            const imageData = ctx.getImageData(sampleX - sampleSize / 2, sampleY - sampleSize / 2, sampleSize, sampleSize).data;
            let avgR = 0, avgG = 0, avgB = 0;
            for (let i = 0; i < imageData.length; i += 4) {
                avgR += imageData[i];
                avgG += imageData[i+1]; avgB += imageData[i+2];
            }
            const pixelCount = imageData.length / 4;
            avgR /= pixelCount; avgG /= pixelCount; avgB /= pixelCount;
            row.push(findClosestColor(avgR, avgG, avgB));
        }
        classicBoard.push(row);
    }

    // (B) ONNX Inference
    const batchFeatures = [];
    for (let r = 0; r < 20; r++) {
        const row = [];
        for (let c = 0; c < 10; c++) {
            const x = c * cellW;
            const y = r * cellH;
            const cellPixels = extractCellPixels(boardImgData, x, y, cellW, cellH);
            const feats = extractFeaturesJS(cellPixels, Math.floor(cellW), Math.floor(cellH));
            batchFeatures.push(feats);
            row.push(null);
        }
        recognizedBoard.push(row);
    }

    const flatInput = new Float32Array(batchFeatures.length * 63);
    for (let i = 0; i < batchFeatures.length; i++) {
        flatInput.set(batchFeatures[i], i * 63);
    }
    
    const tensor = new ort.Tensor('float32', flatInput, [200, 63]);
    const inputName = onnxSession.inputNames[0];
    const feeds = { [inputName]: tensor };
    const labelOutputName = onnxSession.outputNames[0]; 
    const fetches = [labelOutputName];
    const results = await onnxSession.run(feeds, fetches);
    const outputLabel = results[labelOutputName];
    const labelData = outputLabel.data;

    for (let i = 0; i < 200; i++) {
        const label = ONNX_CLASS_NAMES[Number(labelData[i])];
        recognizedBoard[Math.floor(i / 10)][i % 10] = (label === 'null') ? null : label;
    }

    for (let r = 0; r < 20; r++) {
        if (recognizedBoard[r].every(c => c === null || c === 'G')) {
            const gCnt = recognizedBoard[r].filter(c => c === 'G').length;
            if (gCnt !== 10) {
                const classicRow = classicBoard[r];
                if (classicRow.filter(c => c === 'G').length === 9 && classicRow.filter(c => c === null).length === 1) {
                    recognizedBoard[r] = [...classicRow];
                }
            }
        }
    }

    const fullBoard = Array.from({ length: 40 }, () => Array(10).fill(null));
    for(let r=0; r<20; r++) fullBoard[20+r] = recognizedBoard[r];
    
    // Garbage cleanup
    let firstNonG = -1;
    for (let y = 39; y >= 0; y--) { if (!fullBoard[y].includes('G')) { firstNonG = y; break; } }
    if (firstNonG !== -1) { for (let y = firstNonG - 1; y >= 0; y--) { for (let x = 0; x < 10; x++) { if (fullBoard[y][x] === 'G') fullBoard[y][x] = null; } } }

    // Next/Hold
    const nextQueue = [];
    let holdMino = null;
    const rawCropCanvas = document.createElement('canvas');
    rawCropCanvas.width = cropData.cropW; rawCropCanvas.height = cropData.cropH;
    const rawCtx = rawCropCanvas.getContext('2d', { willReadFrequently: true });
    rawCtx.drawImage(originalBitmap, cropData.cropX, cropData.cropY, cropData.cropW, cropData.cropH, 0, 0, cropData.cropW, cropData.cropH);
    const scale = cropData.cropW / 1280;
    const radius = 5 * scale;
    for (let i = 0; i < config.nextCoords.length; i++) {
        const c = config.nextCoords[i];
        const avg = getAverageColorNonBlack(rawCtx, c.x * scale, c.y * scale, radius);
        if (i === 0) {
            if (!(avg.r < 50 && avg.g < 50 && avg.b < 50)) holdMino = findClosestMinoOnly(avg.r, avg.g, avg.b);
        } else {
            if (i === 1 && (avg.r < 50 && avg.g < 50 && avg.b < 50)) break;
            const fm = findClosestMinoOnly(avg.r, avg.g, avg.b);
            if (fm) nextQueue.push(fm);
        }
    }
    return { board: fullBoard, holdMino, nextQueue };
}

function extractCellPixels(imgD, x, y, w, h) {
    const sw = imgD.width, ix = Math.floor(x), iy = Math.floor(y), iw = Math.floor(w), ih = Math.floor(h);
    const d = new Uint8ClampedArray(iw * ih * 4);
    for (let r = 0; r < ih; r++) {
        const s = ((iy + r) * sw + ix) * 4;
        d.set(imgD.data.subarray(s, s + iw * 4), r * iw * 4);
    }
    return d;
}
function extractFeaturesJS(pixelsRGBA, w, h) {
    const numPixels = w * h;
    const feats = [];
    const bCh = new Float32Array(numPixels);
    const gCh = new Float32Array(numPixels);
    const rCh = new Float32Array(numPixels);
    const hCh = new Float32Array(numPixels);
    const sCh = new Float32Array(numPixels);
    const vCh = new Float32Array(numPixels);
    for (let i = 0; i < numPixels; i++) {
        const r = pixelsRGBA[i * 4];
        const g = pixelsRGBA[i * 4 + 1];
        const b = pixelsRGBA[i * 4 + 2];
        bCh[i] = b;
        gCh[i] = g; rCh[i] = r;
        
        const maxVal = Math.max(r, g, b);
        const minVal = Math.min(r, g, b);
        const diff = maxVal - minVal;
        const v = maxVal;
        let s = (maxVal !== 0) ?
        (diff / maxVal) * 255 : 0;
        let h_val = 0;
        if (maxVal === minVal) h_val = 0;
        else if (maxVal === r) h_val = (60 * (g - b) / diff + 360) % 360;
        else if (maxVal === g) h_val = (60 * (b - r) / diff + 120) % 360;
        else if (maxVal === b) h_val = (60 * (r - g) / diff + 240) % 360;
        h_val = h_val / 2;
        vCh[i] = v; sCh[i] = s; hCh[i] = h_val;
    }

    const getStats = (arr) => {
        let sum = 0;
        for(let v of arr) sum += v;
        const mean = sum / arr.length;
        let sqDiffSum = 0;
        for(let v of arr) sqDiffSum += (v - mean) ** 2;
        const std = Math.sqrt(sqDiffSum / arr.length);
        return [mean, std];
    };

    const statsB = getStats(bCh); const statsG = getStats(gCh); const statsR = getStats(rCh);
    feats.push(statsB[0], statsB[1], statsG[0], statsG[1], statsR[0], statsR[1]);
    const statsH = getStats(hCh); const statsS = getStats(sCh); const statsV = getStats(vCh);
    feats.push(statsH[0], statsH[1], statsS[0], statsS[1], statsV[0], statsV[1]);
    const tinyFeats = [];
    const stepX = w / 4;
    const stepY = h / 4;
    for (let ty = 0; ty < 4; ty++) {
        for (let tx = 0; tx < 4; tx++) {
            const sx = Math.floor(tx * stepX), sy = Math.floor(ty * stepY);
            const ex = Math.floor((tx + 1) * stepX), ey = Math.floor((ty + 1) * stepY);
            let sumB=0, sumG=0, sumR=0, count=0;
            for(let py=sy; py<ey; py++){
                for(let px=sx; px<ex; px++){
                    const idx = py * w + px;
                    if(idx < numPixels) { sumB += bCh[idx]; sumG += gCh[idx]; sumR += rCh[idx]; count++;
                    }
                }
            }
            if(count===0) { tinyFeats.push(0,0,0);
            } else { tinyFeats.push(sumB/count, sumG/count, sumR/count); }
        }
    }
    feats.push(...tinyFeats);
    const cx = Math.floor(w/2), cy = Math.floor(h/2);
    const cw = Math.floor(w/4), ch = Math.floor(h/4);
    const startX = cx - cw, endX = cx + cw;
    const startY = cy - ch, endY = cy + ch;
    let cSumB=0, cSumG=0, cSumR=0, cCount=0;
    for(let py=startY; py<endY; py++){
        for(let px=startX; px<endX; px++){
            if(px>=0 && px<w && py>=0 && py<h){
                const idx = py * w + px;
                cSumB += bCh[idx]; cSumG += gCh[idx]; cSumR += rCh[idx]; cCount++;
            }
        }
    }
    if(cCount===0) feats.push(0,0,0);
    else feats.push(cSumB/cCount, cSumG/cCount, cSumR/cCount);

    return new Float32Array(feats);
}

function findClosestColor(r, g, b) {
    const inputColor = { r, g, b };
    const colorDistanceSq = (c1, c2) => {
        return Math.pow(c1.r - c2.r, 2) + Math.pow(c1.g - c2.g, 2) + Math.pow(c1.b - c2.b, 2);
    };
    for (const nullColor of PARSED_SCAN_COLORS.NULL) {
        if (colorDistanceSq(inputColor, nullColor) < 6000) {
            return null;
        }
    }
    for (const gColor of PARSED_SCAN_COLORS.G) {
        if (colorDistanceSq(inputColor, gColor) < 10000) {
            return 'G';
        }
    }
    let minDistance = Infinity;
    let closestKey = null;
    const minoKeys = Object.keys(PARSED_SCAN_COLORS).filter(k => k !== 'NULL' && k !== 'G');
    for (const key of minoKeys) {
        for (const targetColor of PARSED_SCAN_COLORS[key]) {
            const distance = colorDistanceSq(inputColor, targetColor);
            if (distance < minDistance) {
                minDistance = distance;
                closestKey = key;
            }
        }
    }
    return (minDistance > 25000) ? null : closestKey;
}

function getAverageColorNonBlack(ctx, cx, cy, rad) {
    const d = Math.ceil(rad*2), sx=Math.floor(cx-rad), sy=Math.floor(cy-rad);
    const id = ctx.getImageData(sx, sy, d, d).data;
    let tr=0, tg=0, tb=0, c=0;
    for(let y=0; y<d; y++) for(let x=0; x<d; x++) {
        if((x-rad)**2+(y-rad)**2 <= rad*rad) {
            const i=(y*d+x)*4; if(id[i]>50||id[i+1]>50||id[i+2]>50) { tr+=id[i]; tg+=id[i+1]; tb+=id[i+2]; c++; }
        }
    }
    return c ? {r:tr/c, g:tg/c, b:tb/c} : {r:0,g:0,b:0};
}
function findClosestMinoOnly(r,g,b) {
    const dist = (c1, c2) => (c1.r-c2.r)**2 + (c1.g-c2.g)**2 + (c1.b-c2.b)**2;
    let minD = Infinity, best = 'I';
    for(const key of Object.keys(PARSED_SCAN_COLORS)) {
        if(key==='NULL'||key==='G') continue;
        for(const tc of PARSED_SCAN_COLORS[key]) {
            const d = dist({r,g,b}, tc);
            if(d < minD) { minD = d; best = key; }
        }
    }
    return best;
}

// --- Manual Scan Logic (Ported from Simulator) ---
function startScanProcess(file, playerId = 'p1') {
    scanState.targetPlayerId = playerId;
    const reader = new FileReader();
reader.onload = e => {
        scanState.image = new Image();
scanState.image.onload = () => {

            // スキャナーUIを表示
            document.getElementById('editor-container').style.display = 'none';
            document.getElementById('scanner-container').style.display = 'flex';
            
            isScanning = true;
            scanState.bottomLeft = null;
            scanState.topRight = null;
            scanState.step = 0; // 0: Wait BL, 1: Confirm BL, 2: Wait TR, 3: Confirm TR
            updateScanUI();
            
            const canvas = document.getElementById('scanner-canvas');

            const ar = scanState.image.naturalWidth / scanState.image.naturalHeight;
            
            // 画面に合わせて表示サイズ調整
            const maxWidth = window.innerWidth * 0.9;
            const maxHeight = window.innerHeight * 0.8;
            let dW = maxWidth, dH = maxWidth / ar;
            if (dH > maxHeight) { dH = maxHeight; dW = dH * ar; }
            
            // Canvasサイズは実際の画像サイズに合わせる（高解像度維持）か、
            // Simに合わせて「表示サイズ」と「内部サイズ」を使い分けるが、
            // Simは width=2048上限で描画している。
            const canvasWidth = Math.min(2048, scanState.image.naturalWidth);
            canvas.width = canvasWidth;
            canvas.height = canvasWidth / ar;
            
            canvas.style.width = `${dW}px`;
            canvas.style.height = `${dH}px`;
            
            drawScanner();
        };
        scanState.image.src = e.target.result;
    };
    reader.readAsDataURL(file);
}

function updateScanUI() {
    const instr = document.getElementById('scan-instructions');
    const nextBtn = document.getElementById('scan-next-step-btn');
    const confirmBtn = document.getElementById('scan-confirm-btn');
    
    // Reset buttons
    nextBtn.style.display = 'none';
    confirmBtn.style.display = 'none';

    if (scanState.step === 0) {
        instr.textContent = '盤面の左下をクリックしてください';
    } else if (scanState.step === 1) {
        instr.textContent = 'よろしければ「次へ」を押してください';
        nextBtn.style.display = 'inline-block';
    } else if (scanState.step === 2) {
        instr.textContent = '盤面の右上をクリックしてください';
    } else if (scanState.step === 3) {
        instr.textContent = '範囲が正しければ「読込開始」を押してください';
        confirmBtn.style.display = 'inline-block';
    }
}

function drawScanner() {
    if (!scanState.image) return;

    const canvas = document.getElementById('scanner-canvas');
    const ctx = canvas.getContext('2d');
    
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(scanState.image, 0, 0, canvas.width, canvas.height);
    
    if (scanState.bottomLeft) {
        ctx.fillStyle = 'lime';
        ctx.beginPath();
        ctx.arc(scanState.bottomLeft.x, scanState.bottomLeft.y, 10, 0, Math.PI * 2);
        ctx.fill();
    }
    if (scanState.topRight) {
        const {x: blx, y: bly} = scanState.bottomLeft;
        const {x: trx, y: try_} = scanState.topRight;
        
        ctx.fillStyle = 'fuchsia';
        ctx.beginPath();
        ctx.arc(scanState.topRight.x, scanState.topRight.y, 10, 0, Math.PI * 2);
        ctx.fill();
        
        ctx.strokeStyle = 'fuchsia';
        ctx.lineWidth = 4;
        ctx.strokeRect(blx, try_, trx - blx, bly - try_);
    }
}

function endScanProcess() {
    isScanning = false;
    document.getElementById('scanner-container').style.display = 'none';
    document.getElementById('editor-container').style.display = 'flex';
    updateScale();
}

function processAndLoadBoard() {
    if (!scanState.image || !scanState.bottomLeft || !scanState.topRight) return;
    
    const canvas = document.getElementById('scanner-canvas');
    
    // 元画像からピクセルデータを取得するためのCanvas
    const tempC = document.createElement('canvas');
    tempC.width = scanState.image.naturalWidth;
    tempC.height = scanState.image.naturalHeight;
    const tempCtx = tempC.getContext('2d', { willReadFrequently: true });
    tempCtx.drawImage(scanState.image, 0, 0);
    
    // 表示用Canvasと元画像のスケール比
    const sX = scanState.image.naturalWidth / canvas.width;
    const sY = scanState.image.naturalHeight / canvas.height;
    
    const iBL = { x: scanState.bottomLeft.x * sX, y: scanState.bottomLeft.y * sY };
    const iTR = { x: scanState.topRight.x * sX, y: scanState.topRight.y * sY };
    
    const bW_px = iTR.x - iBL.x;
    const bH_px = iBL.y - iTR.y; // Yは上が0なので BL.y > TR.y
    const blW_px = bW_px / 10; // BOARD_WIDTH
    const blH_px = bH_px / 20; // BOARD_VISIBLE_HEIGHT
    // ターゲットのプレイヤー
    const targetId = scanState.targetPlayerId || 'p1';
    const targetBoard = fumenPages[currentPageIndex][targetId].board;
// 盤面クリア
    targetBoard.forEach(row => row.fill(null));
    
    for (let r = 0; r < 20; r++) {

        for (let c = 0; c < 10; c++) {
            // サンプリング座標
            const cX = iBL.x + (c + 0.5) * blW_px;
            const cY = iTR.y + (r + 0.5) * blH_px;
            
            const sampleSize = Math.max(1, Math.floor(blW_px * 0.25));
            const iD = tempCtx.getImageData(cX - sampleSize/2, cY - sampleSize/2, sampleSize, sampleSize).data;
            
            let avgR=0, avgG=0, avgB=0;
            for(let i=0; i<iD.length; i+=4){ avgR+=iD[i]; avgG+=iD[i+1]; avgB+=iD[i+2]; }
            const pCount = iD.length / 4;
            avgR /= pCount; avgG /= pCount; avgB /= pCount;
            
            // 下から埋めていく (r=0 is top visual row)
            // targetBoardは [0..39]。表示領域は [20..39]
            targetBoard[20 + r][c] = findClosestColor(avgR, avgG, avgB);
        }
    }
    
    drawEditorField(targetId);
    pushHistory(); // 変更後に保存
    endScanProcess();
}

// Event Listeners for Scanner
document.getElementById('scanner-canvas').addEventListener('click', e => {

    if (!isScanning) return;
    const canvas = document.getElementById('scanner-canvas');
    const rect = canvas.getBoundingClientRect();
    const scaleX = canvas.width / rect.width;
    const scaleY = canvas.height / rect.height;
    
    const pos = { 
        x: (e.clientX - rect.left) * scaleX, 
        y: (e.clientY - rect.top) * scaleY 
    };
    
    if (scanState.step === 0 || scanState.step === 1) {
        scanState.bottomLeft = pos;
        scanState.step = 1;
    } else if (scanState.step === 2 || scanState.step === 3) {
        scanState.topRight = pos;
        scanState.step = 3;
    }
    updateScanUI();
    drawScanner();
});

document.getElementById('scan-next-step-btn').addEventListener('click', () => {
    if (scanState.step === 1) {
        scanState.step = 2;
        updateScanUI();
    }
});

document.getElementById('scan-cancel-btn').addEventListener('click', endScanProcess);
document.getElementById('scan-confirm-btn').addEventListener('click', processAndLoadBoard);
