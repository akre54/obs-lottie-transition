# OBS Lottie Transition

OBS Studio transition plugin for Lottie-authored wipes, mattes, and transform-driven scene transitions.

It supports two render paths:

- `thorvg`: native matte rendering plus native slot transform evaluation
- `browser`: CEF + `lottie-web` fallback path

The repo also includes local end-to-end and performance harnesses that launch a real OBS process and validate rendered output.

## What It Supports

The plugin looks for these reserved layer names inside a Lottie JSON:

- `[MatteA]`: grayscale matte for the outgoing scene
- `[MatteB]`: grayscale matte for the incoming scene
- `[SlotA]`: transform carrier for the outgoing scene
- `[SlotB]`: transform carrier for the incoming scene

Any other layer is treated as decorative overlay content.

Example files live in [examples/sliding-window.json](/Users/adam/Projects/obs-lottie-transition/examples/sliding-window.json), [examples/spotlight-zoom.json](/Users/adam/Projects/obs-lottie-transition/examples/spotlight-zoom.json), [examples/diagonal-band.json](/Users/adam/Projects/obs-lottie-transition/examples/diagonal-band.json), [examples/slide-and-mask.json](/Users/adam/Projects/obs-lottie-transition/examples/slide-and-mask.json), [examples/simple-wipe.json](/Users/adam/Projects/obs-lottie-transition/examples/simple-wipe.json), and [examples/circle-reveal.json](/Users/adam/Projects/obs-lottie-transition/examples/circle-reveal.json).

## Build

macOS build:

```bash
cmake --preset macos
cmake --build --preset macos
```

The plugin bundle is produced at:

```text
build_macos/RelWithDebInfo/obs-lottie-transition.plugin
```

ThorVG is built from the vendored source automatically when `ENABLE_THORVG` is on in [CMakeLists.txt](/Users/adam/Projects/obs-lottie-transition/CMakeLists.txt).

## Install

Copy the built bundle into your OBS plugins directory:

```bash
cp -R build_macos/RelWithDebInfo/obs-lottie-transition.plugin \
  "$HOME/Library/Application Support/obs-studio/plugins/"
```

Then restart OBS.

## Use In OBS

1. Open OBS.
2. In the Scene Transitions panel, use `Add Configurable Transition`.
3. Choose `Lottie Transition`.
4. In Properties, select a Lottie JSON from this repo’s `examples/` directory.
5. Pick the renderer backend:
   - `ThorVG Native`
   - `Browser (CEF / lottie-web)`
6. Preview and apply the transition.

The default backend is `thorvg`.

## Authoring Notes

The authoring guide is in [examples/AUTHORING-GUIDE.md](/Users/adam/Projects/obs-lottie-transition/examples/AUTHORING-GUIDE.md).

The short version:

- Comp size should match the OBS output size.
- Mattes should be authored as grayscale shapes.
- Slot layers are data carriers, not visible artwork.
- Frame `0` is the start of the transition.
- The last frame is the end state.

## Tests

Unit and bridge tests:

```bash
ctest --test-dir build_macos
```

JS bridge tests directly:

```bash
node --test data/web/test/bridge-core.test.js data/web/test/examples.test.js
```

## OBS E2E

The live OBS harness is documented in [tests/OBS_E2E.md](/Users/adam/Projects/obs-lottie-transition/tests/OBS_E2E.md).

Single run:

```bash
node tests/obs-e2e-runner.js \
  --obs-app /Applications/OBS.app/Contents/MacOS/OBS \
  --plugin-build build_macos/RelWithDebInfo/obs-lottie-transition.plugin \
  --example examples/slide-and-mask.json \
  --backend thorvg \
  --triggers 1 \
  --fixture patterned \
  --behavior-checks on
```

Behavior matrix:

```bash
node tests/obs-e2e-matrix.js \
  --obs-app /Applications/OBS.app/Contents/MacOS/OBS \
  --plugin-build build_macos/RelWithDebInfo/obs-lottie-transition.plugin
```

Performance matrix:

```bash
node tests/obs-perf-matrix.js \
  --obs-app /Applications/OBS.app/Contents/MacOS/OBS \
  --plugin-build build_macos/RelWithDebInfo/obs-lottie-transition.plugin
```

These runs emit:

- `summary.json`
- `plugin-events.jsonl`
- `obs.log`
- sampled frame captures for visual runs

## Backend Notes

`thorvg` is the preferred path and now runs independently of the browser backend for matte rendering and slot transforms.

`browser` remains useful as a fallback implementation and comparison path when debugging example behavior.

## License

This repository is licensed under the Apache License 2.0. See [LICENSE](/Users/adam/Projects/obs-lottie-transition/LICENSE).
