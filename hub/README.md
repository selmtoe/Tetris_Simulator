# Tetris Hub

`hub/index.html` is the integration shell for the simulator (`../index.html`) and
the chart editor (`../F/index.html`). It keeps the original Hub markup, styles,
storage keys, and postMessage contract intact while the behavior is split into
small ES modules under `hub/js/`.

Open it through a local HTTP server, rather than with `file://`, so its service
worker can register:

~~~text
http://localhost:<port>/hub/
~~~

The fumen-for-mobile frame remains an external integration by design. The two
local iframe URLs are deliberately relative, so the complete application can
run from one repository and one local server.

## Module map

- `bridge.js` — iframe postMessage routing and official-editor clipboard import
- `layout.js` — tab, split, and splitter behavior
- `windows.js` — hidden window-mode behavior and persisted window layouts
- `saves.js` — manual/auto saves, import/export, sharing, and URL loading
- `settings.js` — persisted Hub preferences and auto-save scheduling
- `menu.js` — floating action button menu and drag behavior
- `snapshot.js`, `state.js`, `toast.js` — shared state and small utilities
