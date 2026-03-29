# Creating Lottie Transitions in After Effects

## Quick Start

1. Create a new comp at your OBS output resolution (e.g. 1920x1080), 30fps
2. Add specially-named layers (see below)
3. Animate them over 1 second (30 frames)
4. Export with Bodymovin to `.json`
5. Load in OBS as a Lottie Transition

## Layer Naming Convention

The plugin looks for these exact layer names:

| Layer Name | Type in AE | What it does |
|---|---|---|
| `[MatteA]` | Shape or Solid | Grayscale mask for the **outgoing** scene. White = visible, black = hidden. |
| `[MatteB]` | Shape or Solid | Grayscale mask for the **incoming** scene. White = visible, black = hidden. |
| `[SlotA]` | Null Object | Transform carrier for the outgoing scene (position, scale, rotation, opacity) |
| `[SlotB]` | Null Object | Transform carrier for the incoming scene |
| Anything else | Any | Rendered as a decorative overlay on top of the transition |

## Three Modes

### Matte-Only (simplest)

Only `[MatteA]` and `[MatteB]` layers. Works like a stinger/wipe.

**Example — Circle Reveal:**

1. New comp: 1920x1080, 30fps, 1 second
2. Add a Shape Layer, rename it `[MatteA]`
3. Add an Ellipse path (centered, size 2500x2500 to cover the whole comp)
4. Add a white Fill
5. Keyframe the Ellipse Size from `[2500, 2500]` at frame 0 to `[0, 0]` at frame 30
6. Apply ease (F9)
7. Duplicate the layer, rename to `[MatteB]`
8. On `[MatteB]`, reverse the keyframes: `[0, 0]` at frame 0 → `[1920, 1080]` at frame 30

**Key rule:** At frame 0, `[MatteA]` should be fully white (outgoing scene visible) and `[MatteB]` fully black. At frame 30, the reverse.

### Transform-Only

Only `[SlotA]` and `[SlotB]` null layers. Scenes move/scale/rotate with a simple crossfade.

**Example — Slide Left:**

1. New comp: 1920x1080, 30fps, 1 second
2. Add a Null Object, rename it `[SlotA]`
3. Set anchor point to [960, 540] (comp center)
4. Keyframe Position from [960, 540] → [-960, 540] (slides off left)
5. Add another Null Object, rename it `[SlotB]`
6. Keyframe Position from [2880, 540] → [960, 540] (slides in from right)
7. Apply ease to both

### Hybrid (matte + transform)

All four layers. Scenes move AND are masked through animated shapes.

**Example — Slide + Wipe:**

1. Create `[SlotA]` and `[SlotB]` as above (sliding)
2. Create `[MatteA]` and `[MatteB]` (a wipe or shape reveal)
3. Now scenes slide AND get masked simultaneously

## Adding Decorative Overlays

Any layer NOT named `[SlotA/B]` or `[MatteA/B]` is rendered as a decorative overlay on top of the composited scenes.

**Ideas:**
- Animated particles or confetti
- A colored bar or line that sweeps across during the transition
- A frame or border that appears mid-transition
- Your logo or branding element
- Light flares or bokeh effects

Just animate them normally in the comp. They'll render on top.

## Authoring Rules

1. **Comp size must match OBS output** (e.g. 1920x1080)
2. **Matte layers must be grayscale** — only the red channel is sampled. Use white fills on black background.
3. **Slot layers are invisible** — they're data carriers. Don't put visible content on them.
4. **Slot layers start at comp center** — position [960, 540] = identity (no offset). Scale 100% = identity.
5. **Frame 0 = start of transition** (outgoing scene fully visible)
6. **Last frame = end of transition** (incoming scene fully visible)
7. **Duration is flexible** — the plugin stretches/compresses the animation to match OBS transition duration
8. **Avoid 3D layers** — the plugin flattens to 2D transforms only
9. **Expressions work** — as long as Bodymovin can bake them

## Export with Bodymovin

1. Install the Bodymovin extension: Window → Extensions → Bodymovin
2. Select your comp
3. Settings:
   - **Standard** export
   - **Glyphs** off (no text needed)
   - **Demo** off
   - Format: JSON
4. Click Render
5. Save as `.json`

If you don't have Bodymovin, use the LottieFiles AE plugin or `npx @nicedoc/lottie-exporter`.

## Testing Your File

Before loading in OBS, preview in the browser:

1. Open `data/web/index.html` with query params:
   ```
   file:///path/to/index.html?lottieFile=file:///path/to/your-transition.json&width=1920&height=1080
   ```
2. Open browser console, run: `window.seekFrame(0)` through `window.seekFrame(29)`
3. Check that matte regions render correctly (top = matteA, middle = matteB, bottom = overlay)

## Tips

- **Ease everything** — linear keyframes look mechanical. Use Easy Ease (F9) or custom bezier curves.
- **Overlap the mattes** — a brief moment where both MatteA and MatteB show some white creates a smooth crossfade zone rather than a hard cut.
- **Keep it short** — 0.5–1.5 seconds works best for transitions. Longer feels sluggish.
- **Test at 1x speed** — previewing in AE at reduced speed can be misleading.
- **Solid backgrounds help debug** — temporarily add a red solid behind MatteA and a blue solid behind MatteB to visualize what each matte reveals.
