// Actual Fullscreen API transitions, including the same-origin Hub boundary.
// Desktop Chromium verifies API behavior, not Android system bars or cutouts.
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:8898/';
let checks = 0;
const check = (value, label) => { assert.ok(value, label); checks++; };

async function openSettings(frame, viewer = false) {
    if (viewer) {
        const menu = frame.locator('#viewer-page-indicator');
        if (await menu.getAttribute('aria-expanded') !== 'true') await menu.click();
        await frame.locator('#back-to-editor-btn').click();
        await frame.locator('#editor-container .lab-settings-open').click();
    } else await frame.locator('#settingsBtn').click();
    await frame.locator('#lab-fullscreen-toggle').waitFor({state: 'visible'});
}

async function toggle(page, frame) {
    const button = frame.locator('#lab-fullscreen-toggle');
    check(await button.textContent() === '全画面表示', 'initial button offers fullscreen');
    await button.click();
    await page.waitForFunction(() => document.fullscreenElement === document.documentElement);
    await frame.waitForFunction(() => document.getElementById('lab-fullscreen-toggle').textContent === '全画面を解除');
    check(await button.isVisible(), 'Settings remains visible in fullscreen');
    if (frame !== page.mainFrame()) {
        check(await frame.evaluate(() => document.fullscreenElement === null), 'Hub root owns fullscreen, not an individual pane');
    }
    await button.click();
    await page.waitForFunction(() => !document.fullscreenElement);
    await frame.waitForFunction(() => document.getElementById('lab-fullscreen-toggle').textContent === '全画面表示');
    check(await button.isEnabled(), 'can re-enter after leaving fullscreen');
    // Leaving through browser/OS controls must also reset the button.
    await button.click();
    await page.waitForFunction(() => !!document.fullscreenElement);
    await page.evaluate(() => document.exitFullscreen());
    await frame.waitForFunction(() => document.getElementById('lab-fullscreen-toggle').textContent === '全画面表示');
    check(await button.isEnabled(), 'external fullscreen exit updates Settings');
}

(async () => {
    const browser = await chromium.launch({headless: true});
    try {
        const context = await browser.newContext({viewport: {width: 1280, height: 800}, serviceWorkers: 'block'});
        const errors = [];
        context.on('page', page => page.on('pageerror', error => errors.push(error.message)));
        for (const path of ['manifest.webmanifest', 'hub/manifest.json']) {
            const response = await context.request.get(new URL(path, base).href);
            check(response.ok() && (await response.json()).display === 'fullscreen', path + ' starts fullscreen');
        }
        const hub = await context.newPage();
        await hub.goto(new URL('hub/', base).href);
        await hub.frameLocator('#iframe-sim').locator('#settingsBtn').waitFor();
        const sim = await hub.locator('#iframe-sim').elementHandle().then(handle => handle.contentFrame());
        await openSettings(sim);
        await toggle(hub, sim);
        // A denied request must remain retryable and must not claim success.
        await hub.evaluate(() => {
            window.savedFullscreenRequest = document.documentElement.requestFullscreen;
            document.documentElement.requestFullscreen = () => Promise.reject(new TypeError('denied for test'));
        });
        await sim.locator('#lab-fullscreen-toggle').click();
        await sim.waitForFunction(() => document.getElementById('lab-fullscreen-status').textContent.includes('切り替えられませんでした'));
        check(await sim.locator('#lab-fullscreen-toggle').isEnabled(), 'denied request can be retried');
        check(await hub.evaluate(() => !document.fullscreenElement), 'denied request does not enter fullscreen');
        await hub.evaluate(() => { document.documentElement.requestFullscreen = window.savedFullscreenRequest; });
        await toggle(hub, sim);
        check(await sim.locator('#lab-fullscreen-status').textContent() === '', 'successful retry clears the error');
        await hub.screenshot({path: '../fullscreen-settings.png'});
        await hub.close();

        for (const [path, viewer] of [['index.html?standalone=1', false], ['F/index.html?standalone=1', true]]) {
            const page = await context.newPage();
            await page.goto(new URL(path, base).href);
            await openSettings(page.mainFrame(), viewer);
            await toggle(page, page.mainFrame());
            await page.close();
        }

        const unsupported = await context.newPage();
        await unsupported.addInitScript(() => { Element.prototype.requestFullscreen = undefined; });
        await unsupported.goto(new URL('index.html?standalone=1', base).href);
        await openSettings(unsupported.mainFrame());
        check(await unsupported.locator('#lab-fullscreen-toggle').isDisabled(), 'unsupported API is disabled');
        check((await unsupported.locator('#lab-fullscreen-status').textContent()).includes('利用できません'), 'unsupported API is explained');
        await unsupported.close();

        const installed = await context.newPage();
        await installed.addInitScript(() => {
            const original = window.matchMedia.bind(window);
            window.matchMedia = query => query === '(display-mode: fullscreen)' ? {matches: true, addEventListener() {}} : original(query);
        });
        await installed.goto(new URL('index.html?standalone=1', base).href);
        await openSettings(installed.mainFrame());
        check(await installed.evaluate(() => !document.fullscreenElement), 'manifest fullscreen has no API fullscreen element');
        check(await installed.locator('#lab-fullscreen-toggle').textContent() === '全画面表示中', 'manifest fullscreen is recognized');
        check(await installed.locator('#lab-fullscreen-toggle').isDisabled(), 'does not offer an unavailable API exit for manifest fullscreen');
        check(errors.length === 0, errors.join('\n'));
        console.log(JSON.stringify({passed: true, checks, androidHardwareTested: false}));
    } finally { await browser.close(); }
})().catch(error => {console.error(error); process.exit(1);});
