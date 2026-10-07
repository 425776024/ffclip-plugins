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

![VideoCut 0.2.13: text presets, filters, keyframes and the About dialog in a looping preview](docs/images/videocut-ui.gif)

Edit local videos with an AI assistant. Preview, captions, animation and export in one timeline.

- Trim and arrange clips on an editable timeline.
- Add captions, animated titles and local voiceovers.
- Apply filters and transitions with editable property controls and keyframes.
- Preview changes live and export MP4 or WebM.

The About dialog includes the official website, WeChat QR codes and the creator’s Bilibili profile. The editor automatically checks for new releases; choosing Update saves the complete project before installing the latest official package. Restart the ffclip service or MCP connection to use the installed version.

Requires Node.js 22+ and Chrome or Edge. The bundled demo needs no media selection or model downloads. Ask your assistant to trim a clip, add a title, or export a video.

[Usage guide](docs/usage-en.md) · [MIT](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md)

VideoCut’s early browser video implementation incorporated and adapted [WebAV](https://github.com/WebAV-Tech/WebAV) (MIT, Copyright © 2023 风痕). Thanks to its author and contributors. See [third-party acknowledgment](THIRD_PARTY_NOTICES.md).
