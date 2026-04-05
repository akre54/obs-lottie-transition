const path = require('node:path');
const childProcess = require('node:child_process');

const DEFAULT_OBS_APP = '/Applications/OBS.app/Contents/MacOS/OBS';
const DEFAULT_PLUGIN_BUILD = path.resolve(__dirname, '..', 'build_macos/RelWithDebInfo/obs-lottie-transition.plugin');

const CASES = [
  {
    name: 'slide-and-mask-perf',
    example: 'examples/slide-and-mask.json',
    backend: 'thorvg',
  },
  {
    name: 'simple-wipe-perf',
    example: 'examples/simple-wipe.json',
    backend: 'thorvg',
  },
  {
    name: 'circle-reveal-perf',
    example: 'examples/circle-reveal.json',
    backend: 'thorvg',
  },
];

function parseArgs(argv) {
  const out = {};
  for (let i = 2; i < argv.length; i += 1) {
    const arg = argv[i];
    if (!arg.startsWith('--')) {
      throw new Error(`unexpected argument: ${arg}`);
    }

    const key = arg.slice(2);
    const value = argv[i + 1];
    if (!value || value.startsWith('--')) {
      throw new Error(`missing value for --${key}`);
    }
    out[key] = value;
    i += 1;
  }
  return out;
}

function runCase(caseDef, args) {
  const runner = path.resolve(__dirname, 'obs-e2e-runner.js');
  const obsApp = path.resolve(args['obs-app'] || DEFAULT_OBS_APP);
  const pluginBuild = path.resolve(args['plugin-build'] || DEFAULT_PLUGIN_BUILD);
  const example = path.resolve(__dirname, '..', caseDef.example);

  const child = childProcess.spawnSync(process.execPath, [
    runner,
    '--obs-app', obsApp,
    '--plugin-build', pluginBuild,
    '--example', example,
    '--backend', caseDef.backend,
    '--triggers', args.triggers || '3',
    '--fixture', 'patterned',
    '--perf', 'on',
    '--capture-frames', 'off',
    '--visual-checks', 'off',
    '--perf-checks', 'on',
    '--min-render-fps', args['min-render-fps'] || '30',
    '--max-avg-render-gap-ms', args['max-avg-render-gap-ms'] || '40',
    '--max-max-render-gap-ms', args['max-max-render-gap-ms'] || '85',
    '--max-avg-backend-ms', args['max-avg-backend-ms'] || '22',
    '--max-avg-callback-ms', args['max-avg-callback-ms'] || '28',
  ], {
    encoding: 'utf8',
  });

  let summary = null;
  const text = (child.stdout || '').trim();
  if (text) {
    try {
      summary = JSON.parse(text);
    } catch {}
  }

  return {
    name: caseDef.name,
    status: child.status,
    summary,
    stderr: child.stderr || '',
  };
}

function main() {
  const args = parseArgs(process.argv);
  const results = CASES.map((caseDef) => runCase(caseDef, args));
  const failed = results.filter((result) => result.status !== 0);

  process.stdout.write(`${JSON.stringify({
    pass: failed.length === 0,
    cases: results.map((result) => ({
      name: result.name,
      status: result.status,
      artifact_dir: result.summary?.artifact_dir || null,
      perf: result.summary?.perf?.totals || null,
      errors: result.summary?.errors || (result.stderr ? [result.stderr.trim()] : []),
    })),
  }, null, 2)}\n`);

  if (failed.length > 0) {
    process.exit(1);
  }
}

main();
