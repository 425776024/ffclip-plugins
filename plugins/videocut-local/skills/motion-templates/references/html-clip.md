# HTML animation authoring contract

## Document and clock

An authored item has `clip.type: "html-clip"`, `clip.assetId: ""` and the following `clip.html`:

```json
{
  "html": "<!doctype html><html><body>...</body></html>",
  "width": 1920,
  "height": 1080,
  "duration": 600000,
  "transparent": true,
  "variables": { "title": "VideoCut", "accent": "#34d1bf" }
}
```

`width` and `height` are the source DOM viewport in pixels, not the project canvas or preview thumbnail. Each side is an integer from 1 to 4096; the total is at most 8388608 pixels. HTML must be nonempty and at most 1 MiB of UTF-8 bytes after resource embedding. `duration` is an integer source extent from 1 tick to 24 hours. Variables are at most 100 named values: strings of at most 10000 characters, finite numbers or booleans; nested objects are unsupported.

The project uses 120000 ticks per second. Source time is mapped from placement, trim and constant speed before the renderer is called. A split begins at its preserved source in-point instead of restarting the HTML animation. Keep animation duration independent of the item’s shorter placement length. Extending a trim beyond the authored source duration fails; replacing a split’s HTML does not change its sibling.

## Deterministic animation

GSAP is injected locally before the authored scripts. Do not load GSAP from a CDN or start autonomous playback. Register paused timelines in `window.__timelines`, for example:

```js
const timeline = gsap.timeline({ paused: true });
timeline.fromTo(".title", { opacity: 0, x: -80 },
  { opacity: 1, x: 0, duration: 0.5, ease: "power2.out" }, 0);
timeline.to(".title", { opacity: 0, duration: 0.4 }, 4.6);
window.__timelines.title = timeline;
```

For 2D graphics, set `gsap.defaults({ force3D: false })` before creating tweens, as the starter does. Chromium's automatically promoted GPU layers can rasterize borders and shadows differently after an otherwise identical reverse seek. Prefer explicit 3D transforms only when the design needs them, and sample those scenes in both time directions.

The renderer pauses and seeks registered timelines to source seconds on every frame, including backward seeks. CSS and Web Animations API animations are paused and their `currentTime` is set in milliseconds. These animations must be tied to absolute source time, not event-driven page interaction.

For canvas, SVG or other direct state updates, define either `window.tick` or `window.__videocut.tick`:

```js
window.tick = function (seconds, context) {
  const progress = Math.max(0, Math.min(1, seconds / context.duration));
  document.querySelector(".progress").style.width = `${progress * 100}%`;
};
```

`context` contains `{ width, height, duration, variables }`; its `duration` is in seconds. The callback runs after GSAP and CSS animation seeking and may return a Promise. Prefer one clock mechanism for a given property to prevent the callback from overriding its GSAP animation unintentionally.

The runtime sets `window.__videocutVariables` before authored scripts and `window.__videocutTime` before each frame. Optional `window.__videocutReady` is a Promise the runtime awaits before capturing; use it for finite initialization of embedded assets. The runtime also waits for fonts and decoded images.

Every requested frame must depend on the requested absolute time and frozen variables. Update all state needed for that time. Do not increment counters, append particles on each call, integrate velocity from the previous frame, use `Date.now()` or start `requestAnimationFrame`, intervals or wall-clock playback. Use a stable seed if randomness is part of the design. A fresh evaluation of 0.4 seconds and a seek back to 0.4 seconds after 3 seconds must produce the same graphic.

## Transparent graphics and resources

With `transparent: true`, use transparent `html`, `body` and full-canvas containers. Add opaque panels only where the design needs them:

```css
html, body { margin: 0; width: 100%; height: 100%; background: transparent; overflow: hidden; }
```

The alpha PNG enters the shared compositor, which applies the item’s position, scale, rotation, opacity, cropping, blend mode, effects and transitions. Alpha stays transparent until the graphic is composited over the project’s other layers. MP4/WebM then encode that completed canvas; this does not add an alpha channel to the whole exported video.

Inline payloads should be self-contained: inline scripts/styles and `data:` image/font resources. Local-file import can read relative JS, stylesheet links, CSS `url(...)`, image `src` and SVG image references inside the plugin’s authorized roots and freeze them into the document. Remote dependencies, fetch-based initialization, JS module dependency graphs, CSS `@import` and `srcset` are unsupported. Merge imports or bundle scripts first. The runtime blocks network and file URL access.

Do not embed `video`, `audio`, `iframe`, `object` or `embed`; native media stays on existing tracks. Core HTML validation conservatively rejects literal audio/video tag syntax even in comments or script strings. The browser parser also rejects prohibited elements, including elements created during a tick.

## Tools and edit commands

`list_motion_templates` takes no arguments and reports native templates/components plus the local HTML renderer’s capability. For existing sessions use the session supplied by the user; for new cuts use `create_session` and its returned preview URL.

Local-file import example, with a real authorized absolute path:

```json
{
  "id": "session-id",
  "path": "/authorized/graphics/lower-third.html",
  "width": 1920,
  "height": 1080,
  "duration": 600000,
  "transparent": true,
  "variables": { "title": "本地动画", "subtitle": "可编辑时间轴" },
  "name": "Lower third",
  "start": 240000,
  "length": 600000
}
```

These are arguments to `add_html_clip`, not a shell command. Supply either `path` or a full `html` object. `html` object fields already hold the viewport, source duration, transparency and variables. `trackId` is optional and must reference an existing unlocked video track.

For a batch, raw `edit_timeline` commands accept complete inline payloads:

```js
const payload = { html: generatedHTML, width: 1920, height: 1080,
  duration: 600000, transparent: true, variables: { title: "本地动画" } };
// Submit with the id/version just read from get_session.
const operations = [
  { action: "add_html_clip", html: payload, name: "Title", start: 240000, length: 600000 },
];
// To change an existing graphic instead:
const replacement = { action: "set_html_clip", itemId: existingItemId, html: payload };
```

Raw HTML add/replace fields use ticks. Ordinary timeline commands keep their established seconds fields: `trim_clip.sourceInSeconds`, `trim_clip.durationSeconds`, `split_clip.atSeconds`, `move_clip.startSeconds`. `set_property` uses its descriptor unit; time properties are ticks and `time.speed` is a multiplier. Read shared properties with `get_properties` instead of inventing new HTML-only transform commands.

## Verification and output limits

Check both source animation and timeline integration: seek near entrance/middle/exit, return to earlier source times, drag the playhead backward/forward, and play alongside the intended media. Ensure no opaque full-frame background hides lower tracks. Keep the session preview open for video export and inspect the output’s decoded frames and media streams when making export claims.

Installed Chrome/Chromium or an absolute `VIDEOCUT_CHROMIUM` executable path is required. The server starts a private headless profile and returns captured frames to the Codex/browser preview. Real-time performance depends on the HTML complexity, source resolution and machine; frame-addressable seeking does not guarantee every scene maintains the output frame rate.

Use `save_project` with the latest session version to save the entire project as a portable `.vcutweb` directory. It freezes media, editable HTML and variables, flower recipes/styles, effects, keys and template resources. Move the entire directory and reopen with `open_project`; source HTML and media files are no longer required. Native `.vcut` export supports custom flower recipes and per-clip styles through the updated bridge (`customTextRecipes:1`, `textTemplateStyles:1`); HTML requires `.vcutweb`. Transparent whole-video output and arbitrary HyperFrames project import are not implemented. An imported compatible graphics HTML document is supported, not every HyperFrames project dependency or feature.
