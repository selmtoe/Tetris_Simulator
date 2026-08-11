/* Image/PPT board recognition using the ONNX model. */

function startScanProcess(file, playerId) { const reader = new FileReader(); reader.onload = e => { scanState.image = new Image(); scanState.image.onload = () => { scanState.targetPlayerId = playerId; scanState.bottomLeft = null; scanState.topRight = null; gameState = 'SCAN_BL'; document.getElementById('editor-container').style.display = 'none'; document.getElementById('game-container').style.display = 'block'; document.getElementById('scan-controls').style.display = 'flex'; 
const ar = scanState.image.naturalWidth / scanState.image.naturalHeight; const displayMaxWidth = window.innerWidth * 0.9; const displayMaxHeight = window.innerHeight * 0.8; let displayWidth = displayMaxWidth; let displayHeight = displayWidth / ar; if (displayHeight > displayMaxHeight) { displayHeight = displayMaxHeight; displayWidth = displayHeight * ar; } const canvasWidth = Math.min(2048, scanState.image.naturalWidth); mainCanvas.width = canvasWidth; mainCanvas.height = canvasWidth / ar; mainCanvas.style.width = `${displayWidth}px`; mainCanvas.style.height = `${displayHeight}px`; updateScanUI(); setTimeout(updateScale, 0);}; scanState.image.src = e.target.result; }; reader.readAsDataURL(file); }
function endScanProcess() { gameState = 'EDITING'; scanState.image = null; mainCanvas.style.width = ''; mainCanvas.style.height = ''; document.getElementById('editor-container').style.display = 'flex'; document.getElementById('game-container').style.display = 'none'; document.getElementById('scan-controls').style.display = 'none'; ['p1', 'p2'].forEach(drawEditorField); setTimeout(updateScale, 0); }
function updateScanUI() { const instructions = document.getElementById('scan-instructions'), confirmBtn = document.getElementById('scanConfirmBtn'); mainCanvas.style.cursor = 'crosshair'; if (gameState === 'SCAN_BL') { confirmBtn.style.visibility = scanState.bottomLeft ? 'visible' : 'hidden'; } else if (gameState === 'SCAN_TR') {confirmBtn.style.visibility = scanState.topRight ? 'visible' : 'hidden'; if (scanState.topRight) confirmBtn.textContent = '読込開始'; else confirmBtn.textContent = '次へ'; } }
function drawScanner() { if (!scanState.image) return; ctx.clearRect(0, 0, mainCanvas.width, mainCanvas.height); ctx.drawImage(scanState.image, 0, 0, mainCanvas.width, mainCanvas.height); if (scanState.bottomLeft) { ctx.fillStyle = 'lime'; ctx.beginPath(); ctx.arc(scanState.bottomLeft.x, scanState.bottomLeft.y, 10, 0, Math.PI * 2); ctx.fill(); } if (scanState.topRight) { const {x: blx, y: bly} = scanState.bottomLeft, {x: trx, y: try_} = scanState.topRight; ctx.strokeStyle = 'fuchsia'; ctx.lineWidth = 4; ctx.strokeRect(blx, try_, trx - blx, bly - try_); } }
function processAndLoadBoard() { const tempC = document.createElement('canvas'); tempC.width = scanState.image.naturalWidth; tempC.height = scanState.image.naturalHeight; const tempCtx = tempC.getContext('2d', { willReadFrequently: true }); tempCtx.drawImage(scanState.image, 0, 0); const sX = scanState.image.naturalWidth / mainCanvas.width, sY = scanState.image.naturalHeight / mainCanvas.height; const iBL = { x: scanState.bottomLeft.x*sX, y: scanState.bottomLeft.y*sY }, iTR = { x: scanState.topRight.x*sX, y: scanState.topRight.y*sY }; const bW_px = iTR.x - iBL.x, bH_px = iBL.y - iTR.y, blW_px = bW_px/BOARD_WIDTH, blH_px = bH_px/BOARD_VISIBLE_HEIGHT; const targetBoard = editorData[scanState.targetPlayerId].board; targetBoard.forEach(row => row.fill(null)); for (let r = 0; r < BOARD_VISIBLE_HEIGHT; r++) { for (let c = 0; c < BOARD_WIDTH; c++) { const cX = iBL.x + (c+0.5)*blW_px, cY = iTR.y + (r+0.5)*blH_px; const sampleSize = Math.max(1, Math.floor(blW_px * 0.25)); const iD = tempCtx.getImageData(cX-sampleSize/2, cY-sampleSize/2, sampleSize, sampleSize).data; let avgR=0, avgG=0, avgB=0; for(let i=0; i<iD.length; i+=4){ avgR+=iD[i]; avgG+=iD[i+1]; avgB+=iD[i+2]; } const pCount = iD.length/4; avgR/=pCount; avgG/=pCount; avgB/=pCount; targetBoard[BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT + r][c] = findClosestColor(avgR, avgG, avgB); } } endScanProcess(); }

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

function getAverageColorNonBlack(ctx, cx, cy, radius) {
    const startX = Math.floor(cx - radius);
    const startY = Math.floor(cy - radius);
    const diameter = Math.ceil(radius * 2);
    if (startX < 0 || startY < 0 || startX + diameter > ctx.canvas.width || startY + diameter > ctx.canvas.height) {
        return { r: 0, g: 0, b: 0 };
    }
    const imageData = ctx.getImageData(startX, startY, diameter, diameter).data;
    
    let totalR = 0, totalG = 0, totalB = 0, count = 0;
    const radiusSq = radius * radius;
    const blackThreshold = 50; 

    for (let y = 0; y < diameter; y++) {
        for (let x = 0; x < diameter; x++) {
            const dx = x - radius;
            const dy = y - radius;
            
            if (dx * dx + dy * dy <= radiusSq) {
                const i = (y * diameter + x) * 4;
                const r = imageData[i];
                const g = imageData[i + 1];
                const b = imageData[i + 2];
                
                if (r > blackThreshold || g > blackThreshold || b > blackThreshold) {
                    totalR += r;
                    totalG += g;
                    totalB += b;
                    count++;
                }
            }
        }
    }
    
    if (count === 0) {
        return { r: 0, g: 0, b: 0 };
    }
    
    return {
        r: totalR / count,
        g: totalG / count,
        b: totalB / count
    };
}

function findClosestMinoOnly(r, g, b) {
    const inputColor = { r, g, b };
    const colorDistanceSq = (c1, c2) => {
        return Math.pow(c1.r - c2.r, 2) + Math.pow(c1.g - c2.g, 2) + Math.pow(c1.b - c2.b, 2);
    };

    let minDistance = Infinity;
    let closestKey = 'I'; 
    
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
    return closestKey;
}

// --- ONNX High Precision PPT Load ---
let onnxSession = null;
const ONNX_CLASS_NAMES = ['null', 'G', 'S', 'Z', 'L', 'J', 'O', 'I', 'T'];
let modelReadyResolver;
const modelReadyPromise = new Promise(resolve => modelReadyResolver = resolve);

async function initOnnxModel() {
    try {
        onnxSession = await ort.InferenceSession.create('./Load%20PPT/tetris.onnx', { executionProviders: ['wasm'] });
console.log("ONNX Model loaded.");
        if (modelReadyResolver) modelReadyResolver(true);
    } catch (e) {
        console.error("Failed to load ONNX model:", e);
if (modelReadyResolver) modelReadyResolver(false);
    }
}

async function processPptImage(file) {

    if (!onnxSession) {
        await initOnnxModel();
if (!onnxSession) {
            alert("AIモデルの読み込みに失敗しました。");
            return;
}
    }
    
    document.getElementById('mode-2p').click();

    // High Precision Logic
    const reader = new FileReader();
    reader.onload = async (e) => {
        const img = new Image();
        img.onload = async () => {
            const imgBitmap = await createImageBitmap(img);
            await runHighPrecisionAnalysis(imgBitmap);
        };
        img.src = e.target.result;
    };
    reader.readAsDataURL(file);
}

window.receiveExtensionImage = async (base64Data) => {
    try {
        const res = await fetch(base64Data);
        const blob = await res.blob();
        const file = new File([blob], "capture.jpg", { type: "image/jpeg" });
        processPptImage(file);
    } catch(e) {
        console.error("Extension image process failed", e);
    }
};

async function runHighPrecisionAnalysis(imgBitmap) {
    try {
        // 1. 前処理 (16:9 クロップ & 1920x1080 リサイズ)
        const { canvas: processedCanvas, cropData } = processImageTo1080p(imgBitmap);

        // 2. プレイヤー情報の抽出と解析
        const p1Config = {
            boardRect: { x: 304, y: 157, w: 670 - 304, h: 882 - 157 },
            nextCoords: [ {x:160, y:155}, {x:500, y:122}, {x:500, y:175}, {x:500, y:225}, {x:500, y:275}, {x:500, y:325} ]
        };
        const p2Config = {
            boardRect: { x: 1257, y: 157, w: 1620 - 1257, h: 882 - 157 },
            nextCoords: [ {x:790, y:155}, {x:1130, y:122}, {x:1130, y:175}, {x:1130, y:225}, {x:1130, y:275}, {x:1130, y:325} ]
        };

        // P1
        const p1Data = await analyzePlayerHighPrecision(processedCanvas, imgBitmap, cropData, p1Config);
        editorData['p1'].board = p1Data.board;
        editorData['p1'].nextQueue = p1Data.nextQueue;
        editorData['p1'].hold = p1Data.holdMino;
        drawEditorField('p1');
        updateNextQueueDisplay('p1');

        // P2 (2Pモード時)
        if (gameMode === '2P') {
            const p2Data = await analyzePlayerHighPrecision(processedCanvas, imgBitmap, cropData, p2Config);
            editorData['p2'].board = p2Data.board;
            editorData['p2'].nextQueue = p2Data.nextQueue;
            editorData['p2'].hold = p2Data.holdMino;
            drawEditorField('p2');
            updateNextQueueDisplay('p2');
        }
        
    } catch (err) {
        console.error("High Precision Analysis Error:", err);
        alert("解析中にエラーが発生しました: " + err.message);
    }
}

function processImageTo1080p(imgBitmap) {
    const srcW = imgBitmap.width;
    const srcH = imgBitmap.height;
    const targetAspect = 16 / 9;
    const srcAspect = srcW / srcH;
    let cropW, cropH, cropX, cropY;

    if (srcAspect > targetAspect) {
        cropH = srcH;
        cropW = srcH * targetAspect;
        cropX = (srcW - cropW) / 2;
        cropY = 0;
    } else {
        cropW = srcW;
        cropH = srcW / targetAspect;
        cropX = 0;
        cropY = (srcH - cropH) / 2;
    }

    const canvas = document.createElement('canvas');
    canvas.width = 1920;
    canvas.height = 1080;
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

    // (A) 旧ロジックによる盤面スキャン (補正用)
    const classicBoard = [];
    const blockWidthPx = cellW;
    const blockHeightPx = cellH;
    for (let r = 0; r < 20; r++) {
        const row = [];
        for (let c = 0; c < 10; c++) {
            const sampleX = boardRect.x + (c + 0.5) * blockWidthPx;
            const sampleY = boardRect.y + (r + 0.5) * blockHeightPx;
            const sampleSize = Math.max(1, Math.floor(blockWidthPx * 0.25));
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

    // (B) ONNX 特徴量抽出と推論
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
        const r = Math.floor(i / 10);
        const c = i % 10;
        const classIdx = Number(labelData[i]);
        const label = ONNX_CLASS_NAMES[classIdx];
        recognizedBoard[r][c] = (label === 'null') ? null : label;
    }

    // (C) 結果のマージ (補正処理)
    for (let r = 0; r < 20; r++) {
        const onnxRow = recognizedBoard[r];
        const isOnlyNullOrG = onnxRow.every(cell => cell === null || cell === 'G');
        if (isOnlyNullOrG) {
            const gCountOnnx = onnxRow.filter(cell => cell === 'G').length;
            if (gCountOnnx !== 10) {
                const classicRow = classicBoard[r];
                const gCountClassic = classicRow.filter(cell => cell === 'G').length;
                const nullCountClassic = classicRow.filter(cell => cell === null).length;
                if (gCountClassic === 9 && nullCountClassic === 1) {
                    recognizedBoard[r] = [...classicRow];
                }
            }
        }
    }

    // フルボード化 (40行)
    const fullBoard = Array.from({ length: 40 }, () => Array(10).fill(null));
    for(let r=0; r<20; r++) {
        fullBoard[20+r] = recognizedBoard[r];
    }

    // ガベージ・削除列処理
    let firstNonGarbageRowFromBottom = -1;
    for (let y = 39; y >= 0; y--) { if (!fullBoard[y].includes('G')) { firstNonGarbageRowFromBottom = y; break; } }
    if (firstNonGarbageRowFromBottom !== -1) { 
        for (let y = firstNonGarbageRowFromBottom - 1; y >= 0; y--) { 
            for (let x = 0; x < 10; x++) { if (fullBoard[y][x] === 'G') { fullBoard[y][x] = null; } } 
        } 
    }

    let firstEmptyRowFromBottom = -1;
    for (let y = 39; y >= 0; y--) { if (fullBoard[y].every(cell => cell === null)) { firstEmptyRowFromBottom = y; break; } }
    
    const deletedMinoColors = [];
    const pendingDeletions = [];

    if (firstEmptyRowFromBottom !== -1) {
        const limitY = 40 - 18;
        for (let y = firstEmptyRowFromBottom - 1; y >= 0; y--) {
            if (y <= limitY) {
                for (let x = 0; x < 10; x++) {
                    const piece = fullBoard[y][x];
                    if (piece && piece !== 'G') { deletedMinoColors.push(piece); }
                    if (piece) pendingDeletions.push({y, x});
                }
            }
        }
    }

    // Next/Hold スキャン
    const nextQueue = [];
    let holdMino = null;
    
    const rawCropCanvas = document.createElement('canvas');
    rawCropCanvas.width = cropData.cropW;
    rawCropCanvas.height = cropData.cropH;
    const rawCtx = rawCropCanvas.getContext('2d', { willReadFrequently: true });
    rawCtx.drawImage(originalBitmap, cropData.cropX, cropData.cropY, cropData.cropW, cropData.cropH, 0, 0, cropData.cropW, cropData.cropH);
    
    const currentScale = cropData.cropW / 1280;
    const radius = 5 * currentScale;
    
    for (let i = 0; i < config.nextCoords.length; i++) {
        const coord = config.nextCoords[i];
        const avgColor = getAverageColorNonBlack(rawCtx, coord.x * currentScale, coord.y * currentScale, radius);
        if (i === 0) {
            const isBlack = avgColor.r < 50 && avgColor.g < 50 && avgColor.b < 50;
            if (!isBlack) {
                holdMino = findClosestMinoOnly(avgColor.r, avgColor.g, avgColor.b);
            }
        } else {
            const isBlack = avgColor.r < 50 && avgColor.g < 50 && avgColor.b < 50;
            if (i === 1 && isBlack) break;
            const foundMino = findClosestMinoOnly(avgColor.r, avgColor.g, avgColor.b);
            if (foundMino) nextQueue.push(foundMino);
        }
    }

    if (deletedMinoColors.length > 0 && deletedMinoColors.every(color => color === deletedMinoColors[0])) {
        nextQueue.unshift(deletedMinoColors[0]);
        pendingDeletions.forEach(p => fullBoard[p.y][p.x] = null);
    }

    return { board: fullBoard, holdMino, nextQueue };
}

function extractCellPixels(sourceImgData, x, y, w, h) {
    const sw = sourceImgData.width;
    const ix = Math.floor(x), iy = Math.floor(y);
    const iw = Math.floor(w), ih = Math.floor(h);
    const data = new Uint8ClampedArray(iw * ih * 4);
    for (let row = 0; row < ih; row++) {
        const srcRowStart = ((iy + row) * sw + ix) * 4;
        const destRowStart = (row * iw) * 4;
        const rowPixels = sourceImgData.data.subarray(srcRowStart, srcRowStart + iw * 4);
        data.set(rowPixels, destRowStart);
    }
    return data;
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
        bCh[i] = b; gCh[i] = g; rCh[i] = r;
        
        const maxVal = Math.max(r, g, b);
        const minVal = Math.min(r, g, b);
        const diff = maxVal - minVal;
        const v = maxVal;
        let s = (maxVal !== 0) ? (diff / maxVal) * 255 : 0;
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
                    if(idx < numPixels) { sumB += bCh[idx]; sumG += gCh[idx]; sumR += rCh[idx]; count++; }
                }
            }
            if(count===0) { tinyFeats.push(0,0,0); } else { tinyFeats.push(sumB/count, sumG/count, sumR/count); }
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
    if(cCount===0) feats.push(0,0,0); else feats.push(cSumB/cCount, cSumG/cCount, cSumR/cCount);

    return new Float32Array(feats);
}

