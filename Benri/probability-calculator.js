
        // --- Core Logic ---

        const MINOS = ['I', 'O', 'T', 'S', 'Z', 'J', 'L'];
        // 1. 全順列生成 (Heap's Algorithm)
        function getPermutations(arr) {
            let result = [];
            const swap = (a, i, j) => { [a[i], a[j]] = [a[j], a[i]]; };
            const generate = (n, heapArr) => {
                if (n === 1) {
                    result.push([...heapArr]);
                    return;
                }
                generate(n - 1, heapArr);
                for (let i = 0; i < n - 1; i++) {
                    swap(heapArr, (n % 2 === 0) ? i : 0, n - 1);
                    generate(n - 1, heapArr);
                }
            };
            generate(arr.length, [...arr]);
            return result;
        }

        // 2. ホールドを考慮した「可能な設置順序」の生成 (DFS)
        function getAchievableOrders(queue) {
            const results = new Set();
            function solve(queueIdx, hold, placed) {
                if (placed.length === 7) {
                    results.add(placed);
                    return;
                }
                if (queueIdx >= queue.length) {
                    if (hold) solve(queueIdx, null, placed + hold);
                    return;
                }
                const current = queue[queueIdx];
                // Action A: そのまま置く
                solve(queueIdx + 1, hold, placed + current);
                // Action B: ホールド操作
                if (hold === null) {
                    solve(queueIdx + 1, current, placed);
                } else {
                    solve(queueIdx + 1, current, placed + hold);
                }
            }
            solve(0, null, "");
            return results;
        }

        // 3. 条件式のパースと評価
        function createEvaluator(conditionStr) {
            try {
                let s = conditionStr.toUpperCase().replace(/\s+/g, '');
                if (!s) return () => true; 

                // ワイルドカード展開 (*>Z => I>Z and O>Z ...)
                // パターン1: *>X (X以外のすべてがXより先)
                s = s.replace(/\*>([IOTJLSZ])/g, (_, target) => {
                    const others = MINOS.filter(m => m !== target);
                    return '(' + others.map(m => `${m}>${target}`).join('&&') + ')';
                });

                // パターン2: X>* (XがX以外のすべてより先)
                s = s.replace(/([IOTJLSZ])>\*/g, (_, target) => {
                    const others = MINOS.filter(m => m !== target);
                    return '(' + others.map(m => `${target}>${m}`).join('&&') + ')';
                });

                s = s.replace(/AND/g, '&&').replace(/OR/g, '||');

                const regexChain = /([IOTJLSZ](?:>[IOTJLSZ])+)/g;
                const parsedScript = s.replace(regexChain, (match) => {
                    const parts = match.split('>');
                    let checks = [];
                    for(let i=0; i<parts.length-1; i++) {
                        checks.push(`idx('${parts[i]}') < idx('${parts[i+1]}')`);
                    }
                    return `(${checks.join(' && ')})`;
                });
                return new Function('order', `
                    const idx = (m) => {
                        const i = order.indexOf(m);
                        return i === -1 ? 999 : i;
                    };
                    try { return ${parsedScript}; } catch(e) { return false; }
                `);
            } catch (e) {
                console.error("Parse Error", e);
                return () => false;
            }
        }

        // --- UI Logic & Features ---

        const PRESETS = [
            { name: "はちみつ砲", cond: "I>L>S and O>J" },
            { name: "はちみつ砲(HD)", cond: "I>L>S and O>J and Z>T" },
            { name: "迷走砲", cond: "(L>S or L>I) and *>Z" },
            { name: "迷走砲(HD)", cond: "L>S and *>Z" },
            { name: "くろみつ砲", cond: "L>Z and (J>S>O or (S>O and S>J>T))" },
            { name: "くろみつ砲(HD)", cond: "L>Z and J>S>O" },
            { name: "PC-Spin(OkeyVersion)", cond: "J>S>I and L>O" },
            { name: "PC-Spin(OkeyVersion)(HD)", cond: "J>S>I and L>O and Z>T" },
            { name: "Riif積み v3", cond: "O>J and I>S and (L>S or L>Z)" },
            { name: "Riif積み v3(HD)", cond: "O>J and I>S and L>S and Z>T" },
            { name: "山岳積み2号", cond: "J>S and *>L" },
            { name: "山岳積み2号(HD)", cond: "J>S and *>L and Z>T" }
        ];

        function addSelectedPreset() {
            const select = document.getElementById('preset-select');
            const idx = select.value;
            if (idx === "") return;
            const p = PRESETS[idx];
            addTemplateRow(p.name, p.cond);
        }

        function addTemplateRow(name="", condition="", priority="") {
            const container = document.getElementById('template-list');
            const template = document.getElementById('row-template');
            const clone = template.content.cloneNode(true);
            
            if(name) clone.querySelector('.t-name').value = name;
            if(condition) clone.querySelector('.t-condition').value = condition;
            if(priority) {
                clone.querySelector('.p-input').value = priority;
            } else {
                // デフォルト: 最後の行の優先度 + 1 or 1
                const rows = container.querySelectorAll('.template-row');
                const lastRow = rows[rows.length - 1];
                let nextPrio = 1;
                if(lastRow) {
                    const val = parseInt(lastRow.querySelector('.p-input').value);
                    if(!isNaN(val)) nextPrio = val + 1;
                }
                clone.querySelector('.p-input').value = nextPrio;
            }

            container.appendChild(clone);
        }

        function removeRow(btn) {
            btn.closest('.template-row').remove();
        }

        // 機能: 複製
        function duplicateRow(btn) {
            const row = btn.closest('.template-row');
            const priority = row.querySelector('.p-input').value;
            const name = row.querySelector('.t-name').value;
            const condition = row.querySelector('.t-condition').value;

            addTemplateRow(name, condition, priority);
        }

        // 機能: 左右反転を追加
        // S⇔Z, L⇔J を入れ替える
        function addMirroredRow(btn) {
            const row = btn.closest('.template-row');
            const priority = row.querySelector('.p-input').value;
            const name = row.querySelector('.t-name').value;
            const condition = row.querySelector('.t-condition').value;
            // 名前変更
            const newName = name ? name + " (左右反転)" : "左右反転";

            // 条件式の文字置換 (S<->Z, L<->J)
            // 大文字小文字両対応
            const map = {
                'S': 'Z', 'Z': 'S',
                'L': 'J', 'J': 'L',
                's': 'z', 'z': 's',
                'l': 'j', 'j': 'l'
            };
            // 正規表現で SZJLszjl のいずれかにマッチさせ、マップで変換
            const newCondition = condition.replace(/[SZJLszjl]/g, (match) => map[match]);
            addTemplateRow(newName, newCondition, priority);
        }

        // 初期データ
        window.addEventListener('DOMContentLoaded', () => {
            // プリセットセレクトボックスの初期化
            const select = document.getElementById('preset-select');
            PRESETS.forEach((p, i) => {
                const opt = document.createElement('option');
                opt.value = i;
                opt.text = p.name;
                select.appendChild(opt);
            });

            // 例: DT砲とそのミラー
            addTemplateRow('DT砲', 'L>S and S>Z', 1);
            // addMirroredRow の動作デモのためにコメントアウトしてますが、手動でも追加できます
            // 例: 開幕TSD
            addTemplateRow('開幕TSD', '(I>J) or (Z>L>O)', 2);
        });

        async function calculateProbability() {
            const btn = document.querySelector('button[onclick="calculateProbability()"]');
            const btnText = document.getElementById('btn-text');
            const originalText = btnText.innerText;
            
            btnText.innerText = "計算中...";
            btn.classList.add('opacity-75', 'cursor-wait');
            btn.disabled = true;
            await new Promise(r => setTimeout(r, 50));

            try {
                const rows = Array.from(document.querySelectorAll('.template-row'));
                let templates = rows.map(row => ({
                    priority: parseInt(row.querySelector('.p-input').value) || 999,
                    name: row.querySelector('.t-name').value || "名無し",
                    conditionStr: row.querySelector('.t-condition').value,
                    checkFunc: createEvaluator(row.querySelector('.t-condition').value),
                    count: 0
                }));
                // 優先度順にソート (小さい順)
                // 優先度が同じ場合は、画面上の並び順(追加順)を維持するためにソートアルゴリズムの安定性に頼る
                templates.sort((a, b) => a.priority - b.priority);
                const permutations = getPermutations(MINOS);
                let totalSuccess = 0;
                let failCount = 0;
                let failedBags = []; // 失敗したツモ順を記録

                for (const bag of permutations) {
                    const possibleOrders = getAchievableOrders(bag);
                    let matched = false;

                    for (const tmpl of templates) {
                        let canBuild = false;
                        for (const order of possibleOrders) {
                            if (tmpl.checkFunc(order)) {
                                canBuild = true;
                                break;
                            }
                        }

                        if (canBuild) {
                            tmpl.count++;
                            totalSuccess++;
                            matched = true;
                            break; 
                        }
                    }

                    if (!matched) {
                        failCount++;
                        failedBags.push(bag.join(''));
                    }
                }

                const tbody = document.getElementById('result-body');
                tbody.innerHTML = '';
                
                templates.forEach(tmpl => {
                    const percent = ((tmpl.count / 5040) * 100).toFixed(2);
                    const tr = document.createElement('tr');
                    tr.className = "hover:bg-gray-50 transition";
                    tr.innerHTML = `
                        <td class="px-4 py-3 whitespace-nowrap text-sm text-gray-500 text-center border-r">${tmpl.priority}</td>
                        <td class="px-4 py-3 whitespace-nowrap text-sm font-medium text-gray-900">${escapeHtml(tmpl.name)}</td>
                        <td class="px-4 py-3 whitespace-nowrap text-sm text-gray-500 font-mono hidden md:table-cell">${escapeHtml(tmpl.conditionStr)}</td>
                        <td class="px-4 py-3 whitespace-nowrap text-sm text-right text-gray-500">${tmpl.count}</td>
                        <td class="px-4 py-3 whitespace-nowrap text-sm text-right font-bold text-gray-800">${percent}%</td>
                    `;
                    tbody.appendChild(tr);
                });

                document.getElementById('total-count').innerText = totalSuccess;
                document.getElementById('total-percent').innerText = ((totalSuccess / 5040) * 100).toFixed(2) + '%';
                
                document.getElementById('fail-count').innerText = failCount;
                document.getElementById('fail-percent').innerText = ((failCount / 5040) * 100).toFixed(2) + '%';

                document.getElementById('failed-sequences').value = failedBags.join('\n');

                document.getElementById('result-section').classList.remove('hidden');
            } catch (e) {
                alert("エラーが発生しました: " + e.message);
                console.error(e);
            } finally {
                btn.disabled = false;
                btn.classList.remove('opacity-75', 'cursor-wait');
                btnText.innerText = originalText;
            }
        }

        function escapeHtml(text) {
            if(!text) return "";
            return text
                .replace(/&/g, "&amp;")
                .replace(/</g, "&lt;")
                .replace(/>/g, "&gt;")
                .replace(/"/g, "&quot;")
                .replace(/'/g, "&#039;");
        }
    

