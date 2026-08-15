const $ = id => document.getElementById(id);
const video = $("video");
const canvas = $("frame");
const ctx = canvas.getContext("2d", { willReadFrequently: true });
const RELEASE = "review-v1";
const CELL_NAMES = ["", "I", "L", "O", "Z", "T", "J", "S", "G"];
const PIECES = ["I", "L", "O", "Z", "T", "J", "S"];
const state = {
  file: null, module: null, session: null, running: false, cancel: false,
  framePtr: 0, frameBytes: 0, holdPtr: 0, nextPtr: 0, colorsPtr: 0,
  featurePtr: 0, labelPtr: 0, inputPath: "", outputDir: "", sourceName: "",
  review: null, player: 1, phase: 0, selectedCandidate: -1, candidates: [],
  requiredCells: new Set(), placementCells: new Set(), garbage: { override: false, lines: 0, masks: [] },
  queuePlayer: 1, queueSelected: new Set(), queueCurrent: 0
};

function log(message) { $("log").textContent += "\n" + message; $("log").scrollTop = $("log").scrollHeight; }
function status(message, progress) {
  $("status").textContent = message;
  if (progress !== undefined) {
    const value = Math.max(0, Math.min(100, progress));
    $("progress-bar").style.width = value + "%";
    $("percent").textContent = Math.round(value) + "%";
  }
}
function wasmString(pointer) { return state.module.UTF8ToString(pointer); }
function wasmError() { return wasmString(state.module._tr_last_error()) || "WASM処理に失敗しました"; }
function wasmCString(value) {
  const module = state.module;
  const size = module.lengthBytesUTF8(value) + 1;
  const pointer = module._malloc(size);
  module.stringToUTF8(value, pointer, size);
  return pointer;
}
function cancelled() { if (state.cancel) throw new Error("解析をキャンセルしました"); }
function frameSize() { return video.videoWidth * video.videoHeight * 4; }
function yieldToBrowser() { return new Promise(resolve => setTimeout(resolve, 0)); }
function cellCode(value) { return typeof value === "number" ? value : Math.max(0, CELL_NAMES.indexOf(value || "")); }
function cellName(value) { return CELL_NAMES[cellCode(value)] || ""; }
function escapeHtml(value) { return String(value).replace(/[&<>\"']/g, ch => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;", "'":"&#39;"}[ch])); }
function boardCell(board, x, y) { return board && board.length > y * 10 + x ? board[y * 10 + x] : "_"; }

async function loadWasm() {
  if (state.module) return state.module;
  const loaded = await import("./tetris_recovery.js?rev=" + RELEASE);
  state.module = await loaded.default({ locateFile: name => new URL(name + "?rev=" + RELEASE, import.meta.url).href });
  return state.module;
}

async function loadModel() {
  if (state.session) return state.session;
  $("model-status").textContent = "ONNX: 読み込み中";
  ort.env.wasm.numThreads = 1;
  ort.env.wasm.wasmPaths = "https://cdn.jsdelivr.net/npm/onnxruntime-web/dist/";
  ort.env.logLevel = "fatal";
  const url = new URL("./assets/tetris.onnx", import.meta.url).href;
  state.session = await ort.InferenceSession.create(url, { executionProviders: ["wasm"] });
  $("model-status").textContent = "ONNX: 読み込み完了";
  return state.session;
}

async function loadSettings(module) {
  const response = await fetch(new URL("../config/tetris_recover.ini", import.meta.url));
  if (!response.ok) throw new Error("設定ファイルを読み込めません: " + response.status);
  module.FS.writeFile("/settings.ini", new TextEncoder().encode(await response.text()));
}

function allocateBuffers(module) {
  const bytes = frameSize();
  if (state.frameBytes === bytes) return;
  for (const key of ["framePtr", "holdPtr", "nextPtr", "colorsPtr", "featurePtr", "labelPtr"]) {
    if (state[key]) module._free(state[key]);
  }
  state.frameBytes = bytes;
  state.framePtr = module._malloc(bytes);
  state.holdPtr = module._malloc(4);
  state.nextPtr = module._malloc(5 * 4);
  state.colorsPtr = module._malloc(6 * 3);
  state.featurePtr = module._malloc(200 * 63 * 4);
  state.labelPtr = module._malloc(200);
}

async function seek(time) {
  const target = Math.max(0, Math.min(video.duration || 0, time));
  if (Math.abs(video.currentTime - target) < 0.00001) return;
  await new Promise((resolve, reject) => {
    const done = () => { cleanup(); resolve(); };
    const fail = () => { cleanup(); reject(new Error("動画フレームのシークに失敗しました")); };
    const cleanup = () => { video.removeEventListener("seeked", done); video.removeEventListener("error", fail); };
    video.addEventListener("seeked", done, { once: true });
    video.addEventListener("error", fail, { once: true });
    video.currentTime = target;
  });
}
function readFrame() {
  ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
  return ctx.getImageData(0, 0, canvas.width, canvas.height).data;
}
function nextDecodedFrame() {
  if (video.ended) return Promise.resolve(null);
  if (typeof video.requestVideoFrameCallback === "function") {
    return new Promise(resolve => {
      let finished = false;
      let onEnded;
      const finish = value => { if (finished) return; finished = true; video.removeEventListener("ended", onEnded); resolve(value); };
      onEnded = () => finish(null);
      video.addEventListener("ended", onEnded, { once: true });
      video.requestVideoFrameCallback((_, metadata) => finish(metadata.mediaTime));
    });
  }
  return new Promise(resolve => requestAnimationFrame(() => resolve(video.currentTime)));
}

function submitQueue(player, rgba, time) {
  const module = state.module;
  module.HEAPU8.set(rgba, state.framePtr);
  const returned = module._tr_queue_observe_and_add(player, state.framePtr, rgba.byteLength, time, state.holdPtr, state.nextPtr, 5, state.colorsPtr, 18);
  if (!returned) throw new Error(wasmError());
  const count = returned - 1;
  const next = [];
  for (let i = 0; i < count; i++) next.push(module.HEAP32[(state.nextPtr >> 2) + i]);
  return { hold: module.HEAP32[state.holdPtr >> 2], next };
}
function uploadFrame(rgba) {
  const module = state.module;
  module.HEAPU8.set(rgba, state.framePtr);
  if (!module._tr_frame_upload(state.framePtr, rgba.byteLength)) throw new Error(wasmError());
}
async function playFromStart() { video.pause(); await seek(0); video.muted = true; await video.play(); }

async function queuePass(duration, interval) {
  let nextScan = 0, lastFrame = null, frames = 0;
  await playFromStart();
  for (;;) {
    cancelled();
    const mediaTime = await nextDecodedFrame();
    if (mediaTime === null) break;
    lastFrame = readFrame(); frames++;
    let uploaded = false;
    while (nextScan <= mediaTime + 0.000001 && nextScan < duration) {
      if (!uploaded) { uploadFrame(lastFrame); uploaded = true; }
      submitQueue(1, lastFrame, nextScan); submitQueue(2, lastFrame, nextScan); nextScan += interval;
    }
    status("Pass 1/3: キュー走査 " + frames + "フレーム", duration ? nextScan / duration * 35 : 0);
    if (mediaTime >= duration - 0.0001) break;
  }
  video.pause();
  if (!lastFrame) throw new Error("動画からフレームを取得できませんでした");
  while (nextScan < duration) { submitQueue(1, lastFrame, nextScan); submitQueue(2, lastFrame, nextScan); nextScan += interval; }
  status("Pass 1/3: C++キュー復元", 38);
  return frames;
}

function classLabels(output) {
  const labels = new Uint8Array(200), data = output.data, dims = output.dims || [];
  if (dims.length === 2 && dims[0] === 200 && dims[1] > 1) {
    const classes = dims[1];
    for (let cell = 0; cell < 200; cell++) {
      let best = 0, bestValue = Number(data[cell * classes]);
      for (let c = 1; c < classes; c++) { const value = Number(data[cell * classes + c]); if (value > bestValue) { bestValue = value; best = c; } }
      labels[cell] = best;
    }
  } else for (let i = 0; i < 200; i++) labels[i] = Number(data[i]) || 0;
  return labels;
}

async function boardPass(requests, duration) {
  const module = state.module, session = state.session, inputName = session.inputNames[0], outputName = session.outputNames[0];
  const runRequest = async (request, rgba, alreadyUploaded) => {
    if (!alreadyUploaded) uploadFrame(rgba);
    if (!module._tr_board_features(request.player, state.framePtr, rgba.byteLength, state.featurePtr, 200 * 63)) throw new Error(wasmError());
    const features = new Float32Array(module.HEAPF32.buffer, state.featurePtr, 200 * 63).slice();
    const result = await session.run({ [inputName]: new ort.Tensor("float32", features, [200, 63]) }, [outputName]);
    module.HEAPU8.set(classLabels(result[outputName]), state.labelPtr);
    if (!module._tr_board_finish(request.player, state.framePtr, rgba.byteLength, request.time, state.labelPtr, 200)) throw new Error(wasmError());
  };
  let index = 0, frameCount = 0, lastFrame = null;
  await playFromStart();
  while (index < requests.length) {
    cancelled();
    const mediaTime = await nextDecodedFrame();
    if (mediaTime === null) break;
    const rgba = readFrame(); lastFrame = rgba; frameCount++;
    let uploaded = false;
    while (index < requests.length && requests[index].time <= mediaTime + 0.000001) {
      await runRequest(requests[index++], rgba, uploaded); uploaded = true;
      status("Pass 2/3: 盤面ONNX " + index + "/" + requests.length, 40 + index / Math.max(1, requests.length) * 42);
      await yieldToBrowser();
    }
    if (mediaTime >= duration - 0.0001) break;
  }
  video.pause();
  if (index < requests.length) {
    if (!lastFrame) throw new Error("末尾フレームを取得できませんでした");
    uploadFrame(lastFrame);
    while (index < requests.length) {
      await runRequest(requests[index++], lastFrame, true);
      status("Pass 2/3: 末尾フレームで盤面補完 " + index + "/" + requests.length, 40 + index / Math.max(1, requests.length) * 42);
      await yieldToBrowser();
    }
  }
}

function cellClass(value) {
  const name = cellName(value);
  return name === "" ? "empty" : name === "G" ? "garbage" : "piece-" + name;
}
function renderBoard(id, board, mode) {
  const element = $(id); element.replaceChildren();
  for (let y = 20; y < 40; y++) for (let x = 0; x < 10; x++) {
    const cell = document.createElement("button");
    const fullY = y; const key = x + ":" + fullY; const value = boardCell(board, x, y);
    cell.className = "cell " + cellClass(value);
    cell.type = "button"; cell.title = x + "," + (y - 20) + " = " + (value === "_" ? "empty" : value);
    if (mode === "required" && state.requiredCells.has(key)) cell.classList.add("marked");
    if (mode === "placement" && state.placementCells.has(key)) cell.classList.add("placement");
    if (mode === "garbage" && garbageCellMarked(x, y)) cell.classList.add("marked");
    cell.addEventListener("click", () => boardClick(mode, x, y));
    element.append(cell);
  }
}
function garbageCellMarked(x, y) {
  if (!state.garbage.override || y < 40 - state.garbage.lines) return false;
  const row = y - (40 - state.garbage.lines);
  return !!(state.garbage.masks[row] & (1 << x));
}
function boardClick(mode, x, y) {
  const key = x + ":" + y;
  if (mode === "required") state.requiredCells.has(key) ? state.requiredCells.delete(key) : state.requiredCells.add(key);
  if (mode === "placement") {
    if (state.placementCells.has(key)) state.placementCells.delete(key);
    else if (state.placementCells.size < 4) state.placementCells.add(key);
    else return;
  }
  if (mode === "garbage" && !state.garbage.override) {
    const automatic = state.candidates[0]?.garbage || currentPhase()?.garbage || { lines: 0, holeMasks: [] };
    state.garbage.override = true;
    state.garbage.lines = automatic.lines || 0;
    state.garbage.masks = (automatic.holeMasks || []).slice();
  }
  if (mode === "garbage" && state.garbage.override && y >= 40 - state.garbage.lines) {
    const row = y - (40 - state.garbage.lines); state.garbage.masks[row] = 1 << x;
  }
  renderReview();
}

function currentPlayerData() { return state.review?.["p" + state.player] || { raw: [], solved: [], queue: [], originalQueue: [] }; }
function currentRaw() { return currentPlayerData().raw; }
function currentSolved() { return currentPlayerData().solved; }
function currentPhase() { return currentRaw()[state.phase]; }
function laterManualCorrectionExists() { return currentRaw().some((row, index) => index > state.phase && row.manual); }
function visibleCandidates() {
  return state.candidates.filter(candidate => {
    for (const key of state.requiredCells) {
      const [x, y] = key.split(":").map(Number); if (!"ILZO TJS".replace(" ", "").includes(boardCell(candidate.board, x, y))) return false;
    }
    for (const key of state.placementCells) {
      const [x, y] = key.split(":").map(Number);
      if (!candidate.cells.some(cell => cell[0] === x && cell[1] === y)) return false;
    }
    return true;
  });
}
function candidateMasks() {
  const masks = new Uint16Array(state.garbage.masks.slice(0, state.garbage.lines));
  return masks;
}
function loadCandidates() {
  if (state.phase === 0) { state.candidates = []; state.selectedCandidate = -1; renderReview(); return; }
  const masks = candidateMasks(), ptr = masks.length ? state.module._malloc(masks.length * 2) : 0;
  if (ptr) state.module.HEAPU16.set(masks, ptr >> 1);
  const json = wasmString(state.module._tr_review_candidates(state.player, state.phase, state.garbage.override ? 1 : 0, state.garbage.lines, ptr, masks.length));
  if (ptr) state.module._free(ptr);
  state.candidates = JSON.parse(json);
  const visible = visibleCandidates();
  if (!visible.some(item => item.index === state.selectedCandidate)) state.selectedCandidate = visible.length ? visible[0].index : -1;
  renderReview();
}

function renderPhaseList() {
  const raw = currentRaw(); const list = $("phase-list"); list.replaceChildren();
  $("phase-count").textContent = raw.length + "局面";
  raw.forEach((row, index) => {
    const button = document.createElement("button"); button.type = "button";
    button.className = "phase-item" + (index === state.phase ? " active" : "") + (row.manual ? " manual" : "");
    button.textContent = `#${String(index).padStart(2, "0")}  ${Number(row.time).toFixed(3)}s  ${row.piece || "—"} / ${row.action || "phase"}${row.queueManual ? " [キュー修正]" : ""}${row.manual ? " [盤面修正]" : ""}`;
    button.addEventListener("click", () => { state.phase = index; state.requiredCells.clear(); state.placementCells.clear(); state.garbage = { override: false, lines: 0, masks: [] }; seekVideo(row.time, false); loadCandidates(); });
    list.append(button);
  });
}
function renderCandidateList() {
  const list = $("candidate-list"); list.replaceChildren();
  const visible = visibleCandidates();
  $("candidate-info").textContent = visible.length + " / " + state.candidates.length + "候補";
  for (const candidate of visible) {
    const button = document.createElement("button"); button.type = "button";
    button.className = "candidate-button" + (candidate.index === state.selectedCandidate ? " selected" : "");
    button.innerHTML = `<strong>#${candidate.index + 1} ${escapeHtml(candidate.piece || "—")}</strong><div class="candidate-meta">r${candidate.rotation} / x${candidate.x} / y${candidate.y}<br>消去 ${candidate.clearedLines} / score ${Number(candidate.score).toFixed(0)}</div>`;
    button.addEventListener("click", () => { state.selectedCandidate = candidate.index; renderReview(); });
    list.append(button);
  }
  $("apply-candidate").disabled = state.phase === 0 || state.selectedCandidate < 0;
}
function renderGarbage() {
  const row = currentPhase(); const automatic = state.candidates[0]?.garbage || row?.garbage || { lines: 0, holeMasks: [] };
  if (!state.garbage.override) { state.garbage.lines = automatic.lines || 0; state.garbage.masks = (automatic.holeMasks || []).slice(); }
  $("garbage-lines").value = state.garbage.lines;
  $("garbage-info").textContent = state.garbage.override ? "手動指定中。各行の穴をクリックすると、その行の穴が1つに固定されます。" : `自動検出: ${state.garbage.lines}行。必要なら手動指定へ切り替えられます。`;
  renderBoard("garbage-board", row?.observed || "", "garbage");
}
function renderReview() {
  if (!state.review) return;
  const raw = currentRaw(), solved = currentSolved(), row = raw[state.phase] || {}, previous = solved[Math.max(0, state.phase - 1)] || row;
  $("review").classList.remove("hidden");
  $("review-summary").textContent = `${state.player === 1 ? "P1" : "P2"} / ${raw.length}局面。黄色は手動修正、青い候補を選んで再探索します。`;
  $("phase-label").textContent = `局面 #${state.phase}  ${Number(row.time || 0).toFixed(3)}秒  active=${row.piece || "—"}  Hold=${row.hold || "—"}  NEXT=${row.next || "—"}`;
  $("player-p1").classList.toggle("active", state.player === 1); $("player-p2").classList.toggle("active", state.player === 2);
  renderPhaseList();
  renderBoard("previous-board", previous.board || previous.observed || "", "placement");
  renderBoard("observed-board", row.observed || "", "required");
  const selected = state.candidates.find(item => item.index === state.selectedCandidate);
  renderBoard("candidate-board", selected?.board || row.board || row.observed || "", "none");
  renderCandidateList(); renderGarbage();
  const filters = [];
  if (state.requiredCells.size) filters.push("必須セル " + state.requiredCells.size);
  if (state.placementCells.size) filters.push("配置セル " + state.placementCells.size + "/4");
  $("filter-info").textContent = filters.length ? filters.join(" / ") : "フィルタなし。候補は本家と同じ originalLegalMoves の全候補です。";
  $("restore-automatic").disabled = state.phase === 0;
  renderQueue();
}

function fillPieceSelect(select, value, includeGarbage = false) {
  select.replaceChildren();
  for (const name of ["", ...PIECES, ...(includeGarbage ? ["G"] : [])]) { const option = document.createElement("option"); option.value = name; option.textContent = name || "空"; select.append(option); }
  select.value = value || "";
}
function fillQueueControls(sample) {
  fillPieceSelect($("queue-active"), sample?.active || ""); fillPieceSelect($("queue-hold"), sample?.hold || "");
  const next = sample?.next || "";
  for (let i = 0; i < 5; i++) fillPieceSelect($("queue-next-" + (i + 1)), next[i] || "");
}
function queueSamples() { return currentPlayerData().queue || []; }
function selectedQueueIndices() { return state.queueSelected.size ? [...state.queueSelected].sort((a, b) => a - b) : [state.queueCurrent]; }
function queueHistoryText(indices) {
  return indices.map(index => { const sample = queueSamples()[index]; if (!sample) return ""; return `* ${Number(sample.time).toFixed(3)} s  現在=${sample.active || "—"}  Hold=${sample.hold || "—"}  認識Next=${sample.next || "—"}${sample.decodedNext ? "  復元Next=" + sample.decodedNext : ""}${sample.sequenceCorrected ? "  [補正:全履歴/7-bag]" : ""}${sample.rejected ? "  [除外]" : ""}${sample.manual ? "  [手動]" : ""}`; }).join("\n");
}
function renderQueue() {
  const samples = queueSamples(), list = $("queue-list");
  list.innerHTML = samples.map((sample, index) => `<label class="queue-row${state.queueSelected.has(index) ? " selected" : ""}${sample.manual ? " manual" : ""}" data-index="${index}"><input type="checkbox" ${state.queueSelected.has(index) ? "checked" : ""}><span>${Number(sample.time).toFixed(3)}s</span><span>現:${escapeHtml(sample.active || "—")} Hold:${escapeHtml(sample.hold || "—")}</span><span class="queue-hold">Next:${escapeHtml(sample.next || "—")}</span><span class="queue-next">復元:${escapeHtml(sample.decodedNext || "—")}${sample.sequenceCorrected ? " · 補正" : ""}${sample.rejected ? " · 除外" : ""}</span></label>`).join("");
  for (const row of list.querySelectorAll(".queue-row")) {
    const index = Number(row.dataset.index); const checkbox = row.querySelector("input");
    checkbox.addEventListener("click", event => { event.stopPropagation(); checkbox.checked ? state.queueSelected.add(index) : state.queueSelected.delete(index); row.classList.toggle("selected", checkbox.checked); state.queueCurrent = index; fillQueueControls(samples[index]); seekVideo(samples[index].time, false); });
    row.addEventListener("click", () => { state.queueCurrent = index; state.queueSelected.add(index); fillQueueControls(samples[index]); seekVideo(samples[index].time, false); renderQueue(); });
  }
  if (samples[state.queueCurrent]) fillQueueControls(samples[state.queueCurrent]);
  $("queue-info").textContent = `${samples.length}件。${state.queueSelected.size || 1}行を選択中。複数行に同じ現在ミノ/Hold/NEXTを適用できます。`;
}
function queueValues() {
  const active = cellCode($("queue-active").value), hold = cellCode($("queue-hold").value), next = [];
  let blank = false;
  for (let i = 1; i <= 5; i++) { const value = $("queue-next-" + i).value; if (!value) blank = true; else { if (blank) throw new Error("NEXTは先頭から連続して入力してください"); next.push(cellCode(value)); } }
  if (next.length < 3) throw new Error("NEXTは3個以上入力してください");
  return { active, hold, next };
}
function refreshAfterWasm() { state.review = JSON.parse(wasmString(state.module._tr_review_snapshot())); state.phase = Math.min(state.phase, Math.max(0, currentRaw().length - 1)); state.selectedCandidate = -1; renderReview(); loadCandidates(); }

function applyQueueEdit(showAlert = true) {
  try {
    const values = queueValues();
    const nextPtr = state.module._malloc(values.next.length * 4);
    state.module.HEAP32.set(values.next, nextPtr >> 2);
    for (const index of selectedQueueIndices()) if (!state.module._tr_review_queue_edit(state.queuePlayer, index, values.active, values.hold, nextPtr, values.next.length)) { state.module._free(nextPtr); throw new Error(wasmError()); }
    state.module._free(nextPtr);
    refreshAfterWasm();
    log("キューログの選択行へ現在ミノ/Hold/NEXTを適用しました");
  } catch (error) { if (showAlert) alert(error.message || String(error)); else throw error; }
}
async function reanalyzeQueue() {
  try {
    applyQueueEdit(false);
    if (!confirm("修正した生ログからキュー安定化・局面・合法手を最初から再計算します。現在の盤面固定修正は再計算結果に置き換わります。続けますか？")) return;
    status("キューログを反映して再解析中…", 88); await yieldToBrowser();
    if (!state.module._tr_review_reanalyze()) throw new Error(wasmError());
    refreshAfterWasm(); log("修正済みキューログから局面と合法候補を再解析しました"); status("レビュー可能な解析結果", 100);
  } catch (error) { alert(error.message || String(error)); }
}

function makeDownload(module, path, filename, type) {
  if (!path) return;
  const bytes = module.FS.readFile(path); const url = URL.createObjectURL(new Blob([bytes], { type: type || "application/octet-stream" }));
  const link = document.createElement("a"); link.href = url; link.download = filename; link.textContent = filename; $("downloads").append(link);
}
function publishOutput(module) {
  const jsonPath = wasmString(module._tr_output_path(0)); const parsed = JSON.parse(new TextDecoder().decode(module.FS.readFile(jsonPath)));
  const links = $("links"); links.replaceChildren();
  [[1, "P1シミュレータ"], [2, "P2シミュレータ"], [3, "2Pシミュレータ"]].forEach(([kind, label]) => { const a = document.createElement("a"); a.href = wasmString(module._tr_output_url(kind)); a.target = "_blank"; a.rel = "noopener"; a.textContent = label + "を開く"; links.append(a); });
  $("downloads").replaceChildren();
  [[0, state.sourceName + "_tetris_recovered.json", "application/json"], [4, state.sourceName + "_links.html", "text/html"], [5, state.sourceName + "_report.html", "text/html"], [6, "training-annotation.json", "application/json"], [7, "training-manifest.json", "application/json"], [8, state.sourceName + "_training.mp4", "video/mp4"]].forEach(([kind, name, type]) => makeDownload(module, wasmString(module._tr_output_path(kind)), name, type));
  $("counts").textContent = `P1 ${parsed.p1?.length || 0} phases / P2 ${parsed.p2?.length || 0} phases`;
  log("本家互換JSON・Simulator URL・レポート・学習アノテーション・マニフェスト・元動画を保存可能にしました");
}
function exportApprovedResult() {
  try {
    const inputPtr = wasmCString(state.inputPath), outputPtr = wasmCString(state.outputDir);
    const written = state.module._tr_write_output(inputPtr, outputPtr); state.module._free(inputPtr); state.module._free(outputPtr);
    if (!written) throw new Error(wasmError());
    publishOutput(state.module); status("承認済み出力を保存しました", 100);
  } catch (error) { status(error.message || String(error), 0); log("ERROR: " + (error.stack || error)); }
}

async function analyze() {
  if (!state.file || state.running) return;
  state.running = true; state.cancel = false; $("run").disabled = true; $("cancel").disabled = false; $("review").classList.add("hidden"); $("links").replaceChildren(); $("downloads").replaceChildren(); $("log").textContent = "開始";
  try {
    const module = await loadWasm(); await loadModel(); await loadSettings(module);
    canvas.width = video.videoWidth; canvas.height = video.videoHeight; allocateBuffers(module);
    const settingsPtr = wasmCString("/settings.ini"); if (!module._tr_runtime_init(video.videoWidth, video.videoHeight, settingsPtr)) throw new Error(wasmError()); module._free(settingsPtr);
    const duration = video.duration; module._tr_runtime_reset(duration);
    const bytes = new Uint8Array(await state.file.arrayBuffer()); state.sourceName = state.file.name.replace(/[\\/]/g, "_").replace(/\.[^.]*$/, "") || "video";
    const safeName = state.file.name.replace(/[\\/]/g, "_") || "video.mp4"; state.inputPath = "/input/" + safeName; state.outputDir = "/output/" + state.sourceName;
    try { module.FS.mkdir("/input"); } catch (_) {} try { module.FS.mkdir("/output"); } catch (_) {} try { module.FS.mkdir(state.outputDir); } catch (_) {}
    module.FS.writeFile(state.inputPath, bytes); log(`動画 ${video.videoWidth}x${video.videoHeight}, ${duration.toFixed(3)}秒`);
    await queuePass(duration, module._tr_sample_interval());
    const capacity = Math.ceil(duration / Math.max(module._tr_sample_interval(), 0.001)) * 5 + 32, requestPtr = module._malloc(capacity * 8);
    const p1Count = module._tr_prepare_board_requests(1, requestPtr, capacity), p1Times = Array.from(new Float64Array(module.HEAPF64.buffer, requestPtr, p1Count));
    const p2Count = module._tr_prepare_board_requests(2, requestPtr, capacity), p2Times = Array.from(new Float64Array(module.HEAPF64.buffer, requestPtr, p2Count)); module._free(requestPtr);
    const requests = p1Times.map(time => ({ player: 1, time })).concat(p2Times.map(time => ({ player: 2, time }))).sort((a, b) => a.time - b.time || a.player - b.player);
    log(`C++が生成した盤面要求: P1 ${p1Count} / P2 ${p2Count}`); await boardPass(requests, duration);
    status("Pass 3/3: C++合法手ビーム探索", 86); await yieldToBrowser(); if (!module._tr_recover()) throw new Error(wasmError()); await yieldToBrowser();
    refreshAfterWasm(); status("解析完了。レビューで修正してから出力してください", 100); log("解析完了。誤ルート・NEXT・ガベージを修正して承認済み出力を保存できます");
  } catch (error) { status(error.message || String(error), 0); log("ERROR: " + (error.stack || error)); }
  finally { state.running = false; $("run").disabled = !state.file; $("cancel").disabled = true; }
}

function seekVideo(seconds, play) { const target = Math.max(0, Math.min(video.duration || 0, seconds)); video.currentTime = target; if (play) video.play().catch(() => {}); else video.pause(); }
function setupEvents() {
  $("video-file").addEventListener("change", () => { const file = $("video-file").files?.[0]; if (!file) return; state.file = file; video.src = URL.createObjectURL(file); video.load(); $("file-name").textContent = `${file.name} (${(file.size / 1024 / 1024).toFixed(1)} MB)`; $("run").disabled = false; status("動画のメタデータを読み込みました", 0); $("log").textContent = "待機中"; });
  video.addEventListener("loadedmetadata", () => { $("duration").textContent = `${video.videoWidth}×${video.videoHeight} / ${video.duration.toFixed(3)}秒`; });
  $("run").addEventListener("click", analyze); $("cancel").addEventListener("click", () => { state.cancel = true; status("キャンセル処理中…"); });
  $("player-p1").addEventListener("click", () => { if (state.review?.p1.raw.length) { state.player = 1; state.phase = 0; state.requiredCells.clear(); state.placementCells.clear(); state.garbage = { override: false, lines: 0, masks: [] }; loadCandidates(); } });
  $("player-p2").addEventListener("click", () => { if (state.review?.p2.raw.length) { state.player = 2; state.phase = 0; state.requiredCells.clear(); state.placementCells.clear(); state.garbage = { override: false, lines: 0, masks: [] }; loadCandidates(); } });
  $("clear-filter").addEventListener("click", () => { state.requiredCells.clear(); state.placementCells.clear(); loadCandidates(); });
  $("apply-candidate").addEventListener("click", () => { try { if (laterManualCorrectionExists() && !confirm("この局面を修正すると、後ろの手動修正は解除されます。続けますか？")) return; const masks = candidateMasks(), ptr = masks.length ? state.module._malloc(masks.length * 2) : 0; if (ptr) state.module.HEAPU16.set(masks, ptr >> 1); const ok = state.module._tr_review_apply_candidate(state.player, state.phase, state.selectedCandidate, state.garbage.override ? 1 : 0, state.garbage.lines, ptr, masks.length); if (ptr) state.module._free(ptr); if (!ok) throw new Error(wasmError()); refreshAfterWasm(); log(`${state.player === 1 ? "P1" : "P2"} 局面 #${state.phase} を候補で固定し、以降を再探索しました`); } catch (error) { alert(error.message || String(error)); } });
  $("restore-automatic").addEventListener("click", () => { if (!confirm("この局面以降の手動修正を解除して、自動ビーム探索へ戻しますか？")) return; if (!state.module._tr_review_restore_automatic(state.player, state.phase)) { alert(wasmError()); return; } refreshAfterWasm(); });
  $("garbage-auto").addEventListener("click", () => { state.garbage = { override: false, lines: 0, masks: [] }; loadCandidates(); });
  $("garbage-lines").addEventListener("change", () => { const lines = Math.max(0, Math.min(20, Number($("garbage-lines").value) || 0)); state.garbage.override = true; state.garbage.lines = lines; state.garbage.masks = Array.from({ length: lines }, (_, i) => state.garbage.masks[i] || 1 << 4); loadCandidates(); });
  $("video-back").addEventListener("click", () => seekVideo(video.currentTime - .5, false)); $("video-forward").addEventListener("click", () => seekVideo(video.currentTime + .5, false));
  $("video-play").addEventListener("click", () => video.paused ? video.play().catch(() => {}) : video.pause());
  $("video-phase").addEventListener("click", () => { const row = currentPhase(); if (row) seekVideo(Math.max(0, Number(row.start) - .75), true); });
  $("queue-p1").addEventListener("click", () => { state.queuePlayer = 1; state.queueSelected.clear(); state.queueCurrent = 0; $("queue-p1").classList.add("active"); $("queue-p2").classList.remove("active"); renderQueue(); });
  $("queue-p2").addEventListener("click", () => { state.queuePlayer = 2; state.queueSelected.clear(); state.queueCurrent = 0; $("queue-p2").classList.add("active"); $("queue-p1").classList.remove("active"); renderQueue(); });
  $("queue-apply").addEventListener("click", () => applyQueueEdit()); $("queue-reanalyze").addEventListener("click", reanalyzeQueue);
  $("queue-restore").addEventListener("click", () => { for (const index of selectedQueueIndices()) if (!state.module._tr_review_queue_restore(state.queuePlayer, index)) { alert(wasmError()); return; } refreshAfterWasm(); });
  $("queue-copy-selected").addEventListener("click", () => copyText(queueHistoryText(selectedQueueIndices()), "選択行のNEXT履歴"));
  $("queue-copy-all").addEventListener("click", () => copyText(queueHistoryText(queueSamples().map((_, i) => i)), "全NEXT履歴"));
  $("export").addEventListener("click", exportApprovedResult);
}
async function copyText(value, label) {
  try { await navigator.clipboard.writeText(value); log(label + "をコピーしました"); }
  catch (_) { alert("クリップボードへコピーできませんでした。HTTPS上で実行してください。"); }
}
setupEvents();
