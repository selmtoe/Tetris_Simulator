"""Local appearance variants. Native application files are never rewritten.

Only the seven explicitly listed script URLs opt in. Rendering literals,
display palettes, garbage-meter paint and outer canvas allowance change. Board
cells, piece positions, native input handlers and analysis stay intact.
"""
import re
from urllib.parse import urljoin, urlsplit

# Source: Lab/lab-enhancements.css .lab-I ... .lab-G and .lab-board.
PIECES = {'I': '#4ec7cd', 'O': '#eac44a', 'T': '#b56fc7', 'L': '#e0a15a',
          'J': '#587aca', 'S': '#82c964', 'Z': '#e97d7d', 'G': '#999999'}
BOARD = '#20262a'
GRID = '#32383c'  # #ffffff15 over the comparison board background.
FRAME = '#50575b'
CANVAS_FONT = '"Segoe UI", "Noto Sans JP", sans-serif'

RENDER_LITERALS = {
    '/F/app/20-editor.js': {"'#0f0f18'": (repr(BOARD), 1), "'#444'": (repr(GRID), 2),
        "ctx.fillStyle = 'rgba(255,255,255,.16)';\n            ctx.fillRect(px, py, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);":
            ("window.LabAppearance.drawPlacement(ctx, px, py, EDITOR_BLOCK_SIZE);", 1)},
    '/F/app/30-viewer.js': {"'rgba(0,0,0,0.5)'": (repr(BOARD), 1),
        "'#444'": (repr(GRID), 4), "'#4b4b7c'": (repr(FRAME), 1), '"Orbitron"': (CANVAS_FONT, 1),
        "ctx.fillStyle = '#FFF';": ("ctx.fillStyle = window.LabAppearance.colors.muted;", 2),
        "viewerCtx.fillStyle = 'rgba(255,255,255,.16)';\n                viewerCtx.fillRect(drawX, drawY, BLOCK_SIZE, BLOCK_SIZE);":
            ("window.LabAppearance.drawPlacement(viewerCtx, drawX, drawY, BLOCK_SIZE);", 1)},
    '/F/app/84-ai-scoring.js': {"'rgba(0,0,0,0.5)'": (repr(BOARD), 1),
        "'#444'": (repr(GRID), 3), "'#4b4b7c'": (repr(FRAME), 1),
        "ctx.fillStyle = 'rgba(255,255,255,0.16)';\n            ctx.fillRect(px, py, BLOCK_SIZE, BLOCK_SIZE);":
            ("window.LabAppearance.drawPlacement(ctx, px, py, BLOCK_SIZE);", 1)},
    '/simulator/app/editor.js': {"'#0f0f18'": (repr(BOARD), 2), "'#444'": (repr(GRID), 1)},
    '/simulator/app/player-engine.js': {"'rgba(0,0,0,0.5)'": (repr(BOARD), 2),
        "'#444'": (repr(GRID), 2), "'#4b4b7c'": (repr(FRAME), 1), '"Orbitron"': (CANVAS_FONT, 6),
        "if (!useCustomBG) ctx.fillText('HOLD', layout.hold.x, layout.hold.y - bSize);":
            ("if (!useCustomBG) { ctx.fillStyle = window.LabAppearance.colors.muted; ctx.fillText('HOLD', layout.hold.x, layout.hold.y - bSize); }", 1),
        "if (!useCustomBG) ctx.fillText('NEXT', layout.next[0].x, layout.next[0].y - bSize);":
            ("if (!useCustomBG) { ctx.fillStyle = window.LabAppearance.colors.muted; ctx.fillText('NEXT', layout.next[0].x, layout.next[0].y - bSize); }", 1),
        "const charSpacing = bSize * 0.6;": ("ctx.fillStyle = window.LabAppearance.colors.muted; const charSpacing = bSize * 0.6;", 1)},
}
SCRIPT_PATHS = frozenset(RENDER_LITERALS) | {'/F/app/10-state.js', '/simulator/app/runtime-config.js'}

METER_SOURCE = '''        // Garbage Meter (盤面の左横)
        // カスタム背景時は描画しない、または位置調整が必要だが、ここでは標準位置(盤面左)依存とする
        if (!useCustomBG) {
            const meterX = layout.board.x - 12;
            const meterWidth = 8; 
            const meterMaxHeight = BOARD_VISIBLE_HEIGHT * bSize;
            const pendingHeight = Math.min(this.pendingGarbage, BOARD_VISIBLE_HEIGHT) * bSize;
            
            if (pendingHeight > 0) { 
                ctx.fillStyle = 'red'; 
                ctx.fillRect(meterX, layout.board.y + meterMaxHeight - pendingHeight, meterWidth, pendingHeight);
            }
            const queuedLines = this.garbageQueue.reduce((sum, g) => sum + g.lines, 0);
            const queuedHeight = Math.min(queuedLines, BOARD_VISIBLE_HEIGHT - this.pendingGarbage) * bSize;
            if (queuedHeight > 0) { 
                ctx.fillStyle = 'yellow';
                ctx.fillRect(meterX, layout.board.y + meterMaxHeight - pendingHeight - queuedHeight, meterWidth, queuedHeight);
            }
        }

'''
METER_RENDER = '''        // Incoming meter: the same pending/queued values, themed paint only.
        if (!useCustomBG) {
            window.LabAppearance.drawMeter(ctx, layout.board.x - 12, layout.board.y,
                bSize, BOARD_VISIBLE_HEIGHT, this.pendingGarbage,
                this.garbageQueue.reduce((sum, g) => sum + g.lines, 0),
                window.LabAppearance.colors);
        }

'''


def meter_variant(source):
    pattern = r'\r?\n'.join(re.escape(line) for line in METER_SOURCE.split('\n'))
    # Read-text tests normalize CRLF; byte-serving retains the source's newlines.
    matches = list(re.finditer(pattern, source))
    if len(matches) != 1:
        raise ValueError('Preview garbage meter source changed')
    matched = matches[0]
    rendered = METER_RENDER.replace('\n', '\r\n') if '\r\n' in matched[0] else METER_RENDER
    return source[:matched.start()] + rendered + source[matched.end():]


def _replace(source, old, new, count):
    if '\r\n' in source and '\n' in old:
        old, new = old.replace('\n', '\r\n'), new.replace('\n', '\r\n')
    # Stop on changed source instead of applying a partially matching theme.
    if source.count(old) != count:
        raise ValueError(f'Preview appearance source changed: {old!r}')
    return source.replace(old, new)


def script_variant(path, source):
    if path not in SCRIPT_PATHS:
        return source
    palette = ', '.join(f'{key!r}: {color!r}' for key, color in PIECES.items())
    if path == '/F/app/10-state.js':
        for name in ('COLORS', 'NEXT_COLORS'):
            pattern = rf'const {name} = \{{[^\n]+\}};'
            if len(re.findall(pattern, source)) != 1:
                raise ValueError(f'Preview palette source changed: {name}')
            source = re.sub(pattern, f'const {name} = {{ {palette} }};', source)
    elif path == '/simulator/app/runtime-config.js':
        # Keep COLORS, SCAN_COLORS and SCAN_COLOR_PALETTE untouched: recognition
        # thresholds must not inherit the display palette. Preserve E and skins.
        source = _replace(source, 'const activeSkinColors = { ...COLORS };',
                          f'const activeSkinColors = {{ ...COLORS, {palette} }};', 1)
        source = _replace(source, "const EDITOR_COLORS = {...COLORS, 'EMPTY': '#000000'};",
                          "const EDITOR_COLORS = {...activeSkinColors, 'EMPTY': '#000000'};", 1)
        # The eighth default NEXT reaches below the old canvas edge. Extend the
        # outer drawing area one block without moving pieces or adding board rows.
        source = _replace(source,
                          'const CANVAS_HEIGHT = (BOARD_VISIBLE_HEIGHT + 0.5) * BLOCK_SIZE;',
                          'const CANVAS_HEIGHT = (BOARD_VISIBLE_HEIGHT + 1.5) * BLOCK_SIZE;', 1)
    for old, (new, count) in RENDER_LITERALS.get(path, {}).items():
        source = _replace(source, old, new, count)
    if path == '/simulator/app/player-engine.js':
        source = meter_variant(source)
    return source


def html_variant(html, page_path):
    def rewrite(match):
        src = match.group(2)
        path = urlsplit(urljoin(page_path, src)).path
        # Existing local scripts retain order, defer, versions and relative URL.
        if path in SCRIPT_PATHS and not urlsplit(src).netloc:
            src += ('&amp;' if '?' in src else '?') + 'labTheme=lab'
        return match.group(1) + src + match.group(3)
    return re.sub(r'(<script\b[^>]*\bsrc=")([^"]+)("[^>]*>)', rewrite, html)
