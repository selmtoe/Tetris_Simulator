function getFumenDataForExport(pages = fumenPages, mode = gameMode) {
    const exportedData = {
        v: 'f2', // Fumen version 2 (圧縮対応)
        m: mode,
        p: []
    };

    // 差分圧縮のために直前の1Dボードデータを保持
    let prevP1Board1D = null;
    let prevP2Board1D = null;

    for (let i = 0; i < pages.length; i++) {
        const page = pages[i];
        const pageData = {};

        // P1
        const currentP1Board1D = boardToString(page.p1.board).split('');
        let p1BoardCompressed;
        if (i === 0) {
            // 1ページ目: 生データをRLE
            p1BoardCompressed = encodeRLE(currentP1Board1D);
        } else {
            // 2ページ目以降: 差分をRLE
            const diff = getDifference(prevP1Board1D, currentP1Board1D);
            p1BoardCompressed = encodeRLE(diff);
        }
        pageData.p1 = {
            b: p1BoardCompressed, // 圧縮データを格納
            h: page.p1.hold || '',
            n: typeof displayNextForPage === 'function' && pages === fumenPages
                ? displayNextForPage('p1', i)
                : (page.p1.next || '')
        };
        const p1Operation = typeof operationForPage === 'function' ? operationForPage(page.p1) : null;
        if (p1Operation) pageData.p1.o = {
            type: p1Operation.type,
            rotation: p1Operation.rotation,
            x: p1Operation.x,
            y: p1Operation.y,
            lock: p1Operation.lock,
            ...(p1Operation.holdUsed ? { holdUsed: true } : {})
        };
        prevP1Board1D = currentP1Board1D; // 次の差分のために現在地を保存

        // P2 (2Pモード時)
        if (mode === '2P') {
            const currentP2Board1D = boardToString(page.p2.board).split('');
            let p2BoardCompressed;
            if (i === 0) {
                p2BoardCompressed = encodeRLE(currentP2Board1D);
            } else {
                const diff = getDifference(prevP2Board1D, currentP2Board1D);
                p2BoardCompressed = encodeRLE(diff);
            }
            pageData.p2 = {
                b: p2BoardCompressed,
                h: page.p2.hold || '',
                n: typeof displayNextForPage === 'function' && pages === fumenPages
                    ? displayNextForPage('p2', i)
                    : (page.p2.next || '')
            };
            const p2Operation = typeof operationForPage === 'function' ? operationForPage(page.p2) : null;
            if (p2Operation) pageData.p2.o = {
                type: p2Operation.type,
                rotation: p2Operation.rotation,
                x: p2Operation.x,
                y: p2Operation.y,
                lock: p2Operation.lock,
                ...(p2Operation.holdUsed ? { holdUsed: true } : {})
            };
            prevP2Board1D = currentP2Board1D;
        }
        
        exportedData.p.push(pageData);
    }
    
    return exportedData;
}

function getCollectionDataForExport() {
    return typeof collectionData === 'function' ? collectionData() : getFumenDataForExport();
}

function encodeBase64Utf8(text) {
    const bytes = new TextEncoder().encode(text);
    let binary = '';
    const chunkSize = 0x8000;
    for (let index = 0; index < bytes.length; index += chunkSize) {
        binary += String.fromCharCode(...bytes.subarray(index, index + chunkSize));
    }
    return btoa(binary);
}

function encodeBase64UrlBytes(bytes) {
    let binary = '';
    for (const byte of bytes) binary += String.fromCharCode(byte);
    return btoa(binary).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/g, '');
}

function decodeBase64UrlBytes(text) {
    let normalized = String(text || '').replace(/-/g, '+').replace(/_/g, '/');
    while (normalized.length % 4) normalized += '=';
    const binary = atob(normalized);
    return Uint8Array.from(binary, value => value.charCodeAt(0));
}

async function encodeSharedStateHash(text) {
    // The collection format contains every board on every page.  Plain
    // Base64 makes a long 2P replay fragile when copied through a browser or
    // chat application, so use a compact gzip hash when supported.
    if (typeof CompressionStream === 'function') {
        try {
            const compressed = await new Response(
                new Blob([text]).stream().pipeThrough(new CompressionStream('gzip'))
            ).arrayBuffer();
            return `z1.${encodeBase64UrlBytes(new Uint8Array(compressed))}`;
        } catch (error) {
            console.warn('Compressed share link unavailable; using plain Base64', error);
        }
    }
    return encodeBase64Utf8(text);
}

async function decodeSharedStateText(value) {
    let text = String(value || '').trim().replace(/^\uFEFF/, '');
    if (!text) throw new Error('Empty shared data');
    const internetShortcut = text.match(/^\[InternetShortcut\]\s*URL=(\S+)/i);
    if (internetShortcut) return decodeSharedStateText(internetShortcut[1]);
    if (text.startsWith('{') || text.startsWith('[')) return JSON.parse(text);

    if (/^https?:\/\//i.test(text)) {
        const hashIndex = text.indexOf('#');
        if (hashIndex >= 0) text = text.slice(hashIndex + 1);
        else {
            const dataIndex = text.indexOf('d=');
            if (dataIndex >= 0) return text.slice(dataIndex + 2);
        }
    }
    if (text.startsWith('#')) text = text.slice(1);
    text = decodeURIComponent(text).replace(/\s+/g, '').replace(/-/g, '+').replace(/_/g, '/');

    if (text.startsWith('z1.')) {
        if (typeof DecompressionStream !== 'function') {
            throw new Error('This browser cannot decompress shared replay links');
        }
        const compressed = decodeBase64UrlBytes(text.slice(3));
        const decoded = await new Response(
            new Blob([compressed]).stream().pipeThrough(new DecompressionStream('gzip'))
        ).text();
        return JSON.parse(decoded);
    }

    while (text.length % 4) text += '=';
    const binaryString = atob(text);
    const bytes = Uint8Array.from(binaryString, c => c.charCodeAt(0));
    const decoded = new TextDecoder().decode(bytes).replace(/^\uFEFF/, '').trim();
    try {
        return JSON.parse(decoded);
    } catch (error) {
        return decoded;
    }
}

// --- テト譜 v115 変換ロジック ---
const FumenCodec = {
    TABLE: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",
    BLOCK_MAP: { 0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 7, 8: 8 }, // Map indices to themselves
    // Custom colors: I, L, O, Z, T, J, S, G(Gray), Empty
    TYPE_TO_FUMEN: { null: 0, 'I': 1, 'L': 2, 'O': 3, 'Z': 4, 'T': 5, 'J': 6, 'S': 7, 'G': 8, 'X': 8 },
    FUMEN_TO_TYPE: { 0: null, 1: 'I', 2: 'L', 3: 'O', 4: 'Z', 5: 'T', 6: 'J', 7: 'S', 8: 'G' },

    toInt: function(char) {
        return this.TABLE.indexOf(char);
    },

    toChar: function(int) {
        return this.TABLE[int];
    },

    poll: function(str, index, numChars) {
        let val = 0;
        for (let i = 0; i < numChars; i++) {
            val += this.toInt(str[index + i]) * Math.pow(64, i);
        }
        return val;
    },

    encodeInt: function(num, length) {
        let str = '';
        for (let i = 0; i < length; i++) {
            str += this.toChar(num % 64);
            num = Math.floor(num / 64);
        }
        return str;
    },

    // 1ページ分のエンコード
    encodePage: function(prevField, currentField, operation) {
        let data = '';

        // 1. フィールド (Diff + RLE)
        // テト譜フィールド: 240ブロック (24行x10列)。インデックス0が上、239が下。
        // 230-239はせり上がり(ガベージ)行。今回は全て0(空)とする。
        // Customフィールド(40行)の下から23行分を使用する。
        // Custom[17] (Top of 23) -> Fumen[0]
        // Custom[39] (Bottom) -> Fumen[229]
        
        let diffs = [];
        for (let i = 0; i < 240; i++) {
            let val = currentField[i] - prevField[i] + 8;
            diffs.push(val);
        }

        let i = 0;
        while (i < 240) {
            let val = diffs[i];
            let count = 0;
            while (i + count < 240 && diffs[i + count] === val && count < 240) { // count max is technically limited but safe here
                count++;
            }
            // テト譜の仕様: (diff * 240) + (count - 1)
            let chunk = val * 240 + (count - 1);
            data += this.encodeInt(chunk, 2);
            i += count;
        }

        // 1ページ目が空白の場合など、フィールドデータが"vh"（全ブロック変更なし）の場合は
        // Repeat(繰り返し数)を付与する必要がある。今回は毎回出力しているので0回(A)とする。
        if (data === 'vh') {
            data += this.encodeInt(0, 1);
        }

        // 2. ミノ・フラグ (3文字)

        // 2. ミノ・フラグ (3文字)
        // 今回はエディタ上のアクティブミノは再現せず、フラグ(コメント)のみ利用する
        // piece=0, rot=0, loc=0
        // flag_comment: Hold/Nextがある場合は1にする
        const normalizedOperation = typeof editorOperationToFumen === 'function'
            ? editorOperationToFumen(operation)
            : (typeof normalizeOperation === 'function' ? normalizeOperation(operation) : null);
        const hasQuiz = false;
        const flag_comment = 0;
        const flag_lock = 1; // 接着済みとして扱う
        // tetris-fumen's `colorize` flag must be true (1). With 0, the
        // official viewer renders operation cells with a fallback color.
        const flag_color = 1;
        const flag_mirror = 0;
        const flag_raise = 0;
        
        const pieceMap = { I: 1, L: 2, O: 3, Z: 4, T: 5, J: 6, S: 7 };
        const rotationMap = { reverse: 0, right: 1, spawn: 2, left: 3 };
        const piece = normalizedOperation ? (pieceMap[normalizedOperation.type] || 0) : 0;
        const rot = normalizedOperation ? (rotationMap[normalizedOperation.rotation] ?? 2) : 0;
        let fumenX = normalizedOperation ? normalizedOperation.x : 0;
        let fumenY = normalizedOperation ? normalizedOperation.y : 22;
        if (normalizedOperation) {
            if (normalizedOperation.type === 'O') {
                if (normalizedOperation.rotation === 'left' || normalizedOperation.rotation === 'reverse') fumenX -= 1;
                if (normalizedOperation.rotation === 'left' || normalizedOperation.rotation === 'spawn') fumenY += 1;
            } else if (normalizedOperation.type === 'I') {
                if (normalizedOperation.rotation === 'reverse') fumenX -= 1;
                if (normalizedOperation.rotation === 'left') fumenY += 1;
            } else if (normalizedOperation.type === 'S') {
                if (normalizedOperation.rotation === 'spawn') fumenY += 1;
                if (normalizedOperation.rotation === 'right') fumenX += 1;
            } else if (normalizedOperation.type === 'Z') {
                if (normalizedOperation.rotation === 'spawn') fumenY += 1;
                if (normalizedOperation.rotation === 'left') fumenX -= 1;
            }
        }
        const loc = normalizedOperation ? (23 - fumenY - 1) * 10 + fumenX : 0;

        // Value計算 (Decodeの逆順に構成)
        // Decode順: piece -> rot -> loc -> raise -> mirror -> color -> comment -> lock
        // Encode順: lock -> comment -> color -> mirror -> raise -> loc -> rot -> piece
        
        let minoVal = 0;
        minoVal = (flag_lock ? 0 : 1); // lockフラグは反転 (Slide 29: !(value % 2))
        minoVal = minoVal * 2 + flag_comment;
        minoVal = minoVal * 2 + flag_color;
        minoVal = minoVal * 2 + flag_mirror;
        minoVal = minoVal * 2 + flag_raise;
        minoVal = minoVal * 240 + loc;
        minoVal = minoVal * 4 + rot;
        minoVal = minoVal * 8 + piece;

        data += this.encodeInt(minoVal, 3);

        if (hasQuiz) {
            // #Q=[hold](current)next
            // ユーザー指定により () を必須とする
            let quizStr = '#Q=';
            if (hold) quizStr += `[${hold}]`;
            else quizStr += `[]`;
            quizStr += `()`;
            quizStr += next;
            const COMMENT_TABLE = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
            let escStr = '';
            for(let j=0; j<quizStr.length; j++) {
                let char = quizStr[j];
                let idx = COMMENT_TABLE.indexOf(char);
                if(idx === -1) idx = 0; // fallback
                escStr += this.encodeInt(idx, 1); // テーブルインデックスそのものではなく、poll(1)対応文字?
                // Slide 32: "abc" -> ... 
                // Slide 37 example: 'abc' -> 'DABUYCA' ?? 
                // 正確には: 文字列長をpoll(2). 各文字を4096進数(poll 3)ではなく、
                // Slide 32: "escape()でASCII変換" -> "5文字のデータに変換"
                // 詳細仕様が複雑だが、単純なASCII文字の場合、テト譜エディタはURLエンコード等を許容する場合がある。
                // しかし安全のため、標準的なコメントエンコードを模倣する。
                // 簡易実装: URLパラメータに直接乗せるのではなく、バイナリデータ内のコメント領域。
                // テト譜のコメントエンコードは、4文字単位でパッキングする。
                // 文字列 -> ASCII code -> 96進数的な変換 -> 5文字chunk
                
                // 今回は複雑なコメント圧縮を避けるため、QuizなしでNext/Holdを手動設定させるか、
                // あるいは #Q= を正しくエンコードする必要がある。
                // 参照: tetris-fumen library (knewjade) logic.
                // 簡易的に実装するにはコストが高いので、今回は必須要件である「Next/Holdの対応」のため、
                // 最小限の実装を行う。
                
                // 4文字の文字列 -> 5文字のデータ
                // count = quizStr.length
                // data += encodeInt(count, 2)
                // loop chunks of 4 chars
            }
            
            // 文字数
            data += this.encodeInt(quizStr.length, 2);
            
            // 本文
            // 4文字ずつ区切って、各文字をテーブルのインデックス値(0-95)とする
            // val = c0 + c1*96 + c2*96^2 + c3*96^3
            // これをencodeInt(val, 5)で出力
            for(let k=0; k < quizStr.length; k += 4) {
                let chunkVal = 0;
                for(let m=0; m<4; m++) {
                    let char = (k+m < quizStr.length) ? quizStr[k+m] : '';
                    let idx = COMMENT_TABLE.indexOf(char);
                    if(idx === -1) idx = 0; // padding or unknown
                    if(k+m >= quizStr.length) idx = 0; // padding
                    chunkVal += idx * Math.pow(96, m);
                }
                data += this.encodeInt(chunkVal, 5);
            }
        }

        return data;
    },

    decode: function(str) {
        str = str.replace(/\?/g, '');
        if (!str.startsWith('v115@')) return null;
        let idx = 5;
        const pages = [];
        let prevField = Array(240).fill(0);
        let repeatCount = 0;

        while (idx < str.length) {
            let currentField = Array(240).fill(0);
            
            if (repeatCount > 0) {
                currentField = [...prevField];
                repeatCount--;
            } else {
                let totalBlocks = 0;
                let fieldIdx = 0;
                let isVh = false;
                let chunkCount = 0;

                while (totalBlocks < 240) {
                    const chunkVal = this.poll(str, idx, 2);
                    idx += 2;
                    chunkCount++;
                    if (chunkCount === 1 && chunkVal === 2159) {
                        isVh = true;
                    }
                    const diff = Math.floor(chunkVal / 240) - 8;
                    const count = (chunkVal % 240) + 1;
                    for (let k = 0; k < count; k++) {
                        if (fieldIdx < 240) {
                            currentField[fieldIdx] = prevField[fieldIdx] + diff;
                            fieldIdx++;
                        }
                    }
                    totalBlocks += count;
                }

                if (isVh) {
                    repeatCount = this.poll(str, idx, 1);
                    idx += 1;
                }
            }

            const nextPrevField = Array(240).fill(0);
            let writeRow = 22;
            for (let y = 22; y >= 0; y--) {
                let isFull = true;
                for (let x = 0; x < 10; x++) {
                    if (currentField[y * 10 + x] === 0) {
                        isFull = false;
                        break;
                    }
                }
                if (!isFull) {
                    for (let x = 0; x < 10; x++) {
                        nextPrevField[writeRow * 10 + x] = currentField[y * 10 + x];
                    }
                    writeRow--;
                }
            }
            prevField = nextPrevField;
            const minoVal = this.poll(str, idx, 3);
            idx += 3;
            
            let temp = minoVal;

            const piece = temp % 8; temp = Math.floor(temp / 8);
            const rot = temp % 4; temp = Math.floor(temp / 4);
            const loc = temp % 240; temp = Math.floor(temp / 240);
            const raise = temp % 2; temp = Math.floor(temp / 2);
            const mirror = temp % 2; temp = Math.floor(temp / 2);
            const color = temp % 2; temp = Math.floor(temp / 2);
            const commentFlag = temp % 2; temp = Math.floor(temp / 2);
            const lockFlag = temp % 2;
            
            let hold = '';
            let next = '';

            if (commentFlag === 1) {
                const lenVal = this.poll(str, idx, 2);
                idx += 2;
                
                const COMMENT_TABLE = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
                let commentStr = '';
                const numChunks = Math.ceil(lenVal / 4);
                
                for(let k=0; k<numChunks; k++) {
                    let val = this.poll(str, idx, 5);
                    idx += 5;
                    for(let m=0; m<4; m++) {
                        if (commentStr.length < lenVal) {
                            let cIdx = val % 96;
                            val = Math.floor(val / 96);
                            commentStr += COMMENT_TABLE[cIdx] || '';
                        }
                    }
                }

                commentStr = unescape(commentStr);

                if (commentStr.startsWith('#Q=')) {
                    const matchStrict = commentStr.match(/#Q=\[([a-zA-Z]*)\]\(([a-zA-Z]*)\)([a-zA-Z]*)/);
                    if (matchStrict) {
                        hold = matchStrict[1];
                        next = matchStrict[2] + matchStrict[3];
                    } else {
                        const match = commentStr.match(/#Q=\[([a-zA-Z]*)\]([a-zA-Z]*)/);
                        if (match) {
                            hold = match[1];
                            next = match[2];
                        }
                    }
                }
            }
            
            const typeByPiece = { 1: 'I', 2: 'L', 3: 'O', 4: 'Z', 5: 'T', 6: 'J', 7: 'S' };
            const rotationByCode = { 0: 'reverse', 1: 'right', 2: 'spawn', 3: 'left' };
            let operation = null;
            if (piece !== 0 && typeByPiece[piece]) {
                let fumenX = loc % 10;
                let fumenY = 22 - Math.floor(loc / 10);
                const type = typeByPiece[piece];
                const rotation = rotationByCode[rot] || 'spawn';
                if (type === 'O') {
                    if (rotation === 'left' || rotation === 'reverse') fumenX += 1;
                    if (rotation === 'left' || rotation === 'spawn') fumenY -= 1;
                } else if (type === 'I') {
                    if (rotation === 'reverse') fumenX += 1;
                    if (rotation === 'left') fumenY -= 1;
                } else if (type === 'S') {
                    if (rotation === 'spawn') fumenY -= 1;
                    if (rotation === 'right') fumenX -= 1;
                } else if (type === 'Z') {
                    if (rotation === 'spawn') fumenY -= 1;
                    if (rotation === 'left') fumenX += 1;
                }
                const officialOperation = { type, rotation, x: fumenX, y: fumenY, lock: lockFlag === 0 };
                operation = typeof fumenOperationToEditor === 'function'
                    ? fumenOperationToEditor(officialOperation)
                    : { ...officialOperation, y: 39 - fumenY };
            }

            const customBoard = Array.from({length: 40}, () => Array(10).fill(null));
            for(let y=0; y<23; y++) {
                for(let x=0; x<10; x++) {
                    const fVal = currentField[y * 10 + x];
                    customBoard[17 + y][x] = this.FUMEN_TO_TYPE[fVal] || null;
                }
            }

            pages.push({
                board: customBoard,
                hold: hold,
                next: next,
                operation
            });
        }
        return pages;
    },

    export: function(pages, playerId) {
        let str = 'v115@';
        let prevField = Array(240).fill(0); // Fumen starts with empty field

        for (const page of pages) {
            const pData = page[playerId];
            // Convert Custom Board to Fumen Field (240 ints)
            const currentField = Array(240).fill(0);
            
            // Copy bottom 23 lines
            // Custom[17]..[39] -> Fumen[0]..[229]
            for (let y = 0; y < 23; y++) {
                const srcY = 17 + y;
                if (srcY < 40) {
                    for (let x = 0; x < 10; x++) {
                        const cell = pData.board[srcY][x];
                        // ガベージライン(Fumen 230-239)は無視して0のまま
                        currentField[y * 10 + x] = this.TYPE_TO_FUMEN[cell] || 0;
                    }
                }
            }
            
            str += this.encodePage(prevField, currentField, pData.operation);
            
            // --- 後処理 (Post Processing) ---
            // 次のページの差分計算のために、現在ページでライン消去が発生した場合、
            // それを反映した状態を prevField (基準) とする。
            // これを行わないと、ライン消去を含むページの次のページで差分がズレてデータが崩壊する。
            
            const operation = typeof normalizeOperation === 'function'
                ? normalizeOperation(pData.operation)
                : null;
            if (operation && typeof operationCells === 'function') {
                const fumenPiece = this.TYPE_TO_FUMEN[operation.type] || 0;
                for (const [x, y] of operationCells(operation)) {
                    const fumenY = 39 - y;
                    if (x >= 0 && x < 10 && fumenY >= 0 && fumenY < 23) {
                        currentField[fumenY * 10 + x] = fumenPiece;
                    }
                }
            }

            const nextPrevField = Array(240).fill(0);
            let writeRow = 22; // Fumenフィールドは下から y=22 -> 0

            // 下から順にスキャン
            for (let y = 22; y >= 0; y--) {
                let isFull = true;
                for (let x = 0; x < 10; x++) {
                    // 0 (Empty) が一つでもあれば揃っていない
                    if (currentField[y * 10 + x] === 0) {
                        isFull = false;
                        break;
                    }
                }

                if (!isFull) {
                    // 揃っていない行だけをコピー（詰め処理）
                    for (let x = 0; x < 10; x++) {
                        nextPrevField[writeRow * 10 + x] = currentField[y * 10 + x];
                    }
                    writeRow--;
                }
                // 揃っている行はコピーしないことで消去とする
            }
            // writeRowより上の行は初期化時の0のまま（空白が補充される）

            prevField = nextPrevField;
        }
        return str;
    }
};


function applyFumenData(data) {
    try {

        // v1 (非圧縮) と v2 (圧縮) の両方に対応
        if (!data || !['f1', 'f2'].includes(data.v) || !data.p || data.p.length === 0) {
            alert('無効または非対応のデータです。');
            return false;
        }
        
        gameMode = data.m || '1P';
        document.getElementById('mode-1p').classList.toggle('active', gameMode === '1P');
        document.getElementById('mode-2p').classList.toggle('active', gameMode === '2P');
        document.getElementById('p2-editor-col').style.display = (gameMode === '2P') ? 'flex' : 'none';

        fumenPages = []; // いったん空にする
        let prevP1Board1D = null;
        let prevP2Board1D = null;

        for (let i = 0; i < data.p.length; i++) {
            const pageData = data.p[i];
            const newPage = createBlankPage();

            // P1
            if (pageData.p1) {
                let currentP1Board1D;
                if (data.v === 'f1') {
                    // v1: 非圧縮
                    currentP1Board1D = pageData.p1.b.split('');
                } else {
                    // v2: 圧縮 (RLE + 差分)
                    const rleDecoded = decodeRLE(pageData.p1.b);
                    if (i === 0) {
                        // 1ページ目: 生データ
                        currentP1Board1D = rleDecoded;
                    } else {
                        // 2ページ目以降: 差分
                        currentP1Board1D = applyDifference(prevP1Board1D, rleDecoded);
                    }
                }
                newPage.p1.board = stringToBoard(currentP1Board1D.join(''));
                newPage.p1.hold = pageData.p1.h || '';
                newPage.p1.next = pageData.p1.n || '';
                newPage.p1.operation = typeof normalizeOperation === 'function'
                    ? normalizeOperation(pageData.p1.o)
                    : null;
                prevP1Board1D = currentP1Board1D;
            }

            // P2
            if (gameMode === '2P' && pageData.p2) {
                    let currentP2Board1D;
                if (data.v === 'f1') {
                    currentP2Board1D = pageData.p2.b.split('');
                } else {
                    const rleDecoded = decodeRLE(pageData.p2.b);
                    if (i === 0) {
                        currentP2Board1D = rleDecoded;
                    } else {
                        currentP2Board1D = applyDifference(prevP2Board1D, rleDecoded);
                    }
                }
                newPage.p2.board = stringToBoard(currentP2Board1D.join(''));
                newPage.p2.hold = pageData.p2.h || '';
                newPage.p2.next = pageData.p2.n || '';
                newPage.p2.operation = typeof normalizeOperation === 'function'
                    ? normalizeOperation(pageData.p2.o)
                    : null;
                prevP2Board1D = currentP2Board1D;
            }
            
            fumenPages.push(newPage);
        }

        fumenCases = [createCase('Imported Set', 'snapshot')];
        fumenCases[0].pages = fumenPages;
        currentCaseIndex = 0;
        currentPageIndex = 0;
        loadPage(0);
        updateScale();
        return true;
    } catch (e) {
        console.error('Failed to apply fumen data:', e);
        alert('データの読み込みに失敗しました。');
        return false;
    }
}
    
async function generateAndDisplayLink() {
    const stateData = getCollectionDataForExport();
    const jsonString = JSON.stringify(stateData);
    const base64Data = await encodeSharedStateHash(jsonString);
    const url = new URL(window.location);
    url.hash = base64Data;
    document.getElementById('share-link-input').value = url.href;
}

async function openShareModal() {
    await generateAndDisplayLink();
    document.getElementById('share-modal').style.display = 'flex';
}

async function loadStateFromURL() {
    if (window.location.hash) {
        // Keep the source URL for decoding, then remove the very long hash
        // before any async gzip work or page normalization.  This prevents a
        // valid compressed replay from being left in the address bar while
        // the 128-page collection is being applied, and also avoids retrying
        // a malformed link on every reload.
        const sharedUrl = window.location.href;
        const cleanUrl = window.location.pathname + window.location.search;
        history.replaceState('', document.title, cleanUrl);
        try {
            const decodedState = await decodeSharedStateText(sharedUrl);
            
            // テト譜判定 (URLハッシュの場合は ?d=v115@ 等が含まれる可能性があるが、
            // ここでのロードは自作形式のBase64デコード後なので、JSONパースを試みる)
            let data;
            try {
                data = typeof decodedState === 'string' ? JSON.parse(decodedState) : decodedState;
            } catch(e) {
                // JSONでない場合、生の文字列としてチェック
                if (typeof decodedState === 'string' && decodedState.includes('v115@')) {
                    const match = decodedState.match(/v115@.*/);
                    if (match) {
                        const fumenPagesData = FumenCodec.decode(match[0]);
                        if (fumenPagesData) {
                            // 読み込み成功時の処理
                            fumenPages = [];
                            fumenCases = [createCase('Imported Fumen', 'snapshot')];
                            fumenCases[0].pages = fumenPages;
                            currentCaseIndex = 0;
                            fumenPagesData.forEach(p => {
                                const newPage = createBlankPage();
                                newPage.p1 = { ...newPage.p1, board: p.board, hold: p.hold, next: p.next, operation: p.operation || null };
                                // 2Pは空にする
                                fumenPages.push(newPage);
                            });
                            gameMode = '1P'; // テト譜は1Pのみ
                             document.getElementById('mode-1p').click();
                            currentPageIndex = 0;
                            loadPage(0);
                            alert('テト譜データを読み込みました。');
                            history.pushState("", document.title, window.location.pathname + window.location.search);
                            return;
                        }
                    }
                }
                throw e;
            }

            if (data?.simulatorData || data?.pageFormat === 'operation-pages/v1' || data?.version === 5) {
                applyVideoRecoveryData(data);
            } else if (data.v === 3) {
                applyCollectionData(data);
            } else if (data.v === 'f1' || data.v === 'f2') {

                if (applyFumenData(data)) {       
                }
            } else if (data.v === 2) {
                
                fumenPages = [createBlankPage()];
                
                gameMode = data.m || '1P';
                document.getElementById('mode-1p').classList.toggle('active', gameMode === '1P');
                document.getElementById('mode-2p').classList.toggle('active', gameMode === '2P');
                document.getElementById('p2-editor-col').style.display = (gameMode === '2P') ? 'flex' : 'none';

                if (data.p1) {
                    fumenPages[0].p1.board = stringToBoard(data.p1.b);
                    fumenPages[0].p1.next = data.p1.n || '';
                    fumenPages[0].p1.hold = data.p1.h || '';
                    fumenPages[0].p1.operation = typeof normalizeOperation === 'function'
                        ? normalizeOperation(data.p1.o)
                        : null;
                }
                if (gameMode === '2P' && data.p2) {
                    fumenPages[0].p2.board = stringToBoard(data.p2.b);
                    fumenPages[0].p2.next = data.p2.n || '';
                    fumenPages[0].p2.hold = data.p2.h || '';
                    fumenPages[0].p2.operation = typeof normalizeOperation === 'function'
                        ? normalizeOperation(data.p2.o)
                        : null;
                }
                currentPageIndex = 0;
                loadPage(0);
                alert('シミュレータのデータを譜面の1ページ目として読み込みました。');
            }

            history.pushState("", document.title, window.location.pathname + window.location.search);
        } catch (e) {
            console.error('Failed to load state from URL hash:', e);
            alert('URLからのデータ読み込みに失敗しました。');
            history.pushState("", document.title, window.location.pathname + window.location.search);
        }
    }
}
