# Validation — 2026-09-29

## Executed

- Built Emscripten 4.0.9 + pinned Skia + copied VideoCut C++ into a real WASM binary (~9.42 MiB), checked module validity and artifact checksum.
- Seven Node tests passed against that binary: Chinese text replacement and retained frame ownership; deterministic animation seeking with differing pixels; explicit raster opt-in; missing fonts; native unsupported effect diagnostics; lifetime/input bounds; source provenance and package path/digest checks.
- Packed `@videocut/text-wasm@0.1.0` (24 files, approximately 3.6 MB compressed), installed it into an isolated consumer directory, and rendered Chinese using only the installed package plus copied template/font data. The consumer produced 294×225 cropped RGBA (264600 bytes). Its public TypeScript import also passed a strict NodeNext type check.
- Opened the standalone demo in the Codex in-app Chromium browser. Verified actual Chinese glyphs with red fill and white stroke, explicit Source Han Sans fallback, text input + Apply changing the rendered text, and native LUA-OPACITY template loading and rendering.
- Browser sample: LUA-OPACITY, text “文字”, 960×540, 60 distinct timestamps across five seconds: median render/copy/Canvas submission 1.90 ms, P95 3.40 ms, 60 requestAnimationFrame-paced samples in 992.8 ms. Continuous playback reported 60.0 FPS and paused successfully. This is one simple animation on the current machine, not an all-template performance guarantee or a compositor presentation latency measurement.
- Template sweep: 122 original packages, one midpoint sample at 640×360, text “文字 WASM”, per-render Worker budget 3000 ms. 68 returned nonempty pixels, 39 returned errors, 15 timed out. The 68 consist of 60 flower styles/scenes, 3 bubbles, and 5 animations. Some original flower templates lack CJK fallback; the sweep does not establish glyph correctness. The separate Chinese browser/test cases explicitly supply fallback fonts.

The complete per-template results are in template-audit.json. A timeout means only that this CPU sample exceeded its budget. An image result does not establish that all effects or all timestamps are correct. The demo disables failed/timed-out samples; the module preserves the corresponding diagnostics.

## Not accepted by this validation

Native desktop/Metal visual parity, all-template animation completeness, complex effects at real-time speed, video export, other browsers/mobile devices, and integration into the main VideoCut webpage editor have not been verified. The raster profile requires an explicit fallback choice and surfaces that warning.
