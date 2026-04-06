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

function sampleDistance(a, b) {
  return Math.abs((a?.r || 0) - (b?.r || 0)) +
    Math.abs((a?.g || 0) - (b?.g || 0)) +
    Math.abs((a?.b || 0) - (b?.b || 0));
}

function dominant(sample, channel, floor = 0, lead = 0) {
  if (!sample) {
    return false;
  }

  const value = sample[channel];
  const others = ['r', 'g', 'b'].filter((key) => key !== channel).map((key) => sample[key]);
  return value >= floor && others.every((other) => value >= other + lead);
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

function applyBehaviorChecks(summary, events, options, errors) {
  if (!options.behaviorChecks) {
    return;
  }

  const exampleName = path.basename(summary.example_path || options.example || '');
  const triggers = byTrigger(events);

  if (exampleName === 'slide-and-mask.json') {
    for (const [triggerIndex, samples] of triggers.entries()) {
      const early = samples.find((event) => event.bucket_percent === 25);
      const right = early?.sample_right_mid;
      if (!dominant(right, 'g', 140, 60)) {
        errors.push(`Trigger ${triggerIndex} did not show transformed slot content on the entering edge`);
      }
    }
  }

  if (exampleName === 'simple-wipe.json') {
    for (const [triggerIndex, samples] of triggers.entries()) {
      const middle = samples.find((event) => event.bucket_percent === 50);
      const end = samples.find((event) => event.bucket_percent === 100);
      const center = middle?.sample_center;
      const leftEnd = end?.sample_left_mid;
      const rightEnd = end?.sample_right_mid;
      const centerLooksMixed = center && center.r >= 70 && center.b >= 70;
      if (!centerLooksMixed || !dominant(leftEnd, 'b', 120, 40) || !dominant(rightEnd, 'b', 120, 40)) {
        errors.push(`Trigger ${triggerIndex} did not behave like the centered wipe control`);
      }
    }
  }

  if (exampleName === 'circle-reveal.json') {
    for (const [triggerIndex, samples] of triggers.entries()) {
      const end = samples.find((event) => event.bucket_percent === 100);
      const center = end?.sample_center;
      const edge = end?.sample_edge_25;
      if (!center || !edge) {
        errors.push(`Trigger ${triggerIndex} is missing spatial samples for circle-reveal`);
        continue;
      }
      if (sampleDistance(center, edge) < 120) {
        errors.push(`Trigger ${triggerIndex} near-end frame looked like a cut instead of an outside-in reveal`);
      }
      if (!dominant(center, 'r', 80, 20) || !dominant(edge, 'b', 80, 20)) {
        errors.push(`Trigger ${triggerIndex} did not keep the center and edge separated near the end of circle-reveal`);
      }
    }
  }

  if (exampleName === 'sliding-window.json') {
    for (const [triggerIndex, samples] of triggers.entries()) {
      const early = samples.find((event) => event.bucket_percent === 25);
      const middle = samples.find((event) => event.bucket_percent === 50);
      const end = samples.find((event) => event.bucket_percent === 100);
      const earlyLeft = early?.sample_left_mid;
      const earlyRight = early?.sample_right_mid;
      const middleCenter = middle?.sample_center;
      const endLeft = end?.sample_left_mid;
      const endRight = end?.sample_right_mid;

      if (!dominant(earlyLeft, 'r', 120, 40) || !dominant(earlyRight, 'b', 120, 40)) {
        errors.push(`Trigger ${triggerIndex} did not show the moving reveal window entering from the right`);
      }
      if (!dominant(middleCenter, 'b', 120, 40)) {
        errors.push(`Trigger ${triggerIndex} midpoint did not bring the incoming scene into the reveal window`);
      }
      if (!dominant(endLeft, 'b', 120, 40) || !dominant(endRight, 'b', 120, 40)) {
        errors.push(`Trigger ${triggerIndex} did not finish with the incoming scene filling the frame`);
      }
    }
  }
}

function summarizePerfEvents(events) {
  const perfEvents = events
    .filter((event) => event.event === 'perf_summary')
    .sort((a, b) => a.transition_index - b.transition_index);
  if (perfEvents.length === 0) {
    return null;
  }

  const totals = {
    transition_count: perfEvents.length,
    avg_render_fps: 0,
    min_render_fps: Number.POSITIVE_INFINITY,
    avg_render_gap_ms: 0,
    max_render_gap_ms: 0,
    avg_callback_ms: 0,
    max_callback_ms: 0,
    avg_backend_ms: 0,
    max_backend_ms: 0,
    avg_backend_pass_ms: 0,
    max_backend_pass_ms: 0,
    avg_backend_slot_ms: 0,
    max_backend_slot_ms: 0,
    avg_backend_pack_ms: 0,
    max_backend_pack_ms: 0,
    avg_backend_upload_ms: 0,
    max_backend_upload_ms: 0,
    avg_composite_ms: 0,
    max_composite_ms: 0,
    gap_over_20ms: 0,
    gap_over_33ms: 0,
    gap_over_50ms: 0,
  };

  const transitions = perfEvents.map((event) => ({
    transition_index: event.transition_index,
    transition_ms: event.transition_ms || 0,
    render_count: event.render_count || 0,
    render_fps: event.render_fps || 0,
    avg_render_gap_ms: event.avg_render_gap_ms || 0,
    max_render_gap_ms: event.max_render_gap_ms || 0,
    avg_callback_ms: event.avg_callback_ms || 0,
    max_callback_ms: event.max_callback_ms || 0,
    avg_backend_ms: event.avg_backend_ms || 0,
    max_backend_ms: event.max_backend_ms || 0,
    avg_backend_pass_ms: event.avg_backend_pass_ms || 0,
    max_backend_pass_ms: event.max_backend_pass_ms || 0,
    avg_backend_slot_ms: event.avg_backend_slot_ms || 0,
    max_backend_slot_ms: event.max_backend_slot_ms || 0,
    avg_backend_pack_ms: event.avg_backend_pack_ms || 0,
    max_backend_pack_ms: event.max_backend_pack_ms || 0,
    avg_backend_upload_ms: event.avg_backend_upload_ms || 0,
    max_backend_upload_ms: event.max_backend_upload_ms || 0,
    avg_composite_ms: event.avg_composite_ms || 0,
    max_composite_ms: event.max_composite_ms || 0,
    gap_over_20ms: event.gap_over_20ms || 0,
    gap_over_33ms: event.gap_over_33ms || 0,
    gap_over_50ms: event.gap_over_50ms || 0,
  }));

  for (const transition of transitions) {
    totals.avg_render_fps += transition.render_fps;
    totals.min_render_fps = Math.min(totals.min_render_fps, transition.render_fps);
    totals.avg_render_gap_ms += transition.avg_render_gap_ms;
    totals.max_render_gap_ms = Math.max(totals.max_render_gap_ms, transition.max_render_gap_ms);
    totals.avg_callback_ms += transition.avg_callback_ms;
    totals.max_callback_ms = Math.max(totals.max_callback_ms, transition.max_callback_ms);
    totals.avg_backend_ms += transition.avg_backend_ms;
    totals.max_backend_ms = Math.max(totals.max_backend_ms, transition.max_backend_ms);
    totals.avg_backend_pass_ms += transition.avg_backend_pass_ms;
    totals.max_backend_pass_ms = Math.max(totals.max_backend_pass_ms, transition.max_backend_pass_ms);
    totals.avg_backend_slot_ms += transition.avg_backend_slot_ms;
    totals.max_backend_slot_ms = Math.max(totals.max_backend_slot_ms, transition.max_backend_slot_ms);
    totals.avg_backend_pack_ms += transition.avg_backend_pack_ms;
    totals.max_backend_pack_ms = Math.max(totals.max_backend_pack_ms, transition.max_backend_pack_ms);
    totals.avg_backend_upload_ms += transition.avg_backend_upload_ms;
    totals.max_backend_upload_ms = Math.max(totals.max_backend_upload_ms, transition.max_backend_upload_ms);
    totals.avg_composite_ms += transition.avg_composite_ms;
    totals.max_composite_ms = Math.max(totals.max_composite_ms, transition.max_composite_ms);
    totals.gap_over_20ms += transition.gap_over_20ms;
    totals.gap_over_33ms += transition.gap_over_33ms;
    totals.gap_over_50ms += transition.gap_over_50ms;
  }

  const count = transitions.length;
  totals.avg_render_fps /= count;
  totals.avg_render_gap_ms /= count;
  totals.avg_callback_ms /= count;
  totals.avg_backend_ms /= count;
  totals.avg_backend_pass_ms /= count;
  totals.avg_backend_slot_ms /= count;
  totals.avg_backend_pack_ms /= count;
  totals.avg_backend_upload_ms /= count;
  totals.avg_composite_ms /= count;
  if (!Number.isFinite(totals.min_render_fps)) {
    totals.min_render_fps = 0;
  }

  return { transitions, totals };
}

function applyPerfChecks(summary, options, errors) {
  if (!options.perfChecks) {
    return;
  }

  const perf = summary.perf;
  if (!perf || perf.transitions.length === 0) {
    errors.push('No perf_summary events captured');
    return;
  }

  const budgets = {
    minRenderFps: options.minRenderFps || 30,
    maxAvgRenderGapMs: options.maxAvgRenderGapMs || 40,
    maxMaxRenderGapMs: options.maxMaxRenderGapMs || 85,
    maxAvgBackendMs: options.maxAvgBackendMs || 22,
    maxAvgCallbackMs: options.maxAvgCallbackMs || 28,
  };

  for (const transition of perf.transitions) {
    if (transition.render_fps < budgets.minRenderFps) {
      errors.push(
        `Transition ${transition.transition_index} render fps ${transition.render_fps.toFixed(2)} fell below ${budgets.minRenderFps}`
      );
    }
    if (transition.avg_render_gap_ms > budgets.maxAvgRenderGapMs) {
      errors.push(
        `Transition ${transition.transition_index} average render gap ${transition.avg_render_gap_ms.toFixed(2)}ms exceeded ${budgets.maxAvgRenderGapMs}ms`
      );
    }
    if (transition.max_render_gap_ms > budgets.maxMaxRenderGapMs) {
      errors.push(
        `Transition ${transition.transition_index} max render gap ${transition.max_render_gap_ms.toFixed(2)}ms exceeded ${budgets.maxMaxRenderGapMs}ms`
      );
    }
    if (transition.avg_backend_ms > budgets.maxAvgBackendMs) {
      errors.push(
        `Transition ${transition.transition_index} backend render time ${transition.avg_backend_ms.toFixed(2)}ms exceeded ${budgets.maxAvgBackendMs}ms`
      );
    }
    if (transition.avg_callback_ms > budgets.maxAvgCallbackMs) {
      errors.push(
        `Transition ${transition.transition_index} callback time ${transition.avg_callback_ms.toFixed(2)}ms exceeded ${budgets.maxAvgCallbackMs}ms`
      );
    }
  }
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
  const exampleName = path.basename(options.example || '');
  const visualChecks = options.visualChecks !== false;
  const skipGenericMidpointRegression =
    visualChecks &&
    options.behaviorChecks &&
    (exampleName === 'simple-wipe.json' ||
      exampleName === 'circle-reveal.json' ||
      exampleName === 'sliding-window.json');

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

  if (visualChecks && midpoints.length === 0) {
    errors.push('No midpoint render samples captured');
  }

  let blankFrameCount = 0;
  if (visualChecks) {
    for (const event of renderSamples) {
      if ((event.nonblack_ratio || 0) < 0.02 || (event.nonzero_alpha_ratio || 0) < 0.90) {
        blankFrameCount += 1;
      }
    }
    if (blankFrameCount > 0) {
      errors.push(`Detected ${blankFrameCount} blank/near-empty sampled frames`);
    }
  }

  let midpointEquivalentCount = 0;
  if (visualChecks) {
    for (const [triggerIndex, samples] of triggers.entries()) {
      const start = samples.find((event) => event.bucket_percent === 0);
      const middle = samples.find((event) => event.bucket_percent === 50);
      const end = samples.find((event) => event.bucket_percent === 100);
      if (!start || !middle || !end) {
        continue;
      }

      const startDistance = colorDistance(middle, start);
      const endDistance = colorDistance(middle, end);
      if (!skipGenericMidpointRegression && (startDistance < 30 || endDistance < 30)) {
        midpointEquivalentCount += 1;
        errors.push(`Trigger ${triggerIndex} midpoint stayed too close to an endpoint`);
      }
    }
  }

  let jumpFrameCount = 0;
  if (visualChecks) {
    for (const event of renderSamples) {
      if ((event.mean_abs_rgb_delta || 0) > 160) {
        jumpFrameCount += 1;
      }
    }
    if (jumpFrameCount > 0) {
      errors.push(`Detected ${jumpFrameCount} sampled discontinuity spikes`);
    }
  }

  let endpointMismatchCount = 0;
  if (visualChecks) {
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
  const perf = summarizePerfEvents(events);
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
    perf,
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

  if (visualChecks) {
    applyBehaviorChecks(summary, events, options, errors);
  }
  applyPerfChecks(summary, options, errors);

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
