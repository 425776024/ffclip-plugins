import {
  Output,
  BufferTarget,
  Mp4OutputFormat,
  WebMOutputFormat,
  AudioSample,
  CanvasSource,
  Quality
} from 'mediabunny';
import {
  calibrateAacEncoder,
  GaplessAacSource,
  TimedOpusSource
} from '../packages/render/audio-encoder';
import { videoLatencyMode } from '../packages/render/capabilities.mjs';
const status = document.querySelector('#status')!;
const format = new URLSearchParams(location.search).get('format') === 'webm' ? 'webm' : 'mp4';
document.querySelector('#run')!.textContent = `运行 ${format === 'mp4' ? 'AAC' : 'Opus'} 同步验收`;
document.querySelector('#run')!.addEventListener('click', async () => {
  const report: any = { phase: 'calibration', started: new Date().toISOString() };
  const publish = async () => {
    status.textContent = JSON.stringify(report, null, 2);
    await fetch('http://127.0.0.1:4332/report', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-QA-Report': 'render' },
      body: JSON.stringify(report)
    });
  };
  try {
    await publish();
    report.format = format;
    if (format === 'mp4') report.calibration = await calibrateAacEncoder();
    report.phase = 'encoding';
    await publish();
    const sampleRate = 48000,
      total = 144000,
      reference = new Float32Array(total);
    const markers = [0, 12000, 60000, 108000, 142000];
    for (const start of markers) {
      let seed = 0x489975,
        previous = 0;
      for (let i = 0; i < 1024 && start + i < total; i++) {
        seed ^= seed << 13;
        seed ^= seed >>> 17;
        seed ^= seed << 5;
        previous = previous * 0.6 + ((seed >>> 0) / 0xffffffff - 0.5) * 0.5;
        reference[start + i] = previous;
      }
    }
    const target = new BufferTarget(),
      output = new Output({
        format:
          format === 'mp4' ? new Mp4OutputFormat({ fastStart: false }) : new WebMOutputFormat(),
        target
      });
    const audio =
      format === 'mp4'
        ? new GaplessAacSource(report.calibration.delay, total)
        : new TimedOpusSource(total);
    const canvas = new OffscreenCanvas(160, 90),
      ctx = canvas.getContext('2d')!;
    const video = new CanvasSource(canvas, {
      codec: format === 'mp4' ? 'avc' : 'vp9',
      latencyMode: videoLatencyMode(format === 'mp4' ? 'avc' : 'vp9', navigator.userAgent),
      quality: new Quality({ bitrate: 300000 })
    });
    output.addVideoTrack(video, { frameRate: 30 });
    output.addAudioTrack(audio.source);
    await output.start();
    report.phase = 'rendering';
    await publish();
    await Promise.all([
      (async () => {
        for (let frame = 0; frame < 90; frame++) {
          report.pendingVideoFrame = frame;
          status.textContent = JSON.stringify(report, null, 2);
          ctx.fillStyle = frame % 30 === 0 ? 'white' : '#223344';
          ctx.fillRect(0, 0, 160, 90);
          await video.add(frame / 30, 1 / 30);
          if (frame % 30 === 0) {
            report.videoFrame = frame;
            await publish();
          }
        }
        video.close();
        report.videoClosed = true;
        await publish();
      })(),
      (async () => {
        for (let start = 0; start < total; start += 4096) {
          const count = Math.min(4096, total - start),
            data = new Float32Array(count * 2);
          for (let i = 0; i < count; i++) {
            data[i] = reference[start + i];
            data[count + i] = -data[i];
          }
          const sample = new AudioSample({
            format: 'f32-planar',
            data,
            numberOfChannels: 2,
            sampleRate,
            timestamp: start / sampleRate
          });
          try {
            await audio.add(sample);
            if (start % 16384 === 0) {
              report.audioFrame = start;
              await publish();
            }
          } finally {
            sample.close();
          }
        }
        await audio.close();
        report.audioClosed = true;
        await publish();
      })()
    ]);
    report.phase = 'finalizing';
    await publish();
    await output.finalize();
    for (const [name, body] of [
      [`aac-sync-corrected.${format}`, target.buffer!],
      ['aac-sync-reference.f32', reference.buffer]
    ] as const) {
      const response = await fetch(`http://127.0.0.1:4332/audio-file?name=${name}`, {
        method: 'POST',
        headers: { 'X-QA-Report': 'render' },
        body
      });
      if (!response.ok) throw new Error(await response.text());
    }
    report.phase = 'complete';
    report.frames = 90;
    report.audioFrames = total;
    report.markers = markers;
    report.artifacts = [`aac-sync-corrected.${format}`, 'aac-sync-reference.f32'];
    await publish();
  } catch (error) {
    report.phase = 'failed';
    report.error = String(error);
    await publish();
  }
});
