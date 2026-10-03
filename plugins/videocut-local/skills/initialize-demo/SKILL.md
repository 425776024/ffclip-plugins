---
name: initialize-demo
description: Initialize VideoCut or ffclip after installation and visibly open its built-in cinematic project with animated gradient text, editable keyframes, continuous narration and captions. Use for a website setup prompt, onboarding or a request to show the built-in example.
---

Create the built-in 18-second cinematic example and display its live editing preview. Its five tracks contain an original coastal picture, deterministic HTML with staggered text entrances and moving gradients, native gradient flower titles, a continuous narration WAV and timed captions. The picture and titles include editable position, scale and opacity keyframes. No user's media, AI inference or model downloads are needed.

Call `initialize_demo` with `locale: "zh"` for Chinese or `locale: "en"` for English, matching the user's requested content language. Supply `name` only when useful. This creates a fresh independent session. Keep the returned `id` for subsequent edits and open its exact `previewUrl` automatically using the host's visible browser or preview tools. Do not replace an existing user project or stop after returning a URL. Leave the preview open so the user can inspect the animation, titles, narration and subtitle tracks.

Read and follow these instructions even when the host cannot persistently install skills. Confirm that the returned project has five populated tracks and that its preview visibly shows the coastal film and titles. The editor initially rests at 1.5 seconds so the opening title is visible; its first play starts at the beginning to retain the complete narration. A blank editor, a printed URL or MCP registration alone does not complete setup. Initialization uses packaged media and must not trigger AI model downloads.

If installation has just completed and the host has not loaded the MCP tools yet, run the installed CLI in a persistent background process or host terminal:

```sh
videocut-web --demo --demo-locale zh --open --port 0
```

Use `--demo-locale en` for English. Include the already authorized `--root` folders when available; the packaged example itself requires no media folder choice. Capture the `VideoCut:` preview URL from stderr and open that exact URL in the host preview if it supports one. Keep the process alive. Reuse it rather than starting a second blank editor. If the OS browser cannot open, use the printed URL with the available browser tools.

Register or reconnect the local MCP service according to the host's supported setup and preserve its existing connections. When MCP becomes available, locate the displayed example by its exact preview URL with `list_sessions` before continuing edits. A separately launched CLI server is a separate service: never silently switch to a same-named cut on another server. If necessary, save the displayed example as a complete `.vcutweb` and reopen it in the connected service.

Keep the preview server alive after setup and preserve the selected session. The first preview defers optional vision setup without changing consent. Use the visible player for playback when requested or when autoplay is blocked. `get_preview_status` can confirm connected clients, project version, rendering errors and advancing playback, but does not prove audible speaker output. If the host cannot open an in-app preview, the CLI's `--open` must open the OS browser; verify that a preview client connects. Do not claim playback or export verification from initialization alone. Use the editing and motion-template skills for follow-up work. Save the complete `.vcutweb` project when persistence is requested.
