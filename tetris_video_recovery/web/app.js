const $ = id => document.getElementById(id);
const video = $("video");
const canvas = $("frame");
const ctx = canvas.getContext("2d", { willReadFrequently: true });
const RELEASE = "0c05973";
const state = { file: null, module: null, session: null, running: false, cancel: false, framePtr: 0, frameBytes: 0, holdPtr: 0, nextPtr: 0, colorsPtr: 0, featurePtr: 0, labelPtr: 0 };

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
  const url = new URL("../config/tetris_recover.ini", import.meta.url);
  const response = await fetch(url);
  if (!response.ok) throw new Error("設定ファイルを読み込めませんでした: " + response.status);
  module.FS.writeFile("/settings.ini", new TextEncoder().encode(await response.text()));
}

function allocateBuffers(module) {
  const bytes = frameSize();
  if (state.frameBytes === bytes) return;
  if (state.framePtr) module._free(state.framePtr);
  if (state.holdPtr) module._free(state.holdPtr);
  if (state.nextPtr) module._free(state.nextPtr);
  if (state.colorsPtr) module._free(state.colorsPtr);
  if (state.featurePtr) module._free(state.featurePtr);
  if (state.labelPtr) module._free(state.labelPtr);
  state.frameBytes = bytes;
  state.framePtr = module._malloc(bytes);
  state.holdPtr = module._malloc(4);
  state.nextPtr = module._malloc(5 * 4);
  state.colorsPtr = module._malloc(6 * 3);
  state.featurePtr = module._malloc(200 * 63 * 4);
  state.labelPtr = module._malloc(200);
}

async function seek(time) {
  const target = Math.max(0, Math.min(video.duration, time));
  if (Math.abs(video.currentTime - target) < 0.00001) return;
  await new Promise((resolve, reject) => {
    const cleanup = () => { video.removeEventListener("seeked", done); video.removeEventListener("error", fail); };
    const done = () => { cleanup(); resolve(); };
    const fail = () => { cleanup(); reject(new Error("動画フレームのシークに失敗しました")); };
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
      const finish = value => {
        if (finished) return;
        finished = true;
        video.removeEventListener("ended", onEnded);
        resolve(value);
      };
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
  const nextCount = returned - 1;
  const next = [];
  for (let i = 0; i < nextCount; i++) next.push(module.HEAP32[(state.nextPtr >> 2) + i]);
  return { hold: module.HEAP32[state.holdPtr >> 2], next: next };
}
function uploadFrame(rgba) {
  const module = state.module;
  module.HEAPU8.set(rgba, state.framePtr);
  if (!module._tr_frame_upload(state.framePtr, rgba.byteLength)) throw new Error(wasmError());
}

async function playFromStart() {
  video.pause();
  await seek(0);
  video.muted = true;
  await video.play();
}

async function queuePass(duration, interval) {
  let nextScan = 0;
  let lastFrame = null;
  let frames = 0;
  await playFromStart();
  for (;;) {
    cancelled();
    const mediaTime = await nextDecodedFrame();
    if (mediaTime === null) break;
    lastFrame = readFrame();
    frames++;
    let uploaded = false;
    while (nextScan <= mediaTime + 0.000001 && nextScan < duration) {
      if (!uploaded) { uploadFrame(lastFrame); uploaded = true; }
      submitQueue(1, lastFrame, nextScan);
      submitQueue(2, lastFrame, nextScan);
      nextScan += interval;
    }
    status("Pass 1/3: キュー走査 " + frames + "フレーム", duration ? nextScan / duration * 35 : 0);
    if (mediaTime >= duration - 0.0001) break;
  }
  video.pause();
  if (!lastFrame) throw new Error("動画からデコード可能なフレームを取得できませんでした");
  while (nextScan < duration) {
    submitQueue(1, lastFrame, nextScan);
    submitQueue(2, lastFrame, nextScan);
    nextScan += interval;
  }
  status("Pass 1/3: C++キュー復元", 38);
  return frames;
}

function classLabels(output) {
  const labels = new Uint8Array(200);
  const data = output.data;
  const dims = output.dims || [];
  if (dims.length === 2 && dims[0] === 200 && dims[1] > 1) {
    const classes = dims[1];
    for (let cell = 0; cell < 200; cell++) {
      let best = 0;
      let bestValue = Number(data[cell * classes]);
      for (let c = 1; c < classes; c++) {
        const value = Number(data[cell * classes + c]);
        if (value > bestValue) { bestValue = value; best = c; }
      }
      labels[cell] = best;
    }
  } else {
    for (let i = 0; i < 200; i++) labels[i] = Number(data[i]) || 0;
  }
  return labels;
}

async function boardPass(requests, duration) {
  const module = state.module;
  const session = state.session;
  const inputName = session.inputNames[0];
  const outputName = session.outputNames[0];
  let index = 0;
  let frameCount = 0;
  await playFromStart();
  while (index < requests.length) {
    cancelled();
    const mediaTime = await nextDecodedFrame();
    if (mediaTime === null) break;
    const rgba = readFrame();
    frameCount++;
    let uploaded = false;
    while (index < requests.length && requests[index].time <= mediaTime + 0.000001) {
      const request = requests[index++];
      if (!uploaded) { uploadFrame(rgba); uploaded = true; }
      if (!module._tr_board_features(request.player, state.framePtr, rgba.byteLength, state.featurePtr, 200 * 63)) throw new Error(wasmError());
      const features = new Float32Array(module.HEAPF32.buffer, state.featurePtr, 200 * 63).slice();
      const tensor = new ort.Tensor("float32", features, [200, 63]);
      const result = await session.run({ [inputName]: tensor }, [outputName]);
      const labels = classLabels(result[outputName]);
      module.HEAPU8.set(labels, state.labelPtr);
      if (!module._tr_board_finish(request.player, state.framePtr, rgba.byteLength, request.time, state.labelPtr, 200)) throw new Error(wasmError());
      status("Pass 2/3: 盤面ONNX " + index + "/" + requests.length, 40 + index / Math.max(1, requests.length) * 42);
      await yieldToBrowser();
    }
    if (mediaTime >= duration - 0.0001 && index < requests.length) break;
  }
  video.pause();
  if (index !== requests.length) throw new Error("盤面要求を処理できませんでした (" + index + "/" + requests.length + ")");
  return frameCount;
}

function makeDownload(module, path, filename, type) {
  const bytes = module.FS.readFile(path);
  const url = URL.createObjectURL(new Blob([bytes], { type: type || "application/octet-stream" }));
  const link = document.createElement("a");
  link.href = url; link.download = filename; link.textContent = filename;
  $("downloads").append(link);
}

function publishOutput(module, sourceName) {
  const jsonPath = wasmString(module._tr_output_path(0));
  const json = new TextDecoder().decode(module.FS.readFile(jsonPath));
  const parsed = JSON.parse(json);
  const links = $("links"); links.replaceChildren();
  [[1, "P1シミュレータ"], [2, "P2シミュレータ"], [3, "2Pシミュレータ"]].forEach(item => {
    const url = wasmString(module._tr_output_url(item[0]));
    const a = document.createElement("a"); a.href = url; a.target = "_blank"; a.rel = "noopener"; a.textContent = item[1] + "を開く";
    links.append(a);
  });
  $("downloads").replaceChildren();
  [[0, sourceName + "_tetris_recovered.json", "application/json"], [4, sourceName + "_links.html", "text/html"], [5, sourceName + "_report.html", "text/html"], [6, "training-annotation.json", "application/json"], [7, "training-manifest.json", "application/json"]].forEach(item => {
    const path = wasmString(module._tr_output_path(item[0]));
    makeDownload(module, path, item[1], item[2]);
  });
  const p1 = parsed.p1 && parsed.p1.length || 0;
  const p2 = parsed.p2 && parsed.p2.length || 0;
  $("counts").textContent = "P1 " + p1 + " phases / P2 " + p2 + " phases";
  log("C++のwriteOutputsでJSON・URL・レポート・学習アノテーションを生成しました");
}

async function analyze() {
  if (!state.file || state.running) return;
  state.running = true; state.cancel = false;
  $("run").disabled = true; $("cancel").disabled = false; $("links").replaceChildren(); $("downloads").replaceChildren(); $("log").textContent = "開始";
  try {
    const module = await loadWasm();
    await loadModel();
    await loadSettings(module);
    canvas.width = video.videoWidth; canvas.height = video.videoHeight;
    allocateBuffers(module);
    const settingsPtr = wasmCString("/settings.ini");
    const initialized = module._tr_runtime_init(video.videoWidth, video.videoHeight, settingsPtr);
    module._free(settingsPtr);
    if (!initialized) throw new Error(wasmError());
    const duration = video.duration;
    module._tr_runtime_reset(duration);
    const bytes = new Uint8Array(await state.file.arrayBuffer());
    const safeName = state.file.name.replace(/[\/\\]/g, "_") || "video.mp4";
    const inputPath = "/input/" + safeName;
    try { module.FS.mkdir("/input"); } catch (_) {}
    try { module.FS.mkdir("/output"); } catch (_) {}
    module.FS.writeFile(inputPath, bytes);
    log("動画 " + video.videoWidth + "x" + video.videoHeight + ", " + duration.toFixed(3) + "秒");
    const interval = module._tr_sample_interval();
    await queuePass(duration, interval);
    const capacity = Math.ceil(duration / Math.max(interval, 0.001)) * 5 + 32;
    const requestPtr = module._malloc(capacity * 8);
    const p1Count = module._tr_prepare_board_requests(1, requestPtr, capacity);
    const p1Times = Array.from(new Float64Array(module.HEAPF64.buffer, requestPtr, p1Count));
    const p2Count = module._tr_prepare_board_requests(2, requestPtr, capacity);
    const p2Times = Array.from(new Float64Array(module.HEAPF64.buffer, requestPtr, p2Count));
    module._free(requestPtr);
    const requests = p1Times.map(time => ({ player: 1, time })).concat(p2Times.map(time => ({ player: 2, time }))).sort((a, b) => a.time - b.time || a.player - b.player);
    log("C++が生成した盤面要求: P1 " + p1Count + " / P2 " + p2Count);
    await boardPass(requests, duration);
    status("Pass 3/3: C++合法手ビーム探索", 86);
    await yieldToBrowser();
    if (!module._tr_recover()) throw new Error(wasmError());
    await yieldToBrowser();
    const outputDir = "/output/" + (safeName.replace(/\.[^.]*$/, "") || "video");
    try { module.FS.mkdir(outputDir); } catch (_) {}
    const inputPtr = wasmCString(inputPath);
    const outputPtr = wasmCString(outputDir);
    const written = module._tr_write_output(inputPtr, outputPtr);
    module._free(inputPtr); module._free(outputPtr);
    if (!written) throw new Error(wasmError());
    publishOutput(module, safeName.replace(/\.[^.]*$/, "") || "video");
    status("解析完了", 100);
  } catch (error) {
    status(error.message || String(error), 0);
    log("ERROR: " + (error.stack || error));
  } finally {
    state.running = false; $("run").disabled = !state.file; $("cancel").disabled = true;
  }
}

$("video-file").addEventListener("change", () => {
  const file = $("video-file").files && $("video-file").files[0]; if (!file) return;
  state.file = file; video.src = URL.createObjectURL(file); video.load();
  $("file-name").textContent = file.name + " (" + (file.size / 1024 / 1024).toFixed(1) + " MB)";
  $("run").disabled = false; status("動画のメタデータを読み込みました", 0); $("log").textContent = "待機中";
});
video.addEventListener("loadedmetadata", () => { $("duration").textContent = video.videoWidth + "×" + video.videoHeight + " / " + video.duration.toFixed(3) + "秒"; });
$("run").addEventListener("click", analyze);
$("cancel").addEventListener("click", () => { state.cancel = true; status("キャンセル処理中…"); });
