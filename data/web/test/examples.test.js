const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const childProcess = require('node:child_process');

const BridgeCore = require('../bridge-core');

const examplesDir = path.resolve(__dirname, '../../..', 'examples');

function readExample(name) {
  return JSON.parse(fs.readFileSync(path.join(examplesDir, name), 'utf8'));
}

function layerNames(json) {
  return json.layers.map((layer) => layer.nm);
}

test('all example animations have sane top-level timing and dimensions', () => {
  const files = fs.readdirSync(examplesDir).filter((file) => file.endsWith('.json'));

  assert.ok(files.length > 0);

  for (const file of files) {
    const json = readExample(file);
    assert.equal(json.w, 1920, `${file} width`);
    assert.equal(json.h, 1080, `${file} height`);
    assert.ok(json.fr > 0, `${file} frame rate`);
    assert.ok(json.op > json.ip, `${file} frame range`);
    assert.ok(Array.isArray(json.layers) && json.layers.length > 0, `${file} layers`);
  }
});

test('reserved layer routing for hybrid example stays stable', () => {
  const json = readExample('slide-and-mask.json');

  const matteA = BridgeCore.filterLayers(json, (name) => {
    return name === '[MatteA]' || name === '[SlotA]' || name === '[SlotB]';
  });
  const matteB = BridgeCore.filterLayers(json, (name) => name === '[MatteB]');
  const overlay = BridgeCore.filterLayers(json, (name) => {
    return name !== '[MatteA]' && name !== '[MatteB]' && name !== '[SlotA]' && name !== '[SlotB]';
  });

  assert.deepEqual(layerNames(matteA), ['[SlotA]', '[SlotB]', '[MatteA]']);
  assert.deepEqual(layerNames(matteB), ['[MatteB]']);
  assert.deepEqual(layerNames(overlay), ['decorative-bar']);
});

test('matte-only example keeps overlay empty and reserved layers intact', () => {
  const json = readExample('simple-wipe.json');

  const matteA = BridgeCore.filterLayers(json, (name) => {
    return name === '[MatteA]' || name === '[SlotA]' || name === '[SlotB]';
  });
  const matteB = BridgeCore.filterLayers(json, (name) => name === '[MatteB]');
  const overlay = BridgeCore.filterLayers(json, (name) => {
    return name !== '[MatteA]' && name !== '[MatteB]' && name !== '[SlotA]' && name !== '[SlotB]';
  });

  assert.deepEqual(layerNames(matteA), ['[MatteA]']);
  assert.deepEqual(layerNames(matteB), ['[MatteB]']);
  assert.deepEqual(layerNames(overlay), []);
});

test('overlay-bearing example does not accidentally introduce MatteB', () => {
  const json = readExample('circle-reveal.json');
  const names = layerNames(json);

  assert.ok(names.includes('[MatteA]'));
  assert.ok(!names.includes('[MatteB]'));
  assert.ok(names.includes('ring-decoration'));
});

test('JS transform strip encoding stays compatible with native decoder', () => {
  const decoder = process.env.TRANSFORM_DECODE_CLI;

  if (!decoder) {
    test.skip('native transform decoder helper not configured');
    return;
  }

  const imageData = { data: new Uint8ClampedArray(8 * 2 * 4) };
  const slotA = {
    pos_x: 321.25,
    pos_y: -123.5,
    scale_x: 1.75,
    scale_y: 0.5,
    rotation: 33,
    opacity: 0.4
  };
  const slotB = {
    pos_x: -640,
    pos_y: 222.125,
    scale_x: 2.25,
    scale_y: 1.2,
    rotation: -147.5,
    opacity: 0.85
  };

  BridgeCore.encodeDataStrip(imageData, 8, 2, slotA, slotB);

  const rowStart = (2 - 1) * 8 * 4;
  const bytes = Array.from(imageData.data.slice(rowStart, rowStart + 24)).map(String);
  const result = childProcess.spawnSync(decoder, bytes, { encoding: 'utf8' });

  assert.equal(result.status, 0, result.stderr);

  const decoded = JSON.parse(result.stdout);

  assert.ok(Math.abs(decoded.slotA.pos_x - slotA.pos_x) < 0.2);
  assert.ok(Math.abs(decoded.slotA.pos_y - slotA.pos_y) < 0.2);
  assert.ok(Math.abs(decoded.slotA.scale_x - slotA.scale_x) < 0.01);
  assert.ok(Math.abs(decoded.slotA.scale_y - slotA.scale_y) < 0.01);
  assert.ok(Math.abs(decoded.slotA.rotation - slotA.rotation) < 0.02);
  assert.ok(Math.abs(decoded.slotA.opacity - slotA.opacity) < 0.001);

  assert.ok(Math.abs(decoded.slotB.pos_x - slotB.pos_x) < 0.2);
  assert.ok(Math.abs(decoded.slotB.pos_y - slotB.pos_y) < 0.2);
  assert.ok(Math.abs(decoded.slotB.scale_x - slotB.scale_x) < 0.01);
  assert.ok(Math.abs(decoded.slotB.scale_y - slotB.scale_y) < 0.01);
  assert.ok(Math.abs(decoded.slotB.rotation - slotB.rotation) < 0.02);
  assert.ok(Math.abs(decoded.slotB.opacity - slotB.opacity) < 0.001);
});
