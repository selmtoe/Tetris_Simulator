"""Preserve the source supplied with each sfinder / Cold Clear binary release.

Run immediately after compiling, before editing the source again. Web builds
validate these snapshots rather than silently pairing old WASM with new source.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = {
    'cold-clear': {
        'directories': ['third_party/cold-clear-reference', 'simulator/cold-clear-wasm', 'licenses/dependency-overrides'],
        'files': ['simulator/COLD_CLEAR_PORT.md', 'tools/build-cold-clear-wasm.ps1',
                  'simulator/workers/cold-clear-core.js', 'simulator/workers/cold-clear-wasm.js',
                  'simulator/workers/cold-clear-wasm-worker.js', 'licenses/cold-clear-dependencies.txt'],
        'binaries': ['simulator/workers/cold-clear.wasm'],
        'license': 'third_party/cold-clear-reference/LICENSE',
    },
    'sfinder': {
        'directories': ['third_party/sfinder-cpp-master'],
        'files': ['simulator/pc-solver/pc_solver_api.cpp', 'tools/build-pc-solver-wasm.ps1', 'licenses/sfinder-dependencies.txt'],
        'binaries': ['simulator/pc-solver/sfinder-pc.js', 'simulator/pc-solver/sfinder-pc.wasm'],
        'license': 'third_party/sfinder-cpp-master/LICENSE',
    },
}
COMMON = ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'tools/package-ai-licenses.py']
SOURCE_EXTENSIONS = {'.rs', '.toml', '.lock', '.cpp', '.h', '.hpp', '.md', '.txt'}
LICENSE_FILES = ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'simulator/COLD_CLEAR_PORT.md', 'licenses/index.html',
                 'licenses/sfinder-MIT.txt', 'licenses/cold-clear-MPL-2.0.txt',
                 'licenses/cold-clear-dependencies.txt', 'licenses/sfinder-dependencies.txt'] + [
    f'licenses/{component}-{suffix}' for component in COMPONENTS
    for suffix in ('source.zip', 'build.json')]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def cargo_notices(root):
    """Retain transitive crate notices too, including build/platform dependencies."""
    metadata = json.loads(subprocess.check_output([
        'cargo', 'metadata', '--locked', '--format-version', '1',
        '--manifest-path', str(root / 'simulator/cold-clear-wasm/Cargo.toml')], cwd=root))
    sections = ['Cold Clear WASM dependency notices\nIncludes Cargo.lock build and platform dependencies; not every crate is linked on every target.\n']
    for package_info in sorted(metadata['packages'], key=lambda p: (p['name'], p['version'])):
        if not package_info['source']:
            continue
        directory = Path(package_info['manifest_path']).parent
        notices = sorted(p for p in directory.rglob('*') if p.is_file() and
                         p.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE', 'UNLICENSE', 'AUTHORS')))
        override = root / 'licenses/dependency-overrides' / f"{package_info['name']}-{package_info['version']}.txt"
        if not notices and override.is_file():
            notices = [override]
        if not notices:
            raise ValueError(f"Missing dependency license text: {package_info['name']}")
        sections.append(f"\n{'=' * 72}\n{package_info['name']} {package_info['version']}\nLicense: {package_info['license']}\nSource: {package_info.get('repository') or package_info['source']}\n")
        for notice in notices:
            label = notice.relative_to(directory).as_posix() if notice.is_relative_to(directory) else notice.name
            sections.append(f'\n--- {label} ---\n' + notice.read_text(encoding='utf-8', errors='replace'))
    sysroot = Path(subprocess.check_output(['rustc', '--print', 'sysroot'], text=True).strip())
    rustdoc = sysroot / 'share/doc/rust'
    from html.parser import HTMLParser
    class Text(HTMLParser):
        def handle_data(self, data):
            sections.append(data)
    sections.append('\n\nRust standard library notices\n' + subprocess.check_output(['rustc', '--version'], text=True))
    Text().feed((rustdoc / 'COPYRIGHT-library.html').read_text(encoding='utf-8'))
    for notice in sorted((rustdoc / 'licenses').glob('*')):
        if notice.is_file():
            sections.append(f'\n--- Rust {notice.name} ---\n' + notice.read_text(encoding='utf-8'))
    (root / 'licenses/cold-clear-dependencies.txt').write_text('\n'.join(sections), encoding='utf-8', newline='\n')


def source_files(root, component):
    spec = COMPONENTS[component]
    paths = set(COMMON + spec['files'])
    for directory in spec['directories']:
        for path in (root / directory).rglob('*'):
            relative = path.relative_to(root)
            if not path.is_file() or any(part in ('target', '.git') for part in relative.parts):
                continue
            if path.suffix in SOURCE_EXTENSIONS or path.name in ('LICENSE', 'dictionary'):
                paths.add(relative.as_posix())
    return sorted(paths)


def package(root, component, emscripten_root=None):
    spec = COMPONENTS[component]
    destination = root / 'licenses'
    destination.mkdir(exist_ok=True)
    if component == 'cold-clear':
        cargo_notices(root)
    else:
        if emscripten_root is None:
            raise ValueError('Pass --emscripten-root from the SDK used to compile sfinder.')
        sdk = Path(emscripten_root)
        notices = [sdk / 'LICENSE', sdk / 'AUTHORS'] + sorted(
            p for p in (sdk / 'system/lib').rglob('*') if p.is_file() and
            p.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE')))
        if not notices[0].is_file() or len(notices) < 3:
            raise ValueError('Emscripten runtime notices not found')
        text = 'sfinder WASM: Emscripten and bundled runtime notices\nIncludes optional system libraries supplied by this SDK.\n'
        for notice in notices:
            text += f'\n--- {notice.relative_to(sdk).as_posix()} ---\n' + notice.read_text(encoding='utf-8', errors='replace') + '\n'
        (destination / 'sfinder-dependencies.txt').write_text(text, encoding='utf-8', newline='\n')
    license_name = 'sfinder-MIT.txt' if component == 'sfinder' else 'cold-clear-MPL-2.0.txt'
    (destination / license_name).write_bytes((root / spec['license']).read_bytes())
    if component == 'sfinder':
        js = root / spec['binaries'][0]
        text = js.read_text(encoding='utf-8')
        marker = '/* sfinder-cpp license notice\n'
        if text.startswith(marker):
            text = text.split('*/\n', 1)[1]
        notice = (root / spec['license']).read_text(encoding='utf-8').strip()
        js.write_text(marker + notice + '\nSource and notices: ../../licenses/index.html\n*/\n' + text,
                      encoding='utf-8', newline='\n')
    archive = io.BytesIO()
    hashes = {}
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as bundle:
        for relative in source_files(root, component):
            data = (root / relative).read_bytes()
            hashes[relative] = digest(data)
            info = zipfile.ZipInfo(relative, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            bundle.writestr(info, data)
    archive_path = destination / f'{component}-source.zip'
    archive_path.write_bytes(archive.getvalue())
    revision = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=root, capture_output=True, text=True)
    record = {
        'component': component,
        'base_revision': revision.stdout.strip() if revision.returncode == 0 else None,
        'source_note': 'The ZIP is the build-time source snapshot, including uncommitted modifications. base_revision alone is not its identity.',
        'source_archive_sha256': digest(archive.getvalue()),
        'source_files': hashes,
        'binaries': {path: digest((root / path).read_bytes()) for path in spec['binaries']},
    }
    (destination / f'{component}-build.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(f'Packaged {component}: {len(hashes)} source files')


def validate(root):
    for relative in LICENSE_FILES:
        if not (root / relative).is_file():
            raise ValueError(f'Missing license distribution file: {relative}')
    for component, spec in COMPONENTS.items():
        record = json.loads((root / f'licenses/{component}-build.json').read_text(encoding='utf-8'))
        if set(record['binaries']) != set(spec['binaries']):
            raise ValueError(f'Incomplete binary record: {component}')
        for relative, expected in record['binaries'].items():
            if digest((root / relative).read_bytes()) != expected:
                raise ValueError(f'Binary changed without its source package: {relative}; rebuild with the component build script.')
        data = (root / f'licenses/{component}-source.zip').read_bytes()
        if digest(data) != record['source_archive_sha256']:
            raise ValueError(f'Source archive changed: {component}')
        with zipfile.ZipFile(io.BytesIO(data)) as bundle:
            if set(bundle.namelist()) != set(record['source_files']):
                raise ValueError(f'Incomplete source archive: {component}')
            for relative, expected in record['source_files'].items():
                if digest(bundle.read(relative)) != expected:
                    raise ValueError(f'Source archive member changed: {relative}')
            if bundle.read(spec['license']).replace(b'\r\n', b'\n') != (root / ('licenses/sfinder-MIT.txt' if component == 'sfinder' else 'licenses/cold-clear-MPL-2.0.txt')).read_bytes().replace(b'\r\n', b'\n'):
                raise ValueError(f'License text mismatch: {component}')
            dependency_notice = f'licenses/{component}-dependencies.txt'
            if bundle.read(dependency_notice).replace(b'\r\n', b'\n') != (root / dependency_notice).read_bytes().replace(b'\r\n', b'\n'):
                raise ValueError(f'Dependency notice mismatch: {component}')
    mit = (root / 'licenses/sfinder-MIT.txt').read_text(encoding='utf-8').strip()
    if mit not in (root / 'simulator/pc-solver/sfinder-pc.js').read_text(encoding='utf-8'):
        raise ValueError('sfinder JavaScript is missing its MIT notice')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', choices=COMPONENTS)
    parser.add_argument('--emscripten-root', type=Path)
    args = parser.parse_args()
    if args.component:
        package(ROOT, args.component, args.emscripten_root)
    else:
        validate(ROOT)
        print('AI license distribution verified')
