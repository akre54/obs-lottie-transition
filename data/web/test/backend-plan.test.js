const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const BackendPlan = require('../backend-plan');

const examplesDir = path.resolve(__dirname, '../../..', 'examples');

function readExample(name) {
  return JSON.parse(fs.readFileSync(path.join(examplesDir, name), 'utf8'));
}

function layerNames(json) {
  return json.layers.map((layer) => layer.nm);
}

test('browser backend plan keeps slots with MatteA for channel packing', () => {
  const json = readExample('slide-and-mask.json');
  const plan = BackendPlan.buildLayerPlan(json, 'browser');

  assert.equal(plan.backend, 'browser');
  assert.equal(plan.usesBrowserSource, true);
  assert.equal(plan.usesCanvasPacking, true);
  assert.deepEqual(layerNames(plan.matteA), ['[SlotA]', '[SlotB]', '[MatteA]']);
  assert.deepEqual(layerNames(plan.matteB), ['[MatteB]']);
  assert.deepEqual(layerNames(plan.overlay), ['decorative-bar']);
  assert.deepEqual(layerNames(plan.slots), ['[SlotA]', '[SlotB]']);
});

test('thorvg backend plan splits mattes, overlay, and slots independently', () => {
  const json = readExample('slide-and-mask.json');
  const plan = BackendPlan.buildLayerPlan(json, 'thorvg');

  assert.equal(plan.backend, 'thorvg');
  assert.equal(plan.usesBrowserSource, false);
  assert.equal(plan.usesCanvasPacking, false);
  assert.deepEqual(layerNames(plan.matteA), ['[MatteA]']);
  assert.deepEqual(layerNames(plan.matteB), ['[MatteB]']);
  assert.deepEqual(layerNames(plan.overlay), ['decorative-bar']);
  assert.deepEqual(layerNames(plan.slots), ['[SlotA]', '[SlotB]']);
});

test('backend normalization falls back to browser for unknown values', () => {
  assert.equal(BackendPlan.normalizeBackend('browser'), 'browser');
  assert.equal(BackendPlan.normalizeBackend('thorvg'), 'thorvg');
  assert.equal(BackendPlan.normalizeBackend('other'), 'browser');
  assert.equal(BackendPlan.normalizeBackend(undefined), 'browser');
});
