# README examples

[中文](README-zh.md) · [Back to VideoCut](../../../README.md)

Download [showcase-projects.zip](showcase-projects.zip) and extract it. In VideoCut, choose **Open project** and select one complete `.vcutweb` directory:

| Project              | Duration   | Contents                                                  |
| -------------------- | ---------- | --------------------------------------------------------- |
| `html-intro.vcutweb` | 6 seconds  | Editable HTML intro and title variables                   |
| `html-chart.vcutweb` | 6 seconds  | Editable HTML chart with illustrative values              |
| `voiceover.vcutweb`  | 10 seconds | HTML graphic, generated narration and three text captions |

The projects include their resources. Reopening and exporting the supplied voiceover does not require another model download; generating new speech or recognizing new audio does. Fonts use the host system and may vary across platforms.

The HTML files beside this guide are source assets for the VideoCut HTML renderer, which drives `window.tick` with an explicit time. They are not standalone autoplay pages. The voiceover project supplies the waveform data, audio and captions separately.

All three MP4s were exported through VideoCut 0.2.12 at 1280 × 720, 24 fps. The MP4s below are GitHub-hosted video attachments. The narration was synthesized locally with Kokoro `zf_001` at speed 1. Whisper Base supplied an initial transcript; its wording was corrected against the script, and caption boundaries were adjusted using pauses in the generated audio. These are sentence captions, not verified word-level alignment. The circular waveform uses measured RMS audio levels.

The original motion artwork is covered by the repository’s MIT license. Model and runtime notices remain in [THIRD_PARTY_NOTICES.md](../../../THIRD_PARTY_NOTICES.md).

## Prompts and exported videos

These videos were exported by VideoCut and play directly below. After setup, send a prompt to your assistant and customize the words, colors or data for your own version.

### HTML motion · Product intro

```text
Use VideoCut to create a 6-second, 16:9 HTML product intro. Use a cream background and coral
accents. Reveal “From idea.” then “To video.” with upward motion. Fan out three overlapping
cards on the right, let them float gently, and rotate a circular badge. Keep the titles and
animation editable, open the preview, and export an MP4.
```

https://github.com/user-attachments/assets/60a8880e-c636-435c-9dcd-6c2b287854ac

[HTML source](html-intro.html)

Keep going: `Change the title to “Weekend stories” and the coral accents to blue. Keep the card animation.`

### Animated data · Numbers with a story

```text
Use VideoCut to create a 6-second HTML data animation. Use a dark green background, light
green bars, and the title “Small steps. Big momentum.” Grow the Q1–Q4 bars in sequence and
count up from zero to 24%, 48%, 72% and 96%. End the large number on the left at 96%. Label
this as illustrative data and keep the values editable. Open the preview and export an MP4.
```

https://github.com/user-attachments/assets/7e8fb040-f794-4732-9f98-5d83c013ec94

[HTML source](html-chart.html)

Keep going: `Change the values to 18, 42, 65 and 88, and the title to “A year of progress”.`

### Local voiceover · From script to spoken story

```text
Use VideoCut’s local Chinese female voice zf_001 at normal speed to read:
“给灵感一个声音。把文字变成配音，让字幕跟随节奏。现在，开始你的创作。” Make an approximately 10-second video in cream and
olive green, with the title “让灵感，成为作品。” and a circular waveform driven by the actual audio.
Generate captions, then correct their wording and sentence boundaries against the script.
Keep speech and captions on separate editable tracks. Open the preview and export an MP4
with sound.
```

https://github.com/user-attachments/assets/0da34d26-bc46-474d-856a-34a61aef828e

Play the video and unmute it to hear the locally generated Chinese narration.

Keep going: `Replace the narration with an English product introduction using af_maple. Regenerate speech and captions, keeping the visual style.`

Voiceover and speech recognition download their models on first use, then process locally. The sample uses Chinese narration; its captions were corrected against the script.
