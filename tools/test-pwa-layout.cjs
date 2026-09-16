// Exercise the full-size PWA shell, rotating cutout insets, and Settings links.
const assert = require('node:assert/strict');
const { chromium, devices } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:8898/';
const github = 'https://github.com/selmtoe/Tetris_Simulator/blob/main/THIRD_PARTY_NOTICES.md';
let checks = 0;
const check = (value, label) => { assert.ok(value, label); checks++; };
async function stable(frame) {
    await frame.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
}
async function bounds(page, width, height) {
    const result = await page.evaluate(() => {
        const ids = ['workspace-panes', 'simulator-pane', 'iframe-sim'];
        return { viewport: [innerWidth, innerHeight], rectangles: ids.map(id => document.getElementById(id).getBoundingClientRect().toJSON()),
                 cover: document.querySelector('meta[name="viewport"]').content.includes('viewport-fit=cover'),
                 padding: getComputedStyle(document.body).padding, scroll: [scrollX, scrollY] };
    });
    check(result.cover, 'PWA entry opts into the full display');
    check(result.viewport[0] === width && result.viewport[1] === height, 'current orientation dimensions');
    for (const rect of result.rectangles) {
        check(rect.x === 0 && rect.y === 0, 'no top or left shell gutter');
        check(rect.width === width && rect.height === height, 'shell fills the available display');
    }
    check(result.padding === '0px' && result.scroll.every(value => value === 0), 'no page padding or scroll offset');
}
async function settings(frame, viewer) {
    if (viewer) {
        if (await frame.locator('#viewer-page-indicator').getAttribute('aria-expanded') !== 'true') {
            await frame.locator('#viewer-page-indicator').click();
        }
        // Viewer preferences live in its editor's existing Settings dialog.
        await frame.locator('#back-to-editor-btn').click();
        await frame.locator('#editor-container .lab-settings-open').click();
    } else await frame.locator('#settingsBtn').click();
    const dialog = frame.locator(viewer ? '.lab-settings-dialog' : '#settings-modal');
    const link = dialog.locator('.license-notice a');
    await link.scrollIntoViewIfNeeded();
    check(await link.isVisible(), 'license is visible in Settings');
    check(await link.getAttribute('href') === github, 'opens the GitHub notices directly');
    check(await dialog.evaluate(el => {
        const notice = el.querySelector('.license-notice');
        return notice === (el.matches('dialog') ? el : el.querySelector('#settings-close').parentElement).lastElementChild;
    }), 'license is at the bottom of Settings');
    check(await frame.locator('#share-modal .license-notice, #share-modal a[href*="licenses/"]').count() === 0, 'no license in Share');
    await dialog.locator(viewer ? '.lab-settings-footer button' : '#settings-close').click();
}
(async () => {
    const browser = await chromium.launch({ headless: true });
    try {
        const context = await browser.newContext({ ...devices['Pixel 7'], viewport: {width:412,height:915}, serviceWorkers: 'block' });
        const page = await context.newPage();
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(new URL('hub/', base).href);
        await page.frameLocator('#iframe-sim').locator('#startGameBtn').waitFor();
        const sim = await page.locator('#iframe-sim').elementHandle().then(handle => handle.contentFrame());
        for (const [width, height, insets] of [
            [412, 915, {top:28,left:0,right:0,bottom:24}],
            [915, 412, {top:24,left:36,right:0,bottom:24}],
            [412, 915, {top:28,left:0,right:0,bottom:24}]
        ]) {
            // Supply the CSS values normally provided by Android. Desktop
            // Chromium cannot reproduce the Android window's physical cutout.
            await page.evaluate(values => {
                for (const [side, value] of Object.entries(values)) document.documentElement.style.setProperty('--viewport-safe-' + side, value + 'px');
            }, insets);
            await page.setViewportSize({width, height});
            await sim.evaluate(() => dispatchEvent(new Event('resize')));
            await stable(sim);
            await bounds(page, width, height);
            const result = await sim.evaluate(() => {
                const css = getComputedStyle(document.documentElement);
                return {top:parseFloat(css.getPropertyValue('--app-safe-top')),left:parseFloat(css.getPropertyValue('--app-safe-left')),
                        editor:document.getElementById('editor-container').getBoundingClientRect().toJSON()};
            });
            check(result.top === insets.top && result.left === insets.left, 'cutout insets reach the embedded app');
            check(result.editor.left >= insets.left && result.editor.top >= insets.top, 'editor stays inside the usable area');
            check(result.editor.right <= width - insets.right + 1 && result.editor.bottom <= height - insets.bottom + 1, 'editor fits after rotation');
        }
        await settings(sim, false);
        await sim.locator('#startGameBtn').click();
        await sim.waitForFunction(() => document.documentElement.dataset.labPlayFit === 'true');
        await stable(sim);
        const controls = await sim.locator('#game-controls').boundingBox();
        check(controls.y >= 28, 'play controls stay clear of the portrait cutout');
        await page.screenshot({path: '../after-portrait.png'});
        const viewerPage = await context.newPage();
        await viewerPage.goto(new URL('F/index.html?standalone=1', base).href);
        await settings(viewerPage.mainFrame(), true);
        check(errors.length === 0, errors.join('\n'));
        await context.close();
        const desktop = await browser.newPage({viewport:{width:1280,height:800},serviceWorkers:'block'});
        await desktop.goto(new URL('hub/', base).href);
        await desktop.frameLocator('#iframe-sim').locator('#startGameBtn').waitFor();
        await bounds(desktop,1280,800);
        console.log(JSON.stringify({passed:true,checks,androidHardwareTested:false}));
    } finally { await browser.close(); }
})().catch(error => {console.error(error);process.exit(1);});
