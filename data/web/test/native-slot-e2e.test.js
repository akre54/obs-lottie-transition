const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const childProcess = require('node:child_process');

const cli = process.env.LOTTIE_SLOT_EVAL_CLI;
const examplesDir = path.resolve(__dirname, '../../..', 'examples');

function runSlotEval(file, frame) {
  const result = childProcess.spawnSync(
    cli,
    [path.join(examplesDir, file), String(frame)],
    { encoding: 'utf8' }
  );

  assert.equal(result.status, 0, result.stderr);
  return JSON.parse(result.stdout);
}

test('native slot evaluator CLI resolves slide-and-mask endpoints', () => {
  if (!cli) {
    test.skip('native slot evaluator helper not configured');
    return;
  }

  const start = runSlotEval('slide-and-mask.json', 0);
  const end = runSlotEval('slide-and-mask.json', 30);

  assert.equal(start.slotA.pos_x, 960);
  assert.equal(start.slotB.pos_x, 2880);
  assert.equal(end.slotA.pos_x, -960);
  assert.equal(end.slotB.pos_x, 960);
});

test('native slot evaluator CLI resolves sliding-window slot animation endpoints', () => {
  if (!cli) {
    test.skip('native slot evaluator helper not configured');
    return;
  }

  const start = runSlotEval('sliding-window.json', 0);
  const end = runSlotEval('sliding-window.json', 45);

  assert.equal(start.slotA.scale_x, 1);
  assert.equal(start.slotB.scale_x, 1.1);
  assert.equal(start.slotB.pos_x, 1200);
  assert.equal(end.slotA.scale_x, 1.05);
  assert.equal(end.slotB.scale_x, 1);
  assert.equal(end.slotB.pos_x, 960);
});
