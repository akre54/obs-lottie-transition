const path = require('node:path');
const childProcess = require('node:child_process');

const DEFAULT_OBS_APP = '/Applications/OBS.app/Contents/MacOS/OBS';
const DEFAULT_PLUGIN_BUILD = path.resolve(__dirname, '..', 'build_macos/RelWithDebInfo/obs-lottie-transition.plugin');

const CASES = [
  {
    name: 'slide-and-mask-transform',
    example: 'examples/slide-and-mask.json',
    backend: 'thorvg',
    fixture: 'patterned',
  },
  {
    name: 'simple-wipe-control',
    example: 'examples/simple-wipe.json',
    backend: 'thorvg',
    fixture: 'patterned',
  },
  {
    name: 'circle-reveal-radial',
    example: 'examples/circle-reveal.json',
    backend: 'thorvg',
    fixture: 'patterned',
  },
  {
    name: 'sliding-window-matte-transform',
    example: 'examples/sliding-window.json',
    backend: 'thorvg',
    fixture: 'solid',
  },
  {
    name: 'spotlight-zoom-circular-transform',
    example: 'examples/spotlight-zoom.json',
    backend: 'thorvg',
    fixture: 'solid',
  },
  {
    name: 'diagonal-band-sweep',
    example: 'examples/diagonal-band.json',
    backend: 'thorvg',
    fixture: 'solid',
  },
  {
    name: 'credit-card-shuffle-lf-center-reveal',
    example: 'examples/credit-card-shuffle-lf.json',
    backend: 'thorvg',
    fixture: 'solid',
  },
  {
    name: 'circle-transition-lf-center-reveal',
    example: 'examples/circle-transition-lf.json',
    backend: 'thorvg',
    fixture: 'solid',
  },
  {
    name: 'menu-page-transition-lf-page-reveal',
    example: 'examples/menu-page-transition-lf.json',
    backend: 'thorvg',
    fixture: 'solid',
  },
  {
    name: 'ibm-exploration-01-lf-panel-reveal',
    example: 'examples/ibm-exploration-01-lf.json',
    backend: 'thorvg',
    fixture: 'solid',
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
    '--triggers', '1',
    '--fixture', caseDef.fixture || 'patterned',
    '--behavior-checks', 'on',
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
      errors: result.summary?.errors || (result.stderr ? [result.stderr.trim()] : []),
    })),
  }, null, 2)}\n`);

  if (failed.length > 0) {
    process.exit(1);
  }
}

main();
