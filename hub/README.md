# Simulator / Viewer workspace

Hub controls the existing simulator and viewer without replacing their designs.
The public build applies the approved appearance from `tools/tetris-lab/preview-*`
at build time; it does not require the personal Lab server.

## Navigation

- Ordinary entry: full simulator preparation.
- Start: full simulator play. Its return destination is captured at start.
- Back after ordinary play: full simulator preparation, even after earlier replay practice.
- Record: full viewer showing that play's replay.
- Practice here: simulator on the left, source viewer on the right.
- Seeking on the right does not change the left draft; applying a different position is explicit.
- Wide viewer / resume preparation preserve both states without reloading either iframe.
- Back after practice: split preparation. Source return opens the original practice anchor.
- Narrow screens switch preparation/reference from the existing Share dialog. Play always occupies the full workspace.

There is no surrounding toolbar or tab row: the native app receives the full
viewport height, including during preparation. Open/import and interrupted-record
actions are inside the simulator's existing Share dialog. Practice reference
navigation appears there only while preparing a replay position. The viewer has
no generic Screen button or appearance Settings button. Its secondary return,
reference and full-view actions are inside the existing Share dialog. Appearance
follows the same preference as the simulator, including live light/dark changes.

Hub has no user save button, replay library or named autosave entries. Files and
links are opened through the small Open dialog. Sharing and file export remain
in the viewer. The old Hub's browser storage keys are not read, changed or deleted.

## Recovery

IndexedDB database `tetris-workspace-recovery` retains the current workspace per
recent tab (at most eight), with no library UI. The tab identity is stored in
sessionStorage. A normal launch restores the current tab's state, or the most
recent workspace when starting a new tab. An explicit incoming replay link takes
precedence. `?fresh=1` starts with ordinary preparation.

Snapshots run about every two seconds and at navigation/visibility changes.
They include the simulator preparation, current viewer document and cursor,
practice anchor, normal preparation draft and split width. Thus a crash may lose
the latest interval, and browser storage removal also removes recovery data.

An interrupted game restores its preparation with play stopped, and exposes the
last captured replay through “中断前の記録を見る”. It does not resume the live game
clock, AI worker or partially executed input. Existing rules for transferring
board/NEXT/HOLD remain unchanged.

## Run and build

`start.bat` builds the static bundle with Python 3, then opens the local workspace.
Manual build: `python tools/build-web.py`. Output is `dist/pages` only. Serve that
folder using any static HTTP server. HTML, JavaScript, WASM, approved appearance
and the image-input model are copied from an explicit allowlist. CUDA applications,
private Lab data and analysis datasets are outside the public artifact.

The Pages workflow builds this folder and publishes only that artifact on main.
The feature branch does not deploy the public site.

The static build fingerprints every HTML script/style URL after applying the
approved appearance. Its generated service workers precache only present files,
prefer the network and fall back only to their own build cache. Old native browser
caches cannot supply saturated palettes to the new silhouette CSS. Recovery
storage and user preferences are retained across these asset updates.

Native editor access is retained through `F/?view=editor`. `?standalone=1` bypasses
the static entry redirect. Existing te1/v3/f1/f2, simulator snapshots, native
recovery exports and legacy Hub shared payloads remain importable.

## Source files

- `js/workflow.js`: four modes and origin-dependent return behavior.
- `js/workspace.js`: viewport, source/practice state and verified iframe messages.
- `js/recovery.js`: transactional recovery storage.
- `workspace.css`: surrounding layout only.
- `../simulator/app/workspace.js`, `../F/app/85-workspace.js`: native app adapters.

## Verification

Run `tools/test-hub-workflow.cjs` against the built bundle with Playwright. Set
`PLAYWRIGHT_MODULE` if it is installed outside the repository. The browser checks
use an isolated profile and cover both return origins, immutable sources, independent
seeking, mobile layout, direct links, reload recovery and interrupted recordings.
`tools/test-web-build.py` checks static paths, appearance and deployment boundaries.
`tools/test-web-cache-upgrade.cjs` installs a legacy native cache in an isolated
browser, upgrades to the static build and verifies the soft palette without
clearing old caches or the saved appearance preference.
`tools/test-hub-appearance.cjs` compares native and embedded viewport, board and
control geometry at desktop/mobile widths in both themes, and captures both views.
The existing event codec and viewer AI/export regression checks also remain applicable.

## Restore point

The GitHub tag `hub-before-20260913` points to the pre-rebuild web tools and approved
appearance source (`6f9948a`). The implementation follows it on
`codex/hub-workflow-20260913`. To inspect the old version without overwriting current
work, use `git worktree add ../Tetris_Hub_Before hub-before-20260913`.
The working repository's main checkout and unrelated staged changes are preserved;
these history snapshots use a separate Git index.
