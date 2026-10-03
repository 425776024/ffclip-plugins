import { calibrateAacEncoder } from '../packages/render/audio-encoder';

// Read the actual encoder output before calibration, for browser interoperability QA.
document.querySelector('#run')!.addEventListener('click', async () => {
  const status = document.querySelector('#status')!;
  const report: any = { browser: navigator.userAgent };
  let encoder: AudioEncoder | undefined;
  try {
    const packets: any[] = [];
    encoder = new AudioEncoder({
      output(chunk, meta) {
        if (meta?.decoderConfig) {
          const { description, ...config } = meta.decoderConfig;
          const bytes = description
            ? ArrayBuffer.isView(description)
              ? new Uint8Array(description.buffer, description.byteOffset, description.byteLength)
              : new Uint8Array(description)
            : [];
          report.config = { ...config, description: Array.from(bytes) };
        }
        packets.push({
          timestamp: chunk.timestamp,
          duration: chunk.duration,
          size: chunk.byteLength
        });
      },
      error(error) {
        report.encoderError = String(error);
      }
    });
    encoder.configure({
      codec: 'mp4a.40.2',
      sampleRate: 48000,
      numberOfChannels: 2,
      bitrate: 192000,
      bitrateMode: 'variable'
    });
    const sample = new AudioData({
      format: 'f32-planar',
      data: new Float32Array(16384 * 2),
      sampleRate: 48000,
      numberOfChannels: 2,
      numberOfFrames: 16384,
      timestamp: 0
    });
    encoder.encode(sample);
    sample.close();
    await encoder.flush();
    report.packets = { count: packets.length, first: packets.slice(0, 2), last: packets.at(-1) };
    report.calibration = await calibrateAacEncoder();
  } catch (error) {
    report.error = String(error);
  } finally {
    if (encoder && encoder.state !== 'closed') encoder.close();
    status.setAttribute('style', 'white-space:pre-wrap;font:14px system-ui');
    status.textContent = JSON.stringify({
      calibration: report.calibration,
      error: report.error,
      ...report
    });
  }
});
