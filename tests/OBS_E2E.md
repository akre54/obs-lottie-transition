# OBS E2E Harness

The local end-to-end harness launches a real OBS process with an isolated `HOME`, injects the built plugin through `OBS_PLUGINS_PATH`, drives scene transitions through `obs-websocket`, and captures machine-readable artifacts for regression analysis.

## Main Runner

Run the local harness with:

```bash
node tests/obs-e2e-runner.js \
  --obs-app /Applications/OBS.app/Contents/MacOS/OBS \
  --plugin-build build_macos/RelWithDebInfo/obs-lottie-transition.plugin \
  --example examples/slide-and-mask.json \
  --backend thorvg \
  --triggers 3
```

Artifacts are written under the selected run directory:

- `artifacts/plugin-events.jsonl`
- `artifacts/summary.json`
- `artifacts/obs.log`
- `artifacts/obs-process.log`
- `artifacts/frames/*.png`

The plugin-side telemetry is enabled automatically through:

- `LT_E2E_CAPTURE_DIR`
- `LT_E2E_TRACE=1`
- `LT_E2E_CAPTURE_FRAMES=1`

## Assertions

The runner writes `summary.json` automatically. To re-run assertions against an existing artifact directory:

```bash
node tests/obs-e2e-assert.js /tmp/obs-lottie-e2e/run-.../artifacts
```

The summary checks for:

- plugin load and transition lifecycle
- sampled midpoint capture
- blank or near-empty frames
- midpoint collapse back to an endpoint
- sampled discontinuity spikes
- endpoint-to-next-start consistency
- plugin log failures

## UI Smoke

The macOS smoke harness is intentionally separate from the regression runner:

```bash
swift tests/macos/obs-ui-smoke.swift \
  --obs-app /Applications/OBS.app/Contents/MacOS/OBS \
  --artifact-dir /tmp/obs-lottie-ui-smoke \
  --example examples/slide-and-mask.json
```

It captures screenshots and a simple action log while attempting the Properties workflow through Accessibility scripting. Treat it as a local smoke test, not a stable CI gate.
