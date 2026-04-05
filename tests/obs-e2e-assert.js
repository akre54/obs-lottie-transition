const fs = require('node:fs');
const path = require('node:path');

function safeRead(filePath) {
  try {
    return fs.readFileSync(filePath, 'utf8');
  } catch {
    return '';
  }
}

function loadEvents(artifactDir) {
  const file = path.join(artifactDir, 'plugin-events.jsonl');
  const text = safeRead(file).trim();
  if (!text) {
    return [];
  }

  return text
    .split('\n')
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => JSON.parse(line));
}

function findObsLogs(artifactDir) {
  const files = [
    path.join(artifactDir, 'obs.log'),
    path.join(artifactDir, 'obs-process.log'),
  ];
  return files.map((file) => ({ file, text: safeRead(file) })).filter((entry) => entry.text);
}

function bucket(events, bucketPercent) {
  return events.filter((event) => event.event === 'render_sample' && event.bucket_percent === bucketPercent);
}

function byTrigger(events) {
  const out = new Map();
  for (const event of events) {
    if (event.event !== 'render_sample') {
      continue;
    }
    const list = out.get(event.transition_index) || [];
    list.push(event);
    out.set(event.transition_index, list);
  }
  for (const list of out.values()) {
    list.sort((a, b) => a.bucket_percent - b.bucket_percent);
  }
  return out;
}

function colorDistance(a, b) {
  return Math.abs(a.mean_r - b.mean_r) +
    Math.abs(a.mean_g - b.mean_g) +
    Math.abs(a.mean_b - b.mean_b);
}

function resolveFramePath(artifactDir, relativePath) {
  if (!relativePath) {
    return relativePath;
  }

  if (!relativePath.endsWith('.ppm')) {
    return relativePath;
  }

  const pngPath = relativePath.replace(/\.ppm$/, '.png');
  if (fs.existsSync(path.join(artifactDir, pngPath))) {
    return pngPath;
  }

  return relativePath;
}

function summarizeRun(artifactDir, options = {}) {
  const events = loadEvents(artifactDir);
  const logs = findObsLogs(artifactDir);
  const triggers = byTrigger(events);
  const renderSamples = events.filter((event) => event.event === 'render_sample');
  const starts = events.filter((event) => event.event === 'transition_start');
  const stops = events.filter((event) => event.event === 'transition_stop');
  const midpoints = bucket(events, 50);
  const finals = bucket(events, 100);
  const errors = [];

  const pluginLoaded = logs.some((entry) => entry.text.includes('[lottie-transition] Plugin loaded'));
  if (!pluginLoaded) {
    errors.push('OBS log never reported plugin load');
  }

  if (starts.length === 0) {
    errors.push('No transition_start event captured');
  }

  if (stops.length === 0) {
    errors.push('No transition_stop event captured');
  }

  if (midpoints.length === 0) {
    errors.push('No midpoint render samples captured');
  }

  let blankFrameCount = 0;
  for (const event of renderSamples) {
    if ((event.nonblack_ratio || 0) < 0.02 || (event.nonzero_alpha_ratio || 0) < 0.90) {
      blankFrameCount += 1;
    }
  }
  if (blankFrameCount > 0) {
    errors.push(`Detected ${blankFrameCount} blank/near-empty sampled frames`);
  }

  let midpointEquivalentCount = 0;
  for (const [triggerIndex, samples] of triggers.entries()) {
    const start = samples.find((event) => event.bucket_percent === 0);
    const middle = samples.find((event) => event.bucket_percent === 50);
    const end = samples.find((event) => event.bucket_percent === 100);
    if (!start || !middle || !end) {
      continue;
    }

    const startDistance = colorDistance(middle, start);
    const endDistance = colorDistance(middle, end);
    if (startDistance < 30 || endDistance < 30) {
      midpointEquivalentCount += 1;
      errors.push(`Trigger ${triggerIndex} midpoint stayed too close to an endpoint`);
    }
  }

  let jumpFrameCount = 0;
  for (const event of renderSamples) {
    if ((event.mean_abs_rgb_delta || 0) > 160) {
      jumpFrameCount += 1;
    }
  }
  if (jumpFrameCount > 0) {
    errors.push(`Detected ${jumpFrameCount} sampled discontinuity spikes`);
  }

  let endpointMismatchCount = 0;
  const triggerEntries = Array.from(triggers.entries()).sort((a, b) => a[0] - b[0]);
  for (let i = 0; i < triggerEntries.length - 1; i += 1) {
    const [, current] = triggerEntries[i];
    const [, next] = triggerEntries[i + 1];
    const end = current.find((event) => event.bucket_percent === 100);
    const nextStart = next.find((event) => event.bucket_percent === 0);
    if (!end || !nextStart) {
      continue;
    }
    if (colorDistance(end, nextStart) > 35) {
      endpointMismatchCount += 1;
      errors.push(`Trigger ${i + 1} endpoint did not match the next trigger start`);
    }
  }

  for (const entry of logs) {
    const badLines = entry.text
      .split('\n')
      .filter((line) =>
        line.includes('[lottie-transition]') &&
        /(Failed|ERROR|error:|warning: Requested backend)/.test(line)
      );
    if (badLines.length > 0) {
      errors.push(`Plugin log contains failure lines in ${path.basename(entry.file)}`);
    }
  }

  const capturedFrames = renderSamples
    .filter((event) => event.frame_path)
    .map((event) => ({
      transition_index: event.transition_index,
      bucket_percent: event.bucket_percent,
      frame_path: resolveFramePath(artifactDir, event.frame_path),
    }));

  const create = events.find((event) => event.event === 'create') || null;
  const summary = {
    artifact_dir: artifactDir,
    backend_requested: create?.backend_requested || options.backend || null,
    backend_effective: create?.backend_effective || null,
    example_path: create?.lottie_file || options.example || null,
    transition_count: starts.length,
    render_sample_count: renderSamples.length,
    blank_frame_count: blankFrameCount,
    midpoint_equivalent_count: midpointEquivalentCount,
    jump_frame_count: jumpFrameCount,
    endpoint_mismatch_count: endpointMismatchCount,
    captured_frames: capturedFrames,
    checks: {
      plugin_loaded: pluginLoaded,
      transition_started: starts.length > 0,
      transition_stopped: stops.length > 0,
      midpoint_captured: midpoints.length > 0,
      no_blank_frames: blankFrameCount === 0,
      no_midpoint_endpoint_regression: midpointEquivalentCount === 0,
      no_jump_spikes: jumpFrameCount === 0,
      endpoint_consistency: endpointMismatchCount === 0,
      no_plugin_errors: !errors.some((error) => error.includes('Plugin log contains')),
    },
    errors,
  };

  summary.pass = errors.length === 0;
  return summary;
}

function writeSummary(artifactDir, summary) {
  const file = path.join(artifactDir, 'summary.json');
  fs.writeFileSync(file, `${JSON.stringify(summary, null, 2)}\n`);
}

if (require.main === module) {
  const artifactDir = process.argv[2];
  if (!artifactDir) {
    console.error('usage: node tests/obs-e2e-assert.js <artifact-dir>');
    process.exit(2);
  }

  const summary = summarizeRun(path.resolve(artifactDir));
  writeSummary(path.resolve(artifactDir), summary);

  if (!summary.pass) {
    console.error(JSON.stringify(summary, null, 2));
    process.exit(1);
  }

  process.stdout.write(`${JSON.stringify(summary, null, 2)}\n`);
}

module.exports = {
  summarizeRun,
  writeSummary,
};
