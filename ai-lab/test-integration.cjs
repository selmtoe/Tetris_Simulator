'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');
const { load } = require('./simulator.cjs');

test('the selected release is exactly the previously evaluated genome', () => {
  const model = JSON.parse(fs.readFileSync(path.join(__dirname, 'models/ga-v1.json'), 'utf8'));
  const sha = crypto.createHash('sha256').update(JSON.stringify(model.genome)).digest('hex');
  assert.equal(sha, '880ded0208ae826e90546d4bb51c274f9f800068db3103a131fb7b5d367f49bd');
  assert.equal(model.modelSha256, sha);
  assert.ok(Object.values(model.genome.opponent).some(n => n !== 0));
});

test('1P guard applies only to opponent-dependent AI; current NEXT10 remains intact', () => {
  const loaded = load();
  const result = vm.runInContext(`({
    blocked: aiStartBlockReason('1P', true, 'ga-v1'),
    human: aiStartBlockReason('1P', false, 'ga-v1'),
    standard: aiStartBlockReason('1P', true, 'cold-clear'),
    versus: aiStartBlockReason('2P', true, 'ga-v1'),
    next: gameSettings.maxNext
  })`, loaded.context);
  assert.match(result.blocked, /2P/);
  for (const key of ['human', 'standard', 'versus']) assert.equal(result[key], '');
  assert.equal(result.next, 10);
  const bootstrap = fs.readFileSync(path.join(__dirname, '../simulator/app/bootstrap.js'), 'utf8');
  assert.equal((bootstrap.match(/const aiBlockReason = aiStartBlockReason\(/g) || []).length, 2, 'start and retry both guard before construction');
});
