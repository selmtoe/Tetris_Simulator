const CACHE_NAME = 'tetris-simulator-v18';
const APP_SHELL = [
  './index.html',
  './styles/simulator.css?v=app-v7',
  './simulator/app/virtual-controller.js',
  './simulator/app/runtime-config.js?v=app-v7',
  './simulator/app/player-engine.js?v=app-v7',
  './simulator/app/pc-finder.js?v=app-v5',
  './simulator/app/editor.js',
  './simulator/app/settings.js?v=app-v5',
  './simulator/app/scanner.js',
  './simulator/app/state-transport.js?v=app-v7',
  './simulator/app/bootstrap.js?v=app-v7',
  './simulator/workers/cold-clear-worker.js',
  './simulator/workers/cold-clear-core.js',
  './simulator/workers/pc-finder-worker.js?v=app-v5',
  './simulator/pc-solver/sfinder-pc.js?v=app-v5',
  './simulator/pc-solver/sfinder-pc.wasm?v=app-v5',
  './manifest.webmanifest',
  './icons/icon-192x192.png',
  './icons/icon-512x512.png'
];

self.addEventListener('install', event => {
  event.waitUntil(caches.open(CACHE_NAME).then(cache => cache.addAll(APP_SHELL)));
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
  event.respondWith(caches.match(event.request).then(hit => hit || fetch(event.request)));
});
