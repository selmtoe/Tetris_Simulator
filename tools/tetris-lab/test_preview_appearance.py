"""The local theme must not change native files or recognition/input code."""
import html
import re
import threading
import unittest
from types import SimpleNamespace
from urllib.request import urlopen

import preview_appearance as appearance
import server


class AppearanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # A tools-only fixture has no database or owner endpoints.
        cls.http = server.Server(0, SimpleNamespace(root=server.ROOT), True)
        cls.thread = threading.Thread(target=cls.http.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f'http://127.0.0.1:{cls.http.server_port}'

    @classmethod
    def tearDownClass(cls):
        cls.http.shutdown()
        cls.http.server_close()
        cls.thread.join()

    def get(self, path):
        with urlopen(self.base + path, timeout=10) as response:
            return response.read()

    def test_original_script_bytes_and_explicit_variants(self):
        for path in appearance.SCRIPT_PATHS:
            with self.subTest(path=path):
                native = (server.ROOT / path.lstrip('/')).read_bytes()
                self.assertEqual(self.get(path), native)
                themed = self.get(path + '?labTheme=lab').decode()
                self.assertEqual(themed, appearance.script_variant(path, native.decode()))
                self.assertNotEqual(themed, native.decode())
                self.assertEqual((server.ROOT / path.lstrip('/')).read_bytes(), native)

    def test_html_keeps_order_and_original_bypasses_all_variants(self):
        for path in ('/F/index.html', '/index.html'):
            source = (server.ROOT / path.lstrip('/')).read_text(encoding='utf-8')
            themed = appearance.html_variant(source, path)
            self.assertEqual(themed.replace('&amp;labTheme=lab', '').replace('?labTheme=lab', ''), source)
            original = self.get(path + '?theme=original').decode()
            self.assertNotIn('/lab-theme.css', original)
            self.assertNotIn('labTheme=lab', original)
            self.assertNotIn('/lab-appearance.js', original)
            live = self.get(path).decode()
            self.assertIn('/lab-theme.css', live)
            self.assertIn('/lab-appearance.js', live)
            scripts = [html.unescape(url) for url in re.findall(r'<script[^>]+src="([^"]+)"', live)]
            variants = [url for url in scripts if 'labTheme=lab' in url]
            self.assertEqual(len(variants), 4 if path.startswith('/F/') else 3)

    def test_recognition_and_non_theme_scripts_are_byte_identical(self):
        for path in ('/F/app/50-scanner.js', '/F/app/84-ai-scoring-worker.js',
                     '/simulator/app/scanner.js', '/simulator/app/state-transport.js'):
            native = (server.ROOT / path.lstrip('/')).read_bytes()
            self.assertEqual(self.get(path + '?labTheme=lab'), native)
        path = '/simulator/app/runtime-config.js'
        original = (server.ROOT / path.lstrip('/')).read_text(encoding='utf-8')
        themed = appearance.script_variant(path, original)
        # Except the outer canvas's bottom allowance, this whole prefix is
        # untouched: recognition colors and board/piece dimensions are identical.
        themed_prefix = themed.split('const activeSkinColors')[0].replace(
            'const CANVAS_HEIGHT = (BOARD_VISIBLE_HEIGHT + 1.5) * BLOCK_SIZE;',
            'const CANVAS_HEIGHT = (BOARD_VISIBLE_HEIGHT + 0.5) * BLOCK_SIZE;')
        self.assertEqual(original.split('const activeSkinColors')[0], themed_prefix)

    def test_render_variants_only_replace_reviewed_paint_literals(self):
        for path, replacements in appearance.RENDER_LITERALS.items():
            original = (server.ROOT / path.lstrip('/')).read_text(encoding='utf-8')
            expected = original
            for old, (new, count) in replacements.items():
                self.assertEqual(original.count(old), count)
                expected = expected.replace(old, new)
            if path == '/simulator/app/player-engine.js':
                self.assertEqual(expected.count(appearance.METER_SOURCE), 1)
                expected = expected.replace(appearance.METER_SOURCE, appearance.METER_RENDER)
            self.assertEqual(appearance.script_variant(path, original), expected)
        with self.assertRaises(ValueError):
            appearance.script_variant('/F/app/20-editor.js', 'new rendering code')

    def test_palette_matches_the_comparison_board(self):
        lab_css = (server.DEFAULT_LAB / 'lab-enhancements.css').read_text(encoding='utf-8')
        for piece, color in appearance.PIECES.items():
            actual = re.search(r'\.lab-' + piece + r'\{background:(#[0-9a-f]+)', lab_css)[1]
            if len(actual) == 4:
                actual = '#' + ''.join(char * 2 for char in actual[1:])
            self.assertEqual(color, actual)
        self.assertIn('background:' + appearance.BOARD, lab_css)


if __name__ == '__main__':
    unittest.main()
