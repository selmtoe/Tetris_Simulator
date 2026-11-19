document.addEventListener('DOMContentLoaded', () => {
    const modal = document.getElementById('code-gen-modal');   
    const genCodeBtn = document.getElementById('gen-code');
    const closeModalBtn = document.getElementById('close-modal-button');
    const copyCodeBtn = document.getElementById('copy-code-button');
    const codeOutput = document.getElementById('code-gen-output');
    const codeGenTabs = document.getElementById('code-gen-tabs');
    let currentGenType = 'check';

    const generateCode = (type) => {
        const page = fumenPages[currentPageIndex];
        let code = '';
        const playersToGen = (gameMode === '1P') ? ['p1'] : ['p1', 'p2'];

        const getBoardData = (board) => {
            const blocks = [];
            for (let y = 0; y < BOARD_HEIGHT; y++) {
                for (let x = 0; x < BOARD_WIDTH; x++) {
                    if (board[y][x]) {
                        blocks.push({ x, y, type: board[y][x] });
                    }
                }
            }
            return blocks;
        };

        if (type === 'check') {
            code = 'let isSuccess = true;\n';
            playersToGen.forEach(pid => {
                const blocks = getBoardData(page[pid].board);
                if (blocks.length > 0) {
                    const conditions = blocks.map(b => `!api.${pid}.board.hasBlock(${b.x}, ${b.y})`);
                    code += `if (${conditions.join(' ||\n    ')}) {\n`;
                    code += `    isSuccess = false;\n`;
                    code += `}\n`;
                }
            });
            code += `if (!isSuccess) {\n    // Some blocks are missing\n}\n`;

        } else if (type === 'ghost') {
            playersToGen.forEach(pid => {
                const blocks = getBoardData(page[pid].board);
                code += `api.clearAllGhostBlocks(api.${pid});\n`;
                if (blocks.length > 0) {
                    blocks.forEach(b => {
                        code += `api.displayGhostBlock(api.${pid}, ${b.x}, ${b.y}, '${b.type}');\n`;
                    });
                }
            });

        } else if (type === 'place') {
            playersToGen.forEach(pid => {
                const blocks = getBoardData(page[pid].board);
                                if (blocks.length > 0) {
                    blocks.forEach(b => {
                        code += `api.${pid}.board.placeBlock(${b.x}, ${b.y}, '${b.type}');\n`;
                    });
                }
            });
        } else if (type === 'export') {
            const board = page.p1.board;
code = JSON.stringify(board, null, 2);
        } else if (type === 'analyze') {
            const visited = new Set();
            const board = page.p1.board;
            const detectedMinos = [];

            for (let y = 0; y < BOARD_HEIGHT; y++) {
                for (let x = 0; x < BOARD_WIDTH; x++) {
                    const cell = board[y][x];
                    const key = `${x},${y}`;
                    if (cell && cell !== 'G' && cell !== 'X' && !visited.has(key)) {
                        const group = [];
                        const queue = [{x, y}];
                        visited.add(key);
                        group.push({x, y});
                        
                        while (queue.length > 0) {
                             const current = queue.shift();
                             const dirs = [[0,1], [0,-1], [1,0], [-1,0]];
                             for (const [dx, dy] of dirs) {
                                 const nx = current.x + dx;
                                 const ny = current.y + dy;
                                 if (nx >= 0 && nx < BOARD_WIDTH && ny >= 0 && ny < BOARD_HEIGHT) {
                                     const nKey = `${nx},${ny}`;
                                     if (!visited.has(nKey) && board[ny][nx] === cell) {
                                         visited.add(nKey);
                                         group.push({x: nx, y: ny});
                                         queue.push({x: nx, y: ny});
                                     }
                                 }
                             }
                        }

                        if (group.length === 4) {
                            group.sort((a, b) => a.y - b.y || a.x - b.x);
                            const pivot = group[0];
                            const relStr = group.slice(1).map(b => `${b.x - pivot.x},${b.y - pivot.y}`).sort().join(';');
                            
                            const match = DRAW_SHAPE_MAP[relStr];
                            if (match) {
                                const finalX = pivot.x - match.offset[0];
                                const finalY = pivot.y - match.offset[1];
                                detectedMinos.push(`${match.type}:x${finalX},y${finalY},r${match.rot}`);
                            }
                        }
                    }
                }
            }
            code = detectedMinos.join('\n');
        }

        codeOutput.value = code.trim();

        currentGenType = type;

        codeGenTabs.querySelectorAll('.button').forEach(tab => {
            tab.classList.toggle('active', tab.dataset.type === type);
        });
    };

    genCodeBtn.addEventListener('click', () => {
        generateCode(currentGenType);
        modal.style.display = 'flex';
    });

    closeModalBtn.addEventListener('click', () => {
        modal.style.display = 'none';
    });

    window.addEventListener('click', (event) => {
        if (event.target === modal) {
            modal.style.display = 'none';
        }
    });

    copyCodeBtn.addEventListener('click', () => {
        if (!navigator.clipboard) {
            codeOutput.select();
            document.execCommand('copy');
            return;
        }
        navigator.clipboard.writeText(codeOutput.value).then(() => {
        }, (err) => {
        });
    });
codeGenTabs.querySelectorAll('.button').forEach(tab => {
        tab.addEventListener('click', () => {
            generateCode(tab.dataset.type);
        });
    });
window.addEventListener('resize', () => window.updateScale?.());
    setTimeout(() => window.updateScale?.(), 100);

});
