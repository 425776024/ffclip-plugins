# VideoCut usage

## What you can do

- Import video, audio and images; trim, split and arrange clips on a timeline.
- Add captions, animated titles, effects, transitions and background music.
- Generate voiceovers and use an agent to transcribe speech or describe footage locally.
- Export MP4 or WebM videos and save editable projects with their media.

## Install and start

You need Node.js 22 or later and a browser with WebCodecs support, such as Chrome or Edge.

```sh
npm install -g @ffclip-com/videocut
videocut-web --root "/path/to/media"
```

Replace `/path/to/media` with your media folder, then open [http://127.0.0.1:4318](http://127.0.0.1:4318).

To start with the bundled editable example and automatically open its preview:

```sh
videocut-web --demo --open --port 0
```

The 18-second cinematic project includes an original coastal picture, animated text with moving gradients, native gradient titles, continuous narration and timed captions on five editable tracks. Picture and title keyframes remain editable. It needs no media selection or AI model downloads. Use `--demo-locale en` for English; Chinese is the default. Save it as a complete `.vcutweb` to keep your changes.

The editor checks npm for the latest official release when opened and every six hours while visible. An update dialog offers **Update automatically** and **Update with an AI assistant**. Automatic installation saves all current sessions as complete `.vcutweb` projects in the first authorized folder, updates the running package's global, local, npx or bundled plugin installation, then asks you to restart the ffclip service or MCP connection. Bundled plugin updates preserve MCP arguments and the optional native bridge. An active export, voiceover or analysis job prevents installation; a failed save also prevents it. The service never restarts itself. The AI option copies a prompt for Codex, WorkBuddy, Qoder or another assistant to save the project, update, preserve MCP settings and reopen it after restarting. Source checkouts use the AI option rather than overwriting the repository. Offline checks stay quiet; use **Check for updates** in the toolbar to retry manually.

Media, saved projects and export destinations must be inside an allowed folder. To allow more folders, repeat `--root`:

```sh
videocut-web --root "/path/to/media" --root "/path/to/exports"
```

## Connect an agent

After installing the package, add this configuration to an MCP client that supports local servers:

```json
{
  "mcpServers": {
    "videocut": {
      "command": "videocut-web",
      "args": ["--mcp", "--port", "0", "--root", "/path/to/media"]
    }
  }
}
```

Replace the folder path before connecting. Your agent can open a project preview and edit the same timeline you see in the browser. Keep the preview open during video export and AI tasks.

For first-time setup, the bundled `initialize-demo` SKILL calls `initialize_demo` and opens the returned preview automatically. Run `videocut-web --print-setup-skill` to read its instructions, or load `plugins/videocut-local/skills/initialize-demo/SKILL.md` from the installed package. It also supports CLI initialization when newly registered MCP tools are not yet available in the current chat.

Try asking your agent to:

- "Trim the first two seconds and add an opening title."
- "Create editable captions from this clip."
- "Add background music and export an MP4."

## Save and reopen

Choose **Export → Save complete project** to save a `.vcutweb` folder containing the project and its media. Keep the entire folder together when moving or sharing it.

Reopen it with **Open project**, or start the editor with:

```sh
videocut-web --root "/path/to/projects" --project "/path/to/projects/MyVideo.vcutweb"
```

Save before closing the editor: unsaved sessions are temporary. Until you save a complete project, keep imported media in its original location. Projects use fonts installed on your computer; install missing fonts when moving a project to another computer.

Native `.vcut` projects also require a compatible VideoCut bridge, configured with `--native-bridge`.

## Export video

Choose MP4 or WebM in the export menu and keep the editor page open until export finishes. Available formats depend on your browser. If MP4 is unavailable, try WebM. FFmpeg is an optional fallback and is not required to start the editor.

## Local AI features

Voiceovers support Mandarin Chinese and English. An agent can also create captions from speech and describe images or sampled video scenes.

These optional features require model downloads. Speech recognition downloads its model on first use; voiceovers require model installation, and visual analysis asks for your permission before downloading. Once installed, processing runs locally and your media stays on your computer.

Model downloads automatically choose between `huggingface.co` and `hf-mirror.com`. A slow or unavailable source is tried alongside the alternative; interrupted or invalid downloads switch sources automatically. Every file is checked against its pinned revision, size and SHA256 before being cached for offline use.

## Keyboard shortcuts

| Action                 | Shortcut             |
| ---------------------- | -------------------- |
| Play or pause          | Space                |
| Previous or next frame | Left / Right arrow   |
| Split selected clips   | Cmd/Ctrl + B         |
| Delete selected clips  | Delete               |
| Undo                   | Cmd/Ctrl + Z         |
| Redo                   | Cmd/Ctrl + Shift + Z |
