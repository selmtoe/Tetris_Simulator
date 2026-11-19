const CACHE_NAME = 'tetris-hub-workflow-v2';
const ASSETS = ['./', './index.html', './workspace.css?v=workflow-v2', './js/workspace.js?v=workflow-v2', './js/workflow.js', './js/recovery.js', './manifest.json', './icon-192.png', './icon-512.png'];
self.addEventListener('install', event => {
    event.waitUntil(caches.open(CACHE_NAME).then(cache => cache.addAll(ASSETS)).then(() => self.skipWaiting()));
});
self.addEventListener('activate', event => {
    event.waitUntil(caches.keys().then(names => Promise.all(names.filter(name => name.startsWith('tetris-hub-') && name !== CACHE_NAME).map(name => caches.delete(name)))).then(() => self.clients.claim()));
});
self.addEventListener('fetch', event => {
    if (event.request.method !== 'GET' || new URL(event.request.url).origin !== self.location.origin) return;
    event.respondWith(fetch(event.request).then(async response => {
        if (response.ok) (await caches.open(CACHE_NAME)).put(event.request, response.clone());
        return response;
    }).catch(() => caches.match(event.request)));
});
