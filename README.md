# VideoCut

## Copy this into your AI assistant

```text
Install the latest official npm package @ffclip-com/videocut. Set it up for this assistant, preserve my existing settings, and open the built-in editable demo in my browser. Use Node.js 22 or newer. If a new tool connection needs a restart, open the demo with the CLI first.
```

## Or run this in your terminal

```sh
npm install -g @ffclip-com/videocut@latest && videocut-web --demo --open --port 0
```

[Chinese](README-zh.md) · [Website](https://ffclip.com)

![VideoCut 0.2.12: editable demo, text presets and aligned property panels](docs/images/videocut-editor.png)

<details>
<summary>Filters, keyframe buttons and transitions</summary>

![Filter presets and previous, add/remove, next keyframe buttons](docs/images/videocut-keyframes.png)

![Transition library and clip properties](docs/images/videocut-transitions.png)

</details>

Edit local videos with an AI assistant. Preview, captions, animation and export in one timeline.

- Trim and arrange clips on an editable timeline.
- Add captions, animated titles and local voiceovers.
- Apply filters and transitions with editable property controls and keyframes.
- Preview changes live and export MP4 or WebM.

Requires Node.js 22+ and Chrome or Edge. The bundled demo needs no media selection or model downloads. Ask your assistant to trim a clip, add a title, or export a video.

[Usage guide](docs/usage-en.md) · [MIT](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md)

VideoCut’s early browser video implementation incorporated and adapted [WebAV](https://github.com/WebAV-Tech/WebAV) (MIT, Copyright © 2023 风痕). Thanks to its author and contributors. See [third-party acknowledgment](THIRD_PARTY_NOTICES.md).
