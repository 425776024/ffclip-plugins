# README examples

[中文](README-zh.md) · [Back to VideoCut](../../../README.md)

Download [showcase-projects.zip](showcase-projects.zip) and extract it. In VideoCut, choose **Open project** and select one complete `.vcutweb` directory:

| Project | Duration | Contents |
| --- | --- | --- |
| `html-intro.vcutweb` | 6 seconds | Editable HTML intro and title variables |
| `html-chart.vcutweb` | 6 seconds | Editable HTML chart with illustrative values |
| `voiceover.vcutweb` | 10 seconds | HTML graphic, generated narration and three text captions |

The projects include their resources. Reopening and exporting the supplied voiceover does not require another model download; generating new speech or recognizing new audio does. Fonts use the host system and may vary across platforms.

The HTML files beside this guide are source assets for the VideoCut HTML renderer, which drives `window.tick` with an explicit time. They are not standalone autoplay pages. The voiceover project supplies the waveform data, audio and captions separately.

All three MP4s were exported through VideoCut 0.2.12 at 1280 × 720, 24 fps. The README embeds the MP4s as GitHub-hosted video attachments. The narration was synthesized locally with Kokoro `zf_001` at speed 1. Whisper Base supplied an initial transcript; its wording was corrected against the script, and caption boundaries were adjusted using pauses in the generated audio. These are sentence captions, not verified word-level alignment. The circular waveform uses measured RMS audio levels.

The original motion artwork is covered by the repository’s MIT license. Model and runtime notices remain in [THIRD_PARTY_NOTICES.md](../../../THIRD_PARTY_NOTICES.md).
