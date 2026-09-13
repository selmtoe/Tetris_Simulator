const CACHE_NAME = 'tetris-simulator-v44-hub-workflow';
const APP_SHELL = [
  './index.html',
  './styles/simulator.css?v=app-v19',
  './simulator/app/virtual-controller.js',
  './simulator/app/runtime-config.js?v=app-v21',
  './simulator/candidate/kasane-stack-ren-candidate-harness.js?v=candidate-v2',
  './simulator/candidate/kasane-stack-ren-telemetry.js?v=candidate-v2',
  './simulator/app/player-engine.js?v=app-v27',
  './simulator/app/pc-finder.js?v=app-v5',
  './simulator/app/editor.js',
  './simulator/app/settings.js?v=app-v19',
  './shared/cell-cnn-inference.js?v=cell-cnn-v1',
  './simulator/app/scanner.js?v=cell-cnn-v1',
  './shared/tetris-event-codec.js?v=te1-v2',
  './simulator/app/state-transport.js?v=app-v19',
  './simulator/app/league-runner.js?v=league-v15',
  './simulator/app/bootstrap.js?v=app-v19',
  './simulator/app/workspace.js?v=workflow-v2',
  './simulator/workers/cold-clear-wasm-worker.js?v=controller-timing-v3',
  './simulator/workers/cold-clear-wasm.js?v=controller-timing-v2',
  './simulator/workers/cold-clear.wasm?v=controller-timing-v2',
  './simulator/workers/kasane-wasm-worker.js?v=kasane-v14',
  './simulator/workers/kasane-wasm.js?v=kasane-v14',
  './simulator/workers/kasane.wasm?v=kasane-v14',
  './simulator/candidate/kasane-stack-ren-candidate-worker.js?v=candidate-v2',
  './simulator/workers/kasane-wasm.js?v=kasane-candidate-abi-v1',
  './simulator/candidate/kasane-wasm-candidate.js?v=kasane-candidate-v2',
  './simulator/workers/cold-clear-worker.js',
  './simulator/workers/cold-clear-core.js',
  './simulator/workers/pc-finder-worker.js?v=app-v5',
  './simulator/pc-solver/sfinder-pc.js?v=app-v5',
  './simulator/pc-solver/sfinder-pc.wasm?v=app-v5',
  './manifest.webmanifest',
  './icons/icon-192x192.png',
  './icons/icon-512x512.png'
];

// These candidate build products are intentionally gitignored and may not be
// present on a production host. Cache them when a candidate was built, but do
// not make the app-shell upgrade fail when only production WASM is deployed.
const OPTIONAL_CANDIDATE_ASSETS = [
  './simulator/candidate/kasane-stack-ren-candidate.wasm',
  './simulator/candidate/kasane-stack-ren-candidate.manifest.json'
];

self.addEventListener('install', event => {
  event.waitUntil(caches.open(CACHE_NAME).then(async cache => {
    await cache.addAll(APP_SHELL);
    await Promise.allSettled(OPTIONAL_CANDIDATE_ASSETS.map(asset => cache.add(asset)));
  }));
  self.skipWaiting();
});

self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(keys => Promise.all(
      keys.filter(key => key.startsWith('tetris-simulator-') && key !== CACHE_NAME)
        .map(key => caches.delete(key))
    ))
  );
  self.clients.claim();
});

self.addEventListener('fetch', event => {
  if (event.request.method !== 'GET') return;
  const requestUrl = new URL(event.request.url);
  const isCandidateBuildAsset = OPTIONAL_CANDIDATE_ASSETS.some(asset =>
    new URL(asset, self.location.href).pathname === requestUrl.pathname
  );
  if (isCandidateBuildAsset) {
    // Candidate builds reuse an isolated filename while training iterates.
    // Prefer the newest online artifact, update its offline fallback, and use
    // the cache only when the host cannot be reached.
    event.respondWith(
      fetch(event.request).then(async response => {
        if (response.ok) {
          const cache = await caches.open(CACHE_NAME);
          await cache.put(event.request, response.clone());
        }
        return response;
      }).catch(async error => {
        const cached = await caches.match(event.request, { ignoreSearch: true });
        if (cached) return cached;
        throw error;
      })
    );
    return;
  }
  event.respondWith(caches.match(event.request).then(hit => hit || fetch(event.request)));
});
