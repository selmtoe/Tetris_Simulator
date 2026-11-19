const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, '..');
const read = relative => fs.readFileSync(path.join(root, relative), 'utf8');
const scanners = [
    'simulator/app/scanner.js',
    'F/app/50-scanner.js',
    'Load PPT/ppt-scanner.js',
];
const pages = [
    ['index.html', 'simulator/app/scanner.js'],
    ['F/index.html', 'app/50-scanner.js'],
    ['Load PPT/index.html', 'ppt-scanner.js'],
];

const contract = JSON.parse(read('Load PPT/tetris.model.json'));
const onnx = fs.readFileSync(path.join(root, 'Load PPT', 'tetris.onnx'));
const hash = crypto.createHash('sha256').update(onnx).digest('hex');
assert.equal(hash, contract.onnxSha256, 'deployed ONNX does not match its model contract');
assert.deepEqual(contract.input.shape, ['N', 3, 32, 32]);
assert.deepEqual(contract.output.shape, ['N', 9]);
assert.deepEqual(contract.output.classes, ['null', 'G', 'S', 'Z', 'L', 'J', 'O', 'I', 'T']);

for (const scanner of scanners) {
    const source = read(scanner);
    assert.match(source, /TetrisCellCnn\.recognizeBoard\(/, `${scanner} does not call the Cell CNN`);
    assert.doesNotMatch(source, /\[200,\s*63\]/, `${scanner} still contains the legacy ONNX input contract`);
    assert.match(source, /x:\s*316,\s*y:\s*157,\s*w:\s*351,\s*h:\s*713/, `${scanner} has the wrong P1 crop`);
    assert.match(source, /x:\s*1253,\s*y:\s*157,\s*w:\s*354,\s*h:\s*713/, `${scanner} has the wrong P2 crop`);
}

for (const [page, scannerPath] of pages) {
    const html = read(page);
    const helperPosition = html.indexOf('cell-cnn-inference.js');
    const scannerPosition = html.indexOf(scannerPath);
    assert(helperPosition >= 0, `${page} does not load the Cell CNN helper`);
    assert(scannerPosition > helperPosition, `${page} loads its scanner before the Cell CNN helper`);
}

const serviceWorker = read('sw.js');
assert.match(serviceWorker, /tetris-simulator-v21-cell-cnn/);
assert.match(serviceWorker, /cell-cnn-inference\.js\?v=cell-cnn-v1/);

console.log(JSON.stringify({ modelBytes: onnx.length, sha256: hash, scanners: scanners.length, pages: pages.length }));
