"""Check source delivery, public links, and failure on stale/missing packages."""
import importlib.util
from html.parser import HTMLParser
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from urllib.parse import urljoin, urlsplit
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ai_licenses', ROOT / 'tools/package-ai-licenses.py')
licenses = importlib.util.module_from_spec(spec)
spec.loader.exec_module(licenses)


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []

    def handle_starttag(self, tag, attrs):
        if tag == 'a':
            self.links.append(dict(attrs).get('href', ''))


class LicenseDistribution(unittest.TestCase):
    def test_source_and_binaries_match(self):
        licenses.validate(ROOT)
        licenses.validate(ROOT / 'dist/pages')

    def test_covered_sources_and_build_inputs_are_available(self):
        for component, required in {
            'cold-clear': ['simulator/cold-clear-wasm/src/lib.rs', 'simulator/cold-clear-wasm/Cargo.lock',
                           'third_party/cold-clear-reference/bot/src/dag.rs',
                           'third_party/cold-clear-reference/bot/src/modes/normal.rs',
                           'third_party/cold-clear-reference/bot/src/evaluation/standard.rs',
                           'third_party/cold-clear-reference/libtetris/src/lib.rs',
                           'third_party/cold-clear-reference/opening-book/src/dictionary',
                           'tools/build-cold-clear-wasm.ps1', 'licenses/cold-clear-dependencies.txt'],
            'sfinder': ['third_party/sfinder-cpp-master/src/finder/perfect.cpp',
                        'third_party/sfinder-cpp-master/LICENSE', 'simulator/pc-solver/pc_solver_api.cpp',
                        'tools/build-pc-solver-wasm.ps1', 'licenses/sfinder-dependencies.txt'],
        }.items():
            with zipfile.ZipFile(ROOT / f'licenses/{component}-source.zip') as bundle:
                self.assertTrue(set(required).issubset(bundle.namelist()))
                self.assertFalse(any(part in ('target', '.git', 'lab-private', 'data', 'exports')
                                     for name in bundle.namelist() for part in Path(name).parts))

    def test_notice_links_resolve_under_pages_prefix(self):
        output = ROOT / 'dist/pages'
        links = Links()
        links.feed((output / 'licenses/index.html').read_text(encoding='utf-8'))
        for href in links.links:
            if urlsplit(href).netloc:
                continue
            resolved = urlsplit(urljoin('/Tetris_Simulator/licenses/index.html', href)).path
            self.assertTrue(resolved.startswith('/Tetris_Simulator/'))
            self.assertTrue((output / resolved.removeprefix('/Tetris_Simulator/')).is_file()
                            or (output / resolved.removeprefix('/Tetris_Simulator/') / 'index.html').is_file(), href)
        for page, href in [('index.html', './licenses/index.html'), ('F/index.html', '../licenses/index.html')]:
            links = Links()
            links.feed((output / page).read_text(encoding='utf-8'))
            self.assertIn(href, links.links)

    def test_missing_notice_and_stale_binary_block_distribution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = licenses.LICENSE_FILES + [p for c in licenses.COMPONENTS.values() for p in c['binaries']]
            for relative in paths:
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / relative, target)
            licenses.validate(root)
            notice = root / 'licenses/sfinder-MIT.txt'
            data = notice.read_bytes()
            notice.unlink()
            with self.assertRaisesRegex(ValueError, 'Missing license'):
                licenses.validate(root)
            notice.write_bytes(data)
            wasm = root / 'simulator/workers/cold-clear.wasm'
            original = wasm.read_bytes()
            wasm.write_bytes(original + b'stale')
            with self.assertRaisesRegex(ValueError, 'Binary changed'):
                licenses.validate(root)
            wasm.write_bytes(original)
            archive = root / 'licenses/cold-clear-source.zip'
            archive.write_bytes(archive.read_bytes() + b'changed')
            with self.assertRaisesRegex(ValueError, 'Source archive changed'):
                licenses.validate(root)


if __name__ == '__main__':
    unittest.main()
