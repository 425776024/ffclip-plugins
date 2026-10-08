# PAGX in VideoCut

The bundled [official PAGX Skill](../../pagx/SKILL.md) supplies authoring, XML syntax, patterns and CLI references. Its original files are preserved. VideoCut adds timeline integration and the tested runtime boundary below.

`list_motion_templates` exposes `templates` and `pagx` capabilities. `add_pagx_clip` accepts exactly one of `templateId`, `pagx: {xml,width,height,duration,transparent}`, or authorized local `.pagx` `path`. Template `locale` is `zh` or `en`. Local images are embedded; external font file references must first be expanded with the official CLI. The preview loads installed system font families; faux bold/italic are explicit styling controls, not a promise of exact font-weight matching. Embedded glyphs retain their authored outlines.

Authoring through `add_html_clip` defaults to conversion into PAGX; conversion failure returns an HTML clip with a visible reason. `convert_html_to_pagx` remains available for a conversion-only review. Never silently treat a fallback as PAGX. Follow [the HTML contract](html-clip.md) before creating the source.

Use one finite top-level `Animations/Animation` with `loop="once"`. The VideoCut clock seeks this animation using 120000 ticks per second. Nested timelines, state machines, 3D and path-control-point animation are rejected; use supported layer transforms, opacity and masks. Put video/audio on regular media tracks.

Clicking a PAGX clip opens parsed content properties. Text, font family, size, faux bold/italic, colors, shape sizes, position, corner radius and alpha are editable when not controlled by a channel. Use meaningful `name` attributes on native `Layer` elements. Internal channel values are read-only in this panel; edit the XML through `set_pagx_clip` to change those curves. Duration changes proportionally rescale internal key times and must still cover the existing clip source out-point. The editor debounces typing, supports IME and applies one reversible source replacement.

A native CLI render may differ from the pinned WASM viewer. Validate representative frames with VideoCut itself, especially fonts, gradients and strokes. `save_project` preserves editable `.vcutweb`; MP4/WebM uses VideoCut's own compositor/encoder. Neither requires a PAG Enterprise SDK. The HTML converter requires the pinned open-source CLI (`npm run build:pagx` or `VIDEOCUT_PAGX_CLI`); native PAGX rendering does not.
