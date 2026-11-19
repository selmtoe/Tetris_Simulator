
// --- グローバル変数 & 定数 ---
const CELL_CNN_MODEL_URL = './tetris.onnx?v=cell-cnn-stage1-e513a35d';
// ONNXセッション
let session = null;

// ログ機能
function log(msg, type = 'info') {
    const logArea = document.getElementById('logArea');
    const div = document.createElement('div');
    div.textContent = `[${new Date().toLocaleTimeString()}] ${msg}`;
    if (type === 'error') div.className = 'log-error';
    else if (type === 'warn') div.className = 'log-warn';
    else div.className = 'log-info';
    logArea.appendChild(div);
    logArea.scrollTop = logArea.scrollHeight;
    console.log(msg);
}

window.onerror = function(message, source, lineno, colno, error) {
    log(`FATAL ERROR: ${message} at ${lineno}:${colno}`, 'error');
};

// --- 初期化処理 ---
// 自動でモデルをロードする
async function initModel() {
    try {
        log("モデルを読み込み中...", 'info');
        session = await ort.InferenceSession.create(CELL_CNN_MODEL_URL, { executionProviders: ['wasm'] });
        log("Cell CNNモデルロード完了　拡張機能からの画像待機中...", 'info');
        document.getElementById('statusMessage').textContent = "準備完了";
        
        // 待機していた画像があれば処理
        if (pendingImageData) {
            log("画像を処理します。", 'info');
            processExtensionImage(pendingImageData);
        }
    } catch (err) {
        log(`モデルロード失敗: ${err.message}`, 'error');
        document.getElementById('statusMessage').textContent = "エラー: tetris.onnx が見つかりません。";
    }
}

// 拡張機能から呼ばれるグローバル関数
let pendingImageData = null;
window.receiveExtensionImage = async (base64Data) => {
    log("拡張機能から画像データを受信しました", 'info');
    if (!session) {
        log("モデルがまだロードされていません　ロード後に処理します。", 'warn');
        pendingImageData = base64Data;
        return;
    }
    await processExtensionImage(base64Data);
};

async function processExtensionImage(base64Data) {
    document.getElementById('statusMessage').textContent = "解析中... 転送準備中";
    const img = new Image();
    img.onload = async () => {
        const imgBitmap = await createImageBitmap(img);
        runAnalysis(imgBitmap); // 解析実行
    };
    img.src = base64Data;
}

// ページ読み込み時に初期化
initModel();
document.getElementById('imageInput').addEventListener('change', () => checkReady());

function checkReady() {
    const modelLoaded = !!session;
    const imgSelected = document.getElementById('imageInput').files.length > 0;
    document.getElementById('runBtn').disabled = !(modelLoaded && imgSelected);
}

// --- メイン処理 ---
// 既存のイベントリスナーは残すが、メインロジックを関数化して再利用可能にする
document.getElementById('runBtn').addEventListener('click', async () => {
    const imgFile = document.getElementById('imageInput').files[0];
    if(imgFile) {
        const imgBitmap = await createImageBitmap(imgFile);
        runAnalysis(imgBitmap);
    }
});

async function runAnalysis(imgBitmap) {
    try {
        log("処理を開始します...", 'info');
        
        // 1. 画像の前処理 (16:9 クロップ & 1920x1080 リサイズ)
        // クロップ情報(cropData)も受け取る
        const { canvas: processedCanvas, cropData } = processImageTo1080p(imgBitmap);
        
        // デバッグ表示
        const debugCanvas = document.getElementById('debugCanvas');
        debugCanvas.width = processedCanvas.width;
        debugCanvas.height = processedCanvas.height;
        debugCanvas.getContext('2d').drawImage(processedCanvas, 0, 0);

        // 2. プレイヤー情報の抽出と解析
        // 座標定義
        // Board: 1920x1080基準 (ONNX用)
        // Next/Hold: 1280基準 (content.jsオリジナルの座標、後でcrop幅に合わせてスケール計算)
        
        const p1Config = {
            boardRect: { x: 316, y: 157, w: 351, h: 713 },
            nextCoords: [ {x:160, y:155}, {x:500, y:122}, {x:500, y:175}, {x:500, y:225}, {x:500, y:275}, {x:500, y:325} ]
        };
        const p2Config = {
            boardRect: { x: 1253, y: 157, w: 354, h: 713 },
            nextCoords: [ {x:790, y:155}, {x:1130, y:122}, {x:1130, y:175}, {x:1130, y:225}, {x:1130, y:275}, {x:1130, y:325} ]
        };

        log("Player 1 解析中...", 'info');
        // オリジナルの画素データ(imgBitmap)とクロップ情報を渡す
        const p1Data = await analyzePlayer(processedCanvas, imgBitmap, cropData, p1Config);
        
        log("Player 2 解析中...", 'info');
        const p2Data = await analyzePlayer(processedCanvas, imgBitmap, cropData, p2Config);

        // 3. 結果URL生成
        const stateData = {
            v: 2,
            m: "2P",
            p1: { b: boardToString(p1Data.board), n: p1Data.nextQueue.join(''), h: p1Data.holdMino || '' },
            p2: { b: boardToString(p2Data.board), n: p2Data.nextQueue.join(''), h: p2Data.holdMino || '' }
        };

        const jsonString = JSON.stringify(stateData);
        const uint8Array = new TextEncoder().encode(jsonString);
        // Base64エンコード (バイナリ対応)
        const base64Data = btoa(String.fromCharCode.apply(null, uint8Array));
        const simulatorUrl = `../index.html#${base64Data}`;
        
        const linkEl = document.getElementById('simLink');
        linkEl.href = simulatorUrl;
        linkEl.textContent = simulatorUrl;
        document.getElementById('resultArea').classList.remove('hidden');

        log("解析完了！転送中...", 'info');
        
        // 自動リダイレクト
        window.location.href = simulatorUrl;
        
    } catch (err) {
        log(`実行時エラー: ${err.stack}`, 'error');
    }
}

// --- 画像処理ロジック ---

function processImageTo1080p(imgBitmap) {
    // 中央から16:9で最大クロップ -> 1920x1080にリサイズ
    const srcW = imgBitmap.width;
    const srcH = imgBitmap.height;
    const targetAspect = 16 / 9;
    const srcAspect = srcW / srcH;
    let cropW, cropH, cropX, cropY;

    if (srcAspect > targetAspect) {
        // 横長すぎる -> 高さに合わせる
        cropH = srcH;
        cropW = srcH * targetAspect;
        cropX = (srcW - cropW) / 2;
        cropY = 0;
    } else {
        // 縦長すぎる -> 幅に合わせる
        cropW = srcW;
        cropH = srcW / targetAspect;
        cropX = 0;
        cropY = (srcH - cropH) / 2;
    }

    const canvas = document.createElement('canvas');
    canvas.width = 1920;
    canvas.height = 1080;
    const ctx = canvas.getContext('2d', { willReadFrequently: true });

    // クロップして描画
    ctx.drawImage(imgBitmap, cropX, cropY, cropW, cropH, 0, 0, 1920, 1080);
    // クロップ情報も合わせて返す
    return { canvas, cropData: { cropX, cropY, cropW, cropH } };
}

async function analyzePlayer(canvas, originalBitmap, cropData, config) {
    const ctx = canvas.getContext('2d');
    // 1. 盤面スキャン (ONNXモデル使用 + 旧ロジックによる補正)
    // ※ 盤面解析は1080pリサイズ済みの画像で行う (ONNXモデルがその解像度で精度が出ているため)
    const boardRect = config.boardRect;
    const cellW = boardRect.w / 10;
    const cellH = boardRect.h / 20;
    const recognizedBoard = []; // 20x10 array (ONNX Result)

    // --- (A) 旧ロジックによる盤面スキャン (content.jsベース) ---
    const classicBoard = [];
    const blockWidthPx = cellW;
    const blockHeightPx = cellH;
    
    for (let r = 0; r < 20; r++) {
        const row = [];
        for (let c = 0; c < 10; c++) {
            const sampleX = boardRect.x + (c + 0.5) * blockWidthPx;
            const sampleY = boardRect.y + (r + 0.5) * blockHeightPx;
            const sampleSize = Math.max(1, Math.floor(blockWidthPx * 0.25));
            // ピクセル取得 (content.jsと同等の処理)
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

    // --- (B) Cell CNN inference with 200 RGB 32x32 cells ---
    const predictedLabels = await TetrisCellCnn.recognizeBoard(session, ort, canvas, boardRect);
    for (let i = 0; i < 200; i++) {
        const r = Math.floor(i / 10);
        const c = i % 10;
        if (!recognizedBoard[r]) recognizedBoard[r] = new Array(10).fill(null);
        const label = predictedLabels[i];
        recognizedBoard[r][c] = (label === 'null') ? null : label;
    }

    // --- (C) 結果のマージ (補正処理) ---
    // 条件: ONNXの行が(null or G)のみ、かつGが10個ではない(穴あきG列など)
    // かつ、旧ロジックが「Gが9個、nullが1個」と判定している場合 -> 旧ロジックを採用
    for (let r = 0; r < 20; r++) {
        const onnxRow = recognizedBoard[r];
        const isOnlyNullOrG = onnxRow.every(cell => cell === null || cell === 'G');
        if (isOnlyNullOrG) {
            const gCountOnnx = onnxRow.filter(cell => cell === 'G').length;
            if (gCountOnnx !== 10) {
                // 補正候補
                const classicRow = classicBoard[r];
                const gCountClassic = classicRow.filter(cell => cell === 'G').length;
                const nullCountClassic = classicRow.filter(cell => cell === null).length;
                if (gCountClassic === 9 && nullCountClassic === 1) {
                    // 旧ロジックの結果で上書き
                    recognizedBoard[r] = [...classicRow];
                }
            }
        }
    }

    // --- 既存ロジックの適用 (フルボード化、ガベージ、削除列) ---
    const fullBoard = Array.from({ length: 40 }, () => Array(10).fill(null));
    for(let r=0; r<20; r++) {
        fullBoard[20+r] = recognizedBoard[r];
    }

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
    const pendingDeletions = []; // 削除候補の座標リスト

    if (firstEmptyRowFromBottom !== -1) {
        const limitY = 40 - 18;
        for (let y = firstEmptyRowFromBottom - 1; y >= 0; y--) {
            if (y <= limitY) {
                for (let x = 0; x < 10; x++) {
                    const piece = fullBoard[y][x];
                    if (piece && piece !== 'G') { deletedMinoColors.push(piece); }
                    // 即座に消さず、候補として記録する
                    if (piece) pendingDeletions.push({y, x});
                }
            }
        }
    }

    // 2. Next/Hold スキャン (content.js完全準拠・元画像使用)
    const nextQueue = [];
    let holdMino = null;
    
    // 元画像のクロップ領域からCanvasを作成 (リサイズなし)
    const rawCropCanvas = document.createElement('canvas');
    rawCropCanvas.width = cropData.cropW;
    rawCropCanvas.height = cropData.cropH;
    const rawCtx = rawCropCanvas.getContext('2d', { willReadFrequently: true });
    // 元画像をクロップ領域に従って描画
    rawCtx.drawImage(originalBitmap, cropData.cropX, cropData.cropY, cropData.cropW, cropData.cropH, 0, 0, cropData.cropW, cropData.cropH);

    // 実際のスケール倍率 (1280pxに対する現在のクロップ幅の比率)
    const currentScale = cropData.cropW / 1280;
    const radius = 5 * currentScale;

    for (let i = 0; i < config.nextCoords.length; i++) {
        const coord = config.nextCoords[i];
        // 座標(1280基準)に現在のスケールを適用して色取得
        const avgColor = getAverageColorNonBlack(rawCtx, coord.x * currentScale, coord.y * currentScale, radius);
        if (i === 0) {
            holdMino = findClosestColor(avgColor.r, avgColor.g, avgColor.b);
        } else {
            const isBlack = avgColor.r < 50 && avgColor.g < 50 && avgColor.b < 50;
            if (i === 1 && isBlack) break;
            const foundMino = findClosestMinoOnly(avgColor.r, avgColor.g, avgColor.b);
            if (foundMino) nextQueue.push(foundMino);
        }
    }

    // 浮いているミノの処理: 削除色が全て同一の場合のみNextに追加し、ボードから削除を実行
    if (deletedMinoColors.length > 0 && deletedMinoColors.every(color => color === deletedMinoColors[0])) {
        nextQueue.unshift(deletedMinoColors[0]);
        // 確定した削除をボードに適用
        pendingDeletions.forEach(p => fullBoard[p.y][p.x] = null);
    }
    // else: 色が混ざっている、または存在しない場合は削除をキャンセル (fullBoardは変更されない)

    return { board: fullBoard, holdMino, nextQueue };
}

// --- 既存のユーティリティ関数 (content.jsより移植・調整) ---

const SCAN_COLOR_PALETTE = { 'NULL': ['#000000', '#302838'], 'G': ['#999999', '#D8D8D8'], 'I': ['#019899', '#0199D5'], 'O': ['#999A02', '#F9B900'], 'T': ['#980099', '#871E88'], 'L': ['#996700', '#F56100'], 'J': ['#0000BB', '#004BA5'], 'S': ['#10971F', '#5CB523'], 'Z': ['#990000', '#DA1822'] };
const hexToRgb = (hex) => { const r = parseInt(hex.slice(1, 3), 16), g = parseInt(hex.slice(3, 5), 16), b = parseInt(hex.slice(5, 7), 16);
return { r, g, b }; };
const PARSED_SCAN_COLORS = {};
for (const key in SCAN_COLOR_PALETTE) { PARSED_SCAN_COLORS[key] = SCAN_COLOR_PALETTE[key].map(hexToRgb);
}
    const colorDistanceSq = (c1, c2) => (Math.pow(c1.r - c2.r, 2) + Math.pow(c1.g - c2.g, 2) + Math.pow(c1.b - c2.b, 2));
    function findClosestColor(r, g, b) { 
    const inputColor = { r, g, b };
    for (const nullColor of PARSED_SCAN_COLORS.NULL) { 
        if (colorDistanceSq(inputColor, nullColor) < 6000) return null;
    } 
    for (const gColor of PARSED_SCAN_COLORS.G) { 
        if (colorDistanceSq(inputColor, gColor) < 10000) return 'G';
    } 
    let minDistance = Infinity, closestKey = null;
    const minoKeys = Object.keys(PARSED_SCAN_COLORS).filter(k => k !== 'NULL' && k !== 'G');
    for (const key of minoKeys) { 
        for (const targetColor of PARSED_SCAN_COLORS[key]) { 
            const distance = colorDistanceSq(inputColor, targetColor);
            if (distance < minDistance) { minDistance = distance; closestKey = key;
            } 
        } 
    } 
    return (minDistance > 25000) ? null : closestKey; 
}

function findClosestMinoOnly(r, g, b) { 
    const inputColor = { r, g, b };
    let minDistance = Infinity, closestKey = 'I'; 
    const minoKeys = Object.keys(PARSED_SCAN_COLORS).filter(k => k !== 'NULL' && k !== 'G');
    for (const key of minoKeys) { 
        for (const targetColor of PARSED_SCAN_COLORS[key]) { 
            const distance = colorDistanceSq(inputColor, targetColor);
            if (distance < minDistance) { minDistance = distance; closestKey = key;
            } 
        } 
    } 
    return closestKey;
}

function getAverageColorNonBlack(ctx, cx, cy, radius) {
    const startX = Math.max(0, Math.floor(cx - radius)), startY = Math.max(0, Math.floor(cy - radius)), diameter = Math.ceil(radius * 2), endX = Math.min(ctx.canvas.width, startX + diameter), endY = Math.min(ctx.canvas.height, startY + diameter), width = endX - startX, height = endY - startY;
    if (width <= 0 || height <= 0) return { r: 0, g: 0, b: 0 };
    const imageData = ctx.getImageData(startX, startY, width, height).data;
    let totalR = 0, totalG = 0, totalB = 0, count = 0;
    const radiusSq = radius * radius;
    const blackThreshold = 50;
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const dx = (startX + x) - cx;
        const dy = (startY + y) - cy;
        if (dx * dx + dy * dy <= radiusSq) {
          const i = (y * width + x) * 4;
          const r = imageData[i], g = imageData[i + 1], b = imageData[i + 2];
          if (r > blackThreshold || g > blackThreshold || b > blackThreshold) {
            totalR += r;
            totalG += g; totalB += b; count++;
          }
        }
      }
    }
    return count === 0 ? { r: 0, g: 0, b: 0 } : { r: totalR / count, g: totalG / count, b: totalB / count };
}

const boardToString = (board) => board.map(row => row.map(cell => cell === null ? '_' : cell).join('')).join('');

