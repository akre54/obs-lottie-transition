# obs-lottie-transition

## Project Overview

OBS plugin that uses Lottie animations to drive scene transitions. A private CEF browser source renders Lottie via lottie-web, producing matte textures and transform data that the C plugin composites with scene A/B via a custom shader.

## Architecture

- **C plugin** (`src/lottie-transition.c/h`) — OBS_SOURCE_TYPE_TRANSITION, manages browser source lifecycle, GPU rendering pipeline
- **Bridge JS** (`data/web/bridge.js`) — Self-driving animation renderer. Loads lottie data, auto-plays via requestAnimationFrame, channel-packs mattes into single canvas
- **Shader** (`data/lottie_transition.effect`) — HLSL matte compositing from channel-packed browser texture
- **Transform decode** (`src/transform-decode.c/h`) — Pixel data strip → float transform extraction

## Channel-Packing (Browser Canvas Layout)

Single 1920x1080 canvas with channel-packed data:
- **R channel** = MatteA luminance (white = show scene A)
- **G channel** = MatteB luminance (white = show scene B)
- **B channel** = Overlay alpha (blend factor for white overlay)
- **A channel** = Always 255 (browser composites away alpha=0 pixels)

Three off-screen lottie instances render separately, then `getImageData`/`putImageData` packs the channels. Previous approach of stacking 3 regions vertically failed because the browser source only renders the viewport height and stretches it.

## Build / Install / Test

```bash
cmake --build build_macos --config RelWithDebInfo
rm -rf "$HOME/Library/Application Support/obs-studio/plugins/obs-lottie-transition.plugin"
cp -R build_macos/RelWithDebInfo/obs-lottie-transition.plugin "$HOME/Library/Application Support/obs-studio/plugins/"
codesign --force --deep --sign - "$HOME/Library/Application Support/obs-studio/plugins/obs-lottie-transition.plugin"
```

Then restart OBS. Select "Lottie Transition" in scene transition dropdown, configure a .json file.

## Critical: Private Browser Source Constraints (CEF/OBS)

These are hard-won findings from extensive debugging. **Do not re-attempt failed approaches.**

### What WORKS for `obs_source_create_private("browser_source", ...)`
- `is_local_file=true` + `local_file=/path/to.html` — **CONFIRMED WORKING** (2026-03-29). JS executes, no size limits. Uses CEF's `http://absolute/` scheme handler internally, completely different code path from `file://` URLs. This is the current approach.
- `<script src='relative.js'>` with `is_local_file` — **CONFIRMED WORKING** (2026-03-29). Relative paths resolve against the HTML file's directory via `http://absolute/` scheme. External JS loads and executes. This avoids the large-inline-JS-in-data-URL issue.
- Large inline HTML (300KB+) with `is_local_file` — **DOES NOT WORK**. CSS renders but JS doesn't execute, same symptom as large data: URLs. The issue is CEF-level, not URL-scheme-level.
- `data:text/html,<small inline HTML>` — confirmed working up to ~400KB of simple content
- `obs_source_inc_showing()` + `obs_source_inc_active()` — required for private sources to tick/render
- Inline `<script>` tags in data: URLs execute correctly

### What DOES NOT WORK
- **`file://` URLs** — CEF silently fails to load ANY file:// URL for private browser sources, regardless of path (even /tmp with no spaces). The page never loads, zero console messages.
- **`exec_browser_js` (proc_handler "javascript")** — Silently dropped for private browser sources. Even tiny scripts have zero effect. This rules out ALL runtime JS injection.
- **`restart_when_active` + active toggle for page reload** — Does NOT reliably reload the page on subsequent transitions. First load works, but `dec_active/inc_active` toggle does not trigger a fresh page load. Must destroy and recreate the browser source for each transition instead.
- **Blocking `<script src='file.js'>` tags** — Kills ALL subsequent inline `<script>` blocks in CEF private browser sources. Must use dynamic `document.createElement('script')` with onload callback instead.
- **`console.log()` for debugging** — CEF only forwards `LOGSEVERITY_ERROR` and `LOGSEVERITY_FATAL` to OBS logs. Use `console.error()` instead for messages to appear in OBS log files.
- **`data:text/html;base64,` with large payloads (~425KB)** — Page loads (CSS background renders as opaque black) but JS doesn't execute. Cause unknown.
- **Raw unencoded `data:text/html,` with `#` or `%` chars** — `#` acts as URL fragment delimiter (truncates content). `%` triggers percent-decode (corrupts content). These chars MUST be encoded.
- **`obs_source_add_active_child()`** — Doesn't propagate activation when parent transition isn't active
- **`lottie.loadAnimation({ rendererSettings: { canvas: myCanvas } })` with a container** — lottie-web ignores the `canvas` param when a container is also provided. It creates its own canvas inside the container. Must grab the actual canvas via `animInstance.renderer.canvasContext.canvas` after DOMLoaded. Also, the container MUST have real dimensions (not `width:0;height:0`) or lottie's canvas will be 0x0.
- **Tall canvas (e.g. 1920x3242) for multi-region rendering** — The browser source renders only the viewport-height portion of the page content and stretches/scales it to fill the configured `height`. A canvas taller than the viewport results in only the top viewport-height pixels being captured, repeated across the full texture. Use channel-packing into a single viewport-sized canvas instead.
- **Canvas pixels with alpha=0** — The browser source composites away fully transparent pixels. Even if R/G/B contain data, setting A=0 causes the browser to output black/transparent. Always set alpha=255 when encoding data into canvas pixels.

### Resolved: Large JS Payload Loading

**Solution**: Use `is_local_file`/`local_file` browser source settings with external `<script src>` tags. Write a small HTML file to /tmp that references JS files (lottie.min.js, bridge.js, config, anim data) via relative `<script src>` paths. The JS files are copied to the same /tmp directory. This keeps the HTML small and avoids the large-inline-JS execution failure.

Previous failed approaches for reference:

- data: URLs with large JS (~300KB+) — CSS renders but JS doesn't execute
- `file://` URLs — silently fail for private sources
- `exec_browser_js` — silently dropped for private sources

## OBS API Notes
- `gs_stagesurface_copy()` takes only 2 args (stagesurf, texture), no region
- `gs_draw_sprite_subregion()` for extracting texture sub-regions
- Don't nest `gs_texrender_begin/end` calls
- `-Werror` is on — unused parameters need `UNUSED_PARAMETER()` macro
- `lt_create` is called twice by OBS (two transition instances)
- `transition_stop` fires before `transition_start` on first use (settings apply triggers it)

## Lottie Animation Convention
- Layer `[SlotA]` / `[SlotB]` — transform-only placeholders (hidden during render)
- Layer `[MatteA]` / `[MatteB]` — matte masks
- All other layers — decorative overlay
- Animation JSON files go in `examples/`

## Data Strip Encoding
- 6 floats per slot × 2 slots = 12 floats
- 2 floats per RGBA pixel = 6 pixels total (3 per slot)
- Ranges: pos [-4096,4096], scale [0,10], rotation [-360,360], opacity [0,1]
- Magic marker at pixel 6: 0xCAFEBABE
