const CACHE_NAME = 'tetris-hub-v8';
const ASSETS = [
    './',
    './index.html',
    './manifest.json',
    './icon-192.png',
    './icon-512.png',
    './js/bridge.js',
    './js/layout.js',
    './js/main.js?v=app-v5',
    './js/menu.js',
    './js/saves.js',
    './js/settings.js',
    './js/snapshot.js',
    './js/state.js',
    './js/toast.js',
    './js/windows.js'
];

self.addEventListener('install', (event) => {
    event.waitUntil(
        caches.open(CACHE_NAME)
            .then((cache) => cache.addAll(ASSETS))
            .then(() => self.skipWaiting())
    );
});

self.addEventListener('activate', (event) => {
    event.waitUntil(
        caches.keys().then((cacheNames) => Promise.all(
            cacheNames
                .filter((cacheName) => cacheName.startsWith('tetris-hub-') && cacheName !== CACHE_NAME)
                .map((cacheName) => caches.delete(cacheName))
        )).then(() => self.clients.claim())
    );
});

self.addEventListener('fetch', (event) => {
    if (event.request.method !== 'GET') return;

    event.respondWith(
        caches.match(event.request).then((response) => response || fetch(event.request))
    );
});
