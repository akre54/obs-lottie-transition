const test = require('node:test');
const assert = require('node:assert/strict');

const BridgeCore = require('../bridge-core');

function decodeFloat(high, low, min, max) {
  const raw = ((high << 8) | low) >>> 0;
  return min + (raw / 65535) * (max - min);
}

test('filterLayers keeps only matching names', () => {
  const input = {
    layers: [
      { nm: '[MatteA]' },
      { nm: '[MatteB]' },
      { nm: 'Overlay' }
    ]
  };

  const filtered = BridgeCore.filterLayers(input, (name) => name === '[MatteA]');

  assert.deepEqual(filtered.layers.map((layer) => layer.nm), ['[MatteA]']);
  assert.deepEqual(input.layers.map((layer) => layer.nm), ['[MatteA]', '[MatteB]', 'Overlay']);
});

test('packFrameData packs matte luminance and overlay alpha', () => {
  const matteA = new Uint8ClampedArray([
    255, 255, 255, 255,
    10, 20, 30, 0
  ]);
  const matteB = new Uint8ClampedArray([
    0, 0, 0, 0,
    255, 255, 255, 128
  ]);
  const overlay = new Uint8ClampedArray([
    10, 20, 30, 64,
    50, 60, 70, 200
  ]);
  const output = new Uint8ClampedArray(8);

  BridgeCore.packFrameData(matteA, matteB, overlay, output);

  assert.deepEqual(Array.from(output), [
    255, 0, 64, 255,
    0, 128, 200, 255
  ]);
});

test('encodeDataStrip writes encoded transforms and magic marker', () => {
  const width = 8;
  const height = 2;
  const imageData = { data: new Uint8ClampedArray(width * height * 4) };
  const transformA = { pos_x: 100, pos_y: -50, scale_x: 1.5, scale_y: 0.75, rotation: 45, opacity: 0.25 };
  const transformB = { pos_x: -200, pos_y: 300, scale_x: 2.5, scale_y: 1.25, rotation: -30, opacity: 0.9 };

  BridgeCore.encodeDataStrip(imageData, width, height, transformA, transformB);

  const rowStart = (height - 1) * width * 4;
  const aPosX = decodeFloat(imageData.data[rowStart], imageData.data[rowStart + 1], -4096, 4096);
  const aPosY = decodeFloat(imageData.data[rowStart + 2], imageData.data[rowStart + 3], -4096, 4096);
  const marker = Array.from(imageData.data.slice(rowStart + 24, rowStart + 28));

  assert.ok(Math.abs(aPosX - 100) < 0.2);
  assert.ok(Math.abs(aPosY - (-50)) < 0.2);
  assert.deepEqual(marker, [0xCA, 0xFE, 0xBA, 0xBE]);
});

test('extractTransform returns identity defaults when element is missing', () => {
  assert.deepEqual(BridgeCore.extractTransform(null), {
    pos_x: 0,
    pos_y: 0,
    scale_x: 1,
    scale_y: 1,
    rotation: 0,
    opacity: 1
  });
});
