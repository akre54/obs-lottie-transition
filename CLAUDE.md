# obs-lottie-transition

## Project Overview

OBS plugin that uses Lottie animations to drive scene transitions. A private CEF browser source renders Lottie via lottie-web, producing matte textures and transform data that the C plugin composites with scene A/B via a custom shader.

## Architecture

- **C plugin** (`src/lottie-transition.c/h`) — OBS_SOURCE_TYPE_TRANSITION, manages browser source lifecycle, GPU rendering pipeline
- **Bridge JS** (`data/web/bridge.js`) — Self-driving animation renderer. Loads lottie data, auto-plays via requestAnimationFrame, renders 3 regions + data strip
- **Shader** (`data/lottie_transition.effect`) — HLSL matte compositing (scene_a * matte_a + scene_b * matte_b + overlay)
- **Transform decode** (`src/transform-decode.c/h`) — Pixel data strip → float transform extraction

## Browser Canvas Layout

Canvas = outputHeight × 3 + 2 (DATA_STRIP_HEIGHT)
- Region 0: MatteA (white = show scene A)
- Region 1: MatteB (white = show scene B)
- Region 2: Overlay/decorative elements
- Bottom 2px: Data strip encoding transform floats as RGBA pixels

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
- `data:text/html,<small inline HTML>` — confirmed working up to ~400KB of simple content
- `obs_source_inc_showing()` + `obs_source_inc_active()` — required for private sources to tick/render
- Inline `<script>` tags in data: URLs execute correctly

### What DOES NOT WORK
- **`file://` URLs** — CEF silently fails to load ANY file:// URL for private browser sources, regardless of path (even /tmp with no spaces). The page never loads, zero console messages.
- **`exec_browser_js` (proc_handler "javascript")** — Silently dropped for private browser sources. Even tiny scripts have zero effect. This rules out ALL runtime JS injection.
- **`data:text/html;base64,` with large payloads (~425KB)** — Page loads (CSS background renders as opaque black) but JS doesn't execute. Cause unknown.
- **Raw unencoded `data:text/html,` with `#` or `%` chars** — `#` acts as URL fragment delimiter (truncates content). `%` triggers percent-decode (corrupts content). These chars MUST be encoded.
- **`obs_source_add_active_child()`** — Doesn't propagate activation when parent transition isn't active

### Current Blocker
Inlining lottie.min.js (305KB) + bridge.js (8KB) + animation JSON into a data: URL. The HTML is valid (confirmed working when opened as a file in Chrome). But in a CEF data: URL:
- Raw `data:text/html,` — broken by `#` and `%` chars in JS
- `data:text/html,` with `#`→`%23` and `%`→`%25` encoding — page loads (opaque black bg) but JS doesn't execute
- `data:text/html;base64,` — same result, page loads but JS doesn't execute

Small data: URLs (<1KB) with inline JS work perfectly. The issue appears to be size-related for data: URLs containing actual JS (vs padding with `A` characters).

### Potential Next Approaches (not yet tried)
1. **Local HTTP server** — Start a tiny HTTP server (e.g., on localhost:random_port) from the plugin, serve the HTML page. This avoids all file:// and data: URL issues.
2. **Non-private browser source** — Use `obs_source_create()` instead of `_private()`. May fix file:// and exec_browser_js. Risk: source appears in OBS source lists, duplicate name issues.
3. **Split the data: URL** — Use a small data: URL that dynamically creates and executes script elements via `document.createElement('script')` + `script.text = ...`, loading the large JS from a global variable set in a separate smaller data: URL.
4. **CDN lottie-web** — Load lottie.min.js from CDN (e.g., cdnjs) via `<script src="https://...">` in the data: URL page, only inline bridge.js + animation data.

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
