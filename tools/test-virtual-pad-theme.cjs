/* The virtual touch pad is a transparent overlay, so it must take its ring and
   letters from the active appearance instead of the native white. Build first
   (tools/build-web.py) and serve the built site; tools/serve.ps1 serves the
   repository root, so the default base points at its dist/pages directory. */
const assert = require('node:assert/strict');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:8765/dist/pages/';
let checks = 0;
function equal(actual, expected, message) { assert.deepEqual(actual, expected, message); checks++; }
function ok(value, message) { assert.ok(value, message); checks++; }

function sample() {
    const canvas = document.getElementById('virtualControllerCanvas');
    const data = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
    const counts = new Map();
    for (let i = 0; i < data.length; i += 4) {
        if (!data[i + 3]) continue;
        const key = [data[i], data[i + 1], data[i + 2], data[i + 3]].join(',');
        counts.set(key, (counts.get(key) || 0) + 1);
    }
    const ranked = [...counts.entries()].sort((a, b) => b[1] - a[1]);
    const opaque = ranked.filter(([key]) => key.endsWith(',255'));
    return {
        theme: document.documentElement.dataset.labTheme || null,
        ink: window.LabAppearance?.colors?.padInk || null,
        ring: ranked[0][0].split(',').map(Number),
        letters: opaque.length ? opaque[0][0].split(',').map(Number) : null,
    };
}

async function openPad(browser, url, theme) {
    const context = await browser.newContext({viewport: {width: 430, height: 900}, hasTouch: true, serviceWorkers: 'block'});
    await context.addInitScript(value => {
        localStorage.setItem('lab-appearance-mode', value);
        localStorage.setItem('tetrisGameSettings', JSON.stringify({touchControlsEnabled: true, touchControlType: 'button'}));
    }, theme);
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    page.on('dialog', dialog => dialog.dismiss());
    await page.goto(url, {waitUntil: 'load'});
    await page.locator('#startGameBtn').click();
    await page.waitForFunction(() => {
        const container = document.getElementById('virtual-controller-container');
        return !!container && getComputedStyle(container).display !== 'none';
    }, null, {timeout: 20000});
    const state = await page.evaluate(sample);
    await context.close();
    equal(errors, [], theme + ': no browser errors while drawing the pad');
    return state;
}

(async () => {
    const browser = await chromium.launch({headless: true});
    try {
        const page = new URL('index.html?standalone=1', base).href;
        const light = await openPad(browser, page, 'light');
        equal(light.theme, 'light', 'light appearance is applied');
        equal(light.ink, '#000000', 'light mode paints black pad ink');
        equal(light.ring.slice(0, 3), [0, 0, 0], 'light mode ring is black');
        ok(light.ring[3] > 160 && light.ring[3] < 200, 'light mode ring keeps the reviewed 0.7 alpha');
        equal(light.letters.slice(0, 3), [0, 0, 0], 'light mode letters are black');

        const dark = await openPad(browser, page, 'dark');
        equal(dark.theme, 'dark', 'dark appearance is applied');
        equal(dark.ink, '#ffffff', 'dark mode keeps the white pad ink');
        equal(dark.ring.slice(0, 3), [255, 255, 255], 'dark mode ring is unchanged');
        ok(dark.ring[3] > 160 && dark.ring[3] < 200, 'dark mode ring keeps the reviewed 0.7 alpha');
        equal(dark.letters.slice(0, 3), [255, 255, 255], 'dark mode letters are unchanged');
        console.log(JSON.stringify({passed: true, checks}));
    } finally {await browser.close();}
})().catch(error => {console.error(error); process.exitCode = 1;});
