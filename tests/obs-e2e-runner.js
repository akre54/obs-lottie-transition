const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const childProcess = require('node:child_process');
const { setTimeout: delay } = require('node:timers/promises');

const { summarizeRun, writeSummary } = require('./obs-e2e-assert');

const DEFAULT_OBS_APP = '/Applications/OBS.app/Contents/MacOS/OBS';
const DEFAULT_PLUGIN_BUILD = path.resolve(__dirname, '..', 'build_macos/RelWithDebInfo/obs-lottie-transition.plugin');
const DEFAULT_EXAMPLE = path.resolve(__dirname, '..', 'examples/slide-and-mask.json');
const DEFAULT_BACKEND = 'thorvg';
const DEFAULT_TRIGGER_COUNT = 3;
const DEFAULT_FIXTURE_MODE = 'patterned';
const TRANSITION_NAME = 'Lottie E2E';
const BOOTSTRAP_SCENE = 'Bootstrap';
const SCENE_A = 'Scene A';
const SCENE_B = 'Scene B';

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

function mkdirp(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

function write(filePath, text) {
  mkdirp(path.dirname(filePath));
  fs.writeFileSync(filePath, text);
}

function slugTimestamp(date = new Date()) {
  return date.toISOString().replace(/[:.]/g, '-');
}

function obsConfigRoot() {
  return path.join(os.homedir(), 'Library/Application Support/obs-studio');
}

function makeUuid(seed) {
  const hex = crypto.createHash('sha1').update(seed).digest('hex').slice(0, 32);
  return [
    hex.slice(0, 8),
    hex.slice(8, 12),
    hex.slice(12, 16),
    hex.slice(16, 20),
    hex.slice(20, 32),
  ].join('-');
}

function buildSceneCollection(examplePath, backend, collectionName) {
  return {
    name: collectionName,
    current_scene: BOOTSTRAP_SCENE,
    current_program_scene: BOOTSTRAP_SCENE,
    scene_order: [{ name: BOOTSTRAP_SCENE }],
    sources: [
      {
        prev_ver: 536936449,
        name: BOOTSTRAP_SCENE,
        uuid: makeUuid('bootstrap-scene'),
        id: 'scene',
        versioned_id: 'scene',
        settings: {
          id_counter: 0,
          custom_size: false,
          items: [],
        },
        mixers: 0,
        sync: 0,
        flags: 0,
        volume: 1.0,
        balance: 0.5,
        enabled: true,
        muted: false,
        'push-to-mute': false,
        'push-to-mute-delay': 0,
        'push-to-talk': false,
        'push-to-talk-delay': 0,
        hotkeys: {},
        deinterlace_mode: 0,
        deinterlace_field_order: 0,
        monitoring_type: 0,
        canvas_uuid: '6c69626f-6273-4c00-9d88-c5136d61696e',
        private_settings: {},
      },
    ],
    groups: [],
    quick_transitions: [
      { name: 'Cut', duration: 300, hotkeys: [], id: 1, fade_to_black: false },
      { name: 'Fade', duration: 300, hotkeys: [], id: 2, fade_to_black: false },
      { name: 'Fade', duration: 300, hotkeys: [], id: 3, fade_to_black: true },
    ],
    transitions: [
      {
        name: TRANSITION_NAME,
        id: 'lottie_transition',
        settings: {
          lottie_file: examplePath,
          renderer_backend: backend,
          invert_matte: false,
        },
      },
    ],
    saved_projectors: [],
    canvases: [],
    current_transition: TRANSITION_NAME,
    transition_duration: 1000,
    preview_locked: false,
    scaling_enabled: false,
    scaling_level: -5,
    scaling_off_x: 0.0,
    scaling_off_y: 0.0,
    'virtual-camera': { type2: 3 },
    modules: {
      'scripts-tool': [],
      'output-timer': {
        streamTimerHours: 0,
        streamTimerMinutes: 0,
        streamTimerSeconds: 30,
        recordTimerHours: 0,
        recordTimerMinutes: 0,
        recordTimerSeconds: 30,
        autoStartStreamTimer: false,
        autoStartRecordTimer: false,
        pauseRecordTimer: true,
      },
      'auto-scene-switcher': {
        interval: 300,
        non_matching_scene: '',
        switch_if_not_matching: false,
        active: false,
        switches: [],
      },
    },
    version: 2,
  };
}

function buildGlobalIni(homeRoot) {
  return `[General]
MaxLogs=20
InfoIncrement=-1
ProcessPriority=Normal
EnableAutoUpdates=false
BrowserHWAccel=true

[Video]
Renderer=OpenGL
DisableOSXVSync=true
ResetOSXVSyncOnExit=true

[Locations]
Configuration=${homeRoot}
SceneCollections=${homeRoot}
Profiles=${homeRoot}
PluginManagerSettings=${homeRoot}
`;
}

function buildUserIni(profileName, collectionName) {
  return `[General]
ConfirmOnExit=false
FirstRun=false
AutomaticCollectionSearch=false
AutoSearchPrompt=false

[BasicWindow]
PreviewEnabled=false
PreviewProgramMode=false
ShowTransitions=true
ShowListboxToolbars=true
ShowStatusBar=true

[Basic]
Profile=${profileName}
ProfileDir=${profileName}
SceneCollection=${collectionName}
SceneCollectionFile=${collectionName}.json
ConfigOnNewProfile=true
`;
}

function buildProfileIni(profileName) {
  return `[General]
Name=${profileName}

[Video]
BaseCX=1920
BaseCY=1080
OutputCX=1920
OutputCY=1080
FPSType=0
FPSCommon=60
FPSNum=60
FPSDen=1
ScaleType=bicubic
ColorFormat=NV12
ColorSpace=709
ColorRange=Partial

[Audio]
SampleRate=48000
ChannelSetup=Stereo

[Output]
Mode=Simple

[SimpleOutput]
RecFormat2=hybrid_mov
RecQuality=Stream
FilePath=${path.join(os.homedir(), 'Movies')}
`;
}

function readWebSocketConfig(obsRoot) {
  const file = path.join(obsRoot, 'plugin_config/obs-websocket/config.json');
  const raw = JSON.parse(fs.readFileSync(file, 'utf8'));
  return {
    port: raw.server_port || 4455,
    password: raw.server_password || '',
  };
}

function userInstalledPluginPath() {
  return path.join(obsConfigRoot(), 'plugins/obs-lottie-transition.plugin');
}

function writeObsConfig(obsRoot, examplePath, backend, profileName, collectionName) {
  const profileDir = path.join(obsRoot, 'basic/profiles', profileName);
  const scenesDir = path.join(obsRoot, 'basic/scenes');

  mkdirp(profileDir);
  mkdirp(scenesDir);
  mkdirp(path.join(obsRoot, 'logs'));

  write(path.join(profileDir, 'basic.ini'), buildProfileIni(profileName));
  write(
    path.join(scenesDir, `${collectionName}.json`),
    `${JSON.stringify(buildSceneCollection(examplePath, backend, collectionName), null, 2)}\n`
  );

  return { obsRoot };
}

class ObsWebSocketClient {
  constructor(url, password) {
    this.url = url;
    this.password = password;
    this.socket = null;
    this.pending = new Map();
    this.requestId = 1;
  }

  async connect(timeoutMs = 30000) {
    await new Promise((resolve, reject) => {
      const socket = new WebSocket(this.url);
      const timer = setTimeout(() => {
        socket.close();
        reject(new Error(`Timed out connecting to ${this.url}`));
      }, timeoutMs);

      const cleanup = () => clearTimeout(timer);

      socket.addEventListener('message', (raw) => {
        const message = JSON.parse(String(raw.data));

        if (message.op === 0) {
          const identify = { op: 1, d: { rpcVersion: 1 } };
          const auth = message.d?.authentication;
          if (auth && auth.challenge && auth.salt) {
            const secret = crypto
              .createHash('sha256')
              .update(`${this.password}${auth.salt}`)
              .digest('base64');
            identify.d.authentication = crypto
              .createHash('sha256')
              .update(`${secret}${auth.challenge}`)
              .digest('base64');
          }
          socket.send(JSON.stringify(identify));
          return;
        }

        if (message.op === 2) {
          this.socket = socket;
          cleanup();
          resolve();
          return;
        }

        if (message.op === 7) {
          const requestId = message.d?.requestId;
          const pending = this.pending.get(requestId);
          if (!pending) {
            return;
          }
          this.pending.delete(requestId);
          if (message.d.requestStatus?.result) {
            pending.resolve(message.d.responseData || {});
          } else {
            pending.reject(new Error(message.d.requestStatus?.comment || 'OBS request failed'));
          }
        }
      });

      socket.addEventListener('error', (error) => {
        cleanup();
        reject(error.error || error);
      });

      socket.addEventListener('close', () => {
        if (!this.socket) {
          cleanup();
          reject(new Error('OBS websocket closed before identification'));
        }
      });
    });
  }

  request(requestType, requestData = {}) {
    if (!this.socket) {
      return Promise.reject(new Error('OBS websocket is not connected'));
    }

    const requestId = String(this.requestId++);
    const payload = {
      op: 6,
      d: {
        requestType,
        requestId,
        requestData,
      },
    };

    return new Promise((resolve, reject) => {
      this.pending.set(requestId, { resolve, reject });
      this.socket.send(JSON.stringify(payload));
    });
  }

  close() {
    if (this.socket) {
      this.socket.close();
    }
  }
}

async function connectObs(url, password, timeoutMs = 60000) {
  const started = Date.now();
  while (Date.now() - started < timeoutMs) {
    const client = new ObsWebSocketClient(url, password);
    try {
      await client.connect(5000);
      return client;
    } catch {
      client.close();
      await delay(1000);
    }
  }
  throw new Error(`Unable to connect to OBS websocket at ${url}`);
}

async function ensureScene(client, sceneName) {
  const list = await client.request('GetSceneList');
  const exists = (list.scenes || []).some((scene) => scene.sceneName === sceneName);
  if (!exists) {
    await client.request('CreateScene', { sceneName });
  }
}

function writePpm(filePath, pixelFn, width = 1920, height = 1080) {
  const header = Buffer.from(`P6\n${width} ${height}\n255\n`, 'ascii');
  const body = Buffer.alloc(width * height * 3);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const offset = (y * width + x) * 3;
      const [r, g, b] = pixelFn(x, y, width, height);
      body[offset + 0] = r;
      body[offset + 1] = g;
      body[offset + 2] = b;
    }
  }
  fs.writeFileSync(filePath, Buffer.concat([header, body]));
}

function fillRect(x, y, rect) {
  return x >= rect.x0 && x < rect.x1 && y >= rect.y0 && y < rect.y1;
}

function patternedPixelA(x, y, width, height) {
  let color = [170, 28, 28];
  if (fillRect(x, y, { x0: 1500, x1: 1620, y0: 0, y1: height })) {
    color = [0, 240, 80];
  } else if (fillRect(x, y, { x0: 180, x1: 320, y0: 120, y1: 260 })) {
    color = [255, 255, 255];
  } else if (fillRect(x, y, { x0: 720, x1: 860, y0: 760, y1: 900 })) {
    color = [0, 220, 220];
  }
  return color;
}

function patternedPixelB(x, y, width, height) {
  let color = [28, 28, 170];
  if (fillRect(x, y, { x0: 300, x1: 420, y0: 0, y1: height })) {
    color = [255, 220, 0];
  } else if (fillRect(x, y, { x0: 1580, x1: 1720, y0: 120, y1: 260 })) {
    color = [255, 128, 0];
  } else if (fillRect(x, y, { x0: 760, x1: 900, y0: 760, y1: 900 })) {
    color = [255, 255, 255];
  }
  return color;
}

function solidPixel(r, g, b) {
  return () => [r, g, b];
}

function ensureFixtureImages(runDir, fixtureMode) {
  const fixturesDir = path.join(runDir, 'fixtures');
  mkdirp(fixturesDir);

  const redPpm = path.join(fixturesDir, 'solid-red.ppm');
  const bluePpm = path.join(fixturesDir, 'solid-blue.ppm');
  const redPng = path.join(fixturesDir, 'solid-red.png');
  const bluePng = path.join(fixturesDir, 'solid-blue.png');

  if (fixtureMode === 'solid') {
    writePpm(redPpm, solidPixel(255, 0, 0));
    writePpm(bluePpm, solidPixel(0, 0, 255));
  } else {
    writePpm(redPpm, patternedPixelA);
    writePpm(bluePpm, patternedPixelB);
  }

  childProcess.spawnSync('/usr/bin/sips', ['-s', 'format', 'png', redPpm, '--out', redPng], { encoding: 'utf8' });
  childProcess.spawnSync('/usr/bin/sips', ['-s', 'format', 'png', bluePpm, '--out', bluePng], { encoding: 'utf8' });

  return { redPng, bluePng };
}

async function ensureImageInput(client, sceneName, inputName, file) {
  const inputs = await client.request('GetInputList');
  const exists = (inputs.inputs || []).some((input) => input.inputName === inputName);
  if (exists) {
    return;
  }

  await client.request('CreateInput', {
    sceneName,
    inputName,
    inputKind: 'image_source',
    inputSettings: { file },
    sceneItemEnabled: true,
  });
}

async function convertFramesToPng(artifactDir) {
  const framesDir = path.join(artifactDir, 'frames');
  if (!fs.existsSync(framesDir)) {
    return;
  }

  const files = fs.readdirSync(framesDir).filter((file) => file.endsWith('.ppm'));
  for (const file of files) {
    const input = path.join(framesDir, file);
    const output = path.join(framesDir, file.replace(/\.ppm$/, '.png'));
    const result = childProcess.spawnSync('/usr/bin/sips', ['-s', 'format', 'png', input, '--out', output], {
      encoding: 'utf8',
    });
    if (result.status === 0) {
      fs.unlinkSync(input);
    }
  }
}

function collectObsLog(obsRoot, artifactDir) {
  const logsDir = path.join(obsRoot, 'logs');
  if (!fs.existsSync(logsDir)) {
    return;
  }

  const candidates = fs.readdirSync(logsDir)
    .filter((file) => file.endsWith('.txt') || file.endsWith('.log'))
    .map((file) => path.join(logsDir, file))
    .sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs);

  if (candidates[0]) {
    fs.copyFileSync(candidates[0], path.join(artifactDir, 'obs.log'));
  }
}

async function main() {
  const args = parseArgs(process.argv);
  const obsApp = path.resolve(args['obs-app'] || DEFAULT_OBS_APP);
  const pluginBuild = path.resolve(args['plugin-build'] || DEFAULT_PLUGIN_BUILD);
  const example = path.resolve(args.example || DEFAULT_EXAMPLE);
  const backend = args.backend || DEFAULT_BACKEND;
  const triggers = Number.parseInt(args.triggers || String(DEFAULT_TRIGGER_COUNT), 10);
  const fixtureMode = args.fixture || DEFAULT_FIXTURE_MODE;
  const artifactBase = path.resolve(args['artifact-dir'] || path.join(os.tmpdir(), 'obs-lottie-e2e', `run-${slugTimestamp()}`));
  const artifactDir = path.join(artifactBase, 'artifacts');
  const runDir = artifactBase;
  const obsRoot = path.resolve(args['obs-root'] || obsConfigRoot());
  const runId = path.basename(runDir);
  const profileName = `LottieE2E-${runId}`;
  const collectionName = `LottieE2E-${runId}`;

  if (!fs.existsSync(obsApp)) {
    throw new Error(`OBS binary not found: ${obsApp}`);
  }
  if (!fs.existsSync(pluginBuild)) {
    throw new Error(`Plugin bundle not found: ${pluginBuild}`);
  }
  if (!fs.existsSync(example)) {
    throw new Error(`Example JSON not found: ${example}`);
  }
  if (!fs.existsSync(obsRoot)) {
    throw new Error(`OBS config root not found: ${obsRoot}`);
  }
  if (!['thorvg', 'browser'].includes(backend)) {
    throw new Error(`Unsupported backend: ${backend}`);
  }
  if (!['patterned', 'solid'].includes(fixtureMode)) {
    throw new Error(`Unsupported fixture mode: ${fixtureMode}`);
  }
  if (!Number.isFinite(triggers) || triggers < 1) {
    throw new Error(`Invalid trigger count: ${args.triggers}`);
  }

  mkdirp(artifactDir);
  writeObsConfig(obsRoot, example, backend, profileName, collectionName);
  const websocket = readWebSocketConfig(obsRoot);
  const installedPlugin = userInstalledPluginPath();
  const useInjectedPlugin = !fs.existsSync(installedPlugin);
  const pluginDir = path.dirname(pluginBuild);
  const fixtureImages = ensureFixtureImages(runDir, fixtureMode);
  const processLog = path.join(artifactDir, 'obs-process.log');
  const processLogFd = fs.openSync(processLog, 'a');

  const env = {
    ...process.env,
    LT_E2E_CAPTURE_DIR: artifactDir,
    LT_E2E_TRACE: '1',
    LT_E2E_CAPTURE_FRAMES: '1',
  };
  if (useInjectedPlugin) {
    env.OBS_PLUGINS_PATH = pluginDir;
    env.OBS_PLUGINS_DATA_PATH = pluginDir;
  }

  const obs = childProcess.spawn(obsApp, [
    '--multi',
    '--collection', collectionName,
    '--profile', profileName,
    '--scene', BOOTSTRAP_SCENE,
  ], {
    env,
    detached: false,
    stdio: ['ignore', processLogFd, processLogFd],
  });

  let client;
  try {
    client = await connectObs(`ws://127.0.0.1:${websocket.port}`, websocket.password, 90000);
    await client.request('GetVersion');

    await ensureScene(client, SCENE_A);
    await ensureScene(client, SCENE_B);
    await ensureImageInput(client, SCENE_A, 'Solid A', fixtureImages.redPng);
    await ensureImageInput(client, SCENE_B, 'Solid B', fixtureImages.bluePng);
    await client.request('SetCurrentSceneTransition', { transitionName: 'Cut' });
    try {
      await client.request('SetStudioModeEnabled', { studioModeEnabled: true });
    } catch {}
    await client.request('SetCurrentProgramScene', { sceneName: SCENE_A });
    try {
      await client.request('SetCurrentPreviewScene', { sceneName: SCENE_B });
    } catch {}
    await delay(1200);
    await client.request('SetCurrentSceneTransition', { transitionName: TRANSITION_NAME });
    await client.request('SetCurrentSceneTransitionDuration', { transitionDuration: 1000 });

    let current = SCENE_A;
    for (let i = 0; i < triggers; i += 1) {
      current = current === SCENE_A ? SCENE_B : SCENE_A;
      await client.request('SetCurrentPreviewScene', { sceneName: current });
      await delay(100);
      await client.request('TriggerStudioModeTransition');
      await delay(1600);
    }

    await delay(1500);
  } finally {
    if (client) {
      client.close();
    }
  }

  await new Promise((resolve) => {
    const timer = setTimeout(() => {
      obs.kill('SIGTERM');
      resolve();
    }, 15000);
    obs.once('exit', () => {
      clearTimeout(timer);
      resolve();
    });
  });

  fs.closeSync(processLogFd);
  collectObsLog(obsRoot, artifactDir);
  await convertFramesToPng(artifactDir);

  const summary = summarizeRun(artifactDir, {
    backend,
    example,
    behaviorChecks: args['behavior-checks'] === 'on',
  });
  summary.obs_root = obsRoot;
  summary.profile_name = profileName;
  summary.collection_name = collectionName;
  summary.plugin_mode = useInjectedPlugin ? 'injected' : 'installed';
  summary.fixture_mode = fixtureMode;
  writeSummary(artifactDir, summary);

  process.stdout.write(`${JSON.stringify(summary, null, 2)}\n`);
  if (!summary.pass) {
    process.exitCode = 1;
  }
}

main().catch((error) => {
  console.error(error.stack || String(error));
  process.exit(1);
});
