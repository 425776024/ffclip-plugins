import { AudioSample, EncodedAudioPacketSource, EncodedPacket } from 'mediabunny';
import { audioPacketTiming, measureAudioDelay } from './audio-timing.mjs';
import { normalizeAacDecoderConfig } from './aac-config.mjs';
import { isSafari } from './capabilities.mjs';

const config: AudioEncoderConfig = {
  codec: 'mp4a.40.2',
  sampleRate: 48000,
  numberOfChannels: 2,
  bitrate: 192000,
  bitrateMode: 'variable'
};
type Calibration = { delay: number; correlation: number };
let calibration: Promise<Calibration> | undefined;

/** WebCodecs does not expose AAC priming. Measure the actual configured encoder/decoder pair once per worker. */
export function calibrateAacEncoder(): Promise<Calibration> {
  if (!calibration)
    calibration = probeAac().catch((error) => {
      calibration = undefined;
      throw error;
    });
  return calibration;
}

async function probeAac(): Promise<Calibration> {
  const frames = 16384,
    markerStart = 4096,
    markerFrames = 4096;
  const reference = new Float32Array(frames),
    data = new Float32Array(frames * 2);
  let seed = 0x516ba431,
    previous = 0;
  for (let i = markerStart; i < markerStart + markerFrames; i++) {
    seed ^= seed << 13;
    seed ^= seed >>> 17;
    seed ^= seed << 5;
    // A deterministic broad-band marker with softened high frequencies survives lossy AAC encoding.
    previous = previous * 0.6 + ((seed >>> 0) / 0xffffffff - 0.5) * 0.3;
    reference[i] = previous;
  }
  data.set(reference);
  data.set(reference, frames);
  const packets: EncodedAudioChunk[] = [],
    decoded: Float32Array[] = [];
  let metadata: AudioDecoderConfig | undefined, failure: Error | undefined;
  const encoder = new AudioEncoder({
    output(chunk, meta) {
      packets.push(chunk);
      metadata ??= normalizeAacDecoderConfig(meta?.decoderConfig);
    },
    error(error) {
      failure = error;
    }
  });
  let decoder: AudioDecoder | undefined;
  try {
    encoder.configure(config);
    const input = new AudioData({
      format: 'f32-planar',
      data,
      sampleRate: config.sampleRate,
      numberOfChannels: 2,
      numberOfFrames: frames,
      timestamp: 0
    });
    try {
      encoder.encode(input);
    } finally {
      input.close();
    }
    await encoder.flush();
    if (failure) throw failure;
    if (!metadata) throw new Error('AAC 编码器没有返回解码配置');
    decoder = new AudioDecoder({
      output(sample) {
        try {
          const plane = new Float32Array(sample.numberOfFrames);
          sample.copyTo(plane, { planeIndex: 0, format: 'f32-planar' });
          decoded.push(plane);
        } finally {
          sample.close();
        }
      },
      error(error) {
        failure = error;
      }
    });
    decoder.configure(metadata);
    for (const packet of packets) decoder.decode(packet);
    await decoder.flush();
    if (failure) throw failure;
    const pcm = new Float32Array(decoded.reduce((n, plane) => n + plane.length, 0));
    let offset = 0;
    for (const plane of decoded) {
      pcm.set(plane, offset);
      offset += plane.length;
    }
    return measureAudioDelay(reference, pcm, markerStart, markerFrames);
  } finally {
    if (encoder.state !== 'closed') encoder.close();
    if (decoder && decoder.state !== 'closed') decoder.close();
  }
}

/** Public packet API lets the MP4 muxer write an edit list for priming and shorten the final sample. */
export class TimedAudioPacketSource {
  readonly source: EncodedAudioPacketSource;
  private encoder: AudioEncoder;
  private writes: Promise<void> = Promise.resolve();
  private failure: unknown;
  private cursor = 0;
  private closed = false;
  private warmupFrames = 0;
  constructor(
    codec: 'aac' | 'opus',
    private delay: number | null,
    private totalFrames: number
  ) {
    // Apple's AAC encoder can suppress a transient at input sample zero. Feed silence first,
    // then remove both this guard and the measured codec delay via the same MP4 edit list.
    if (codec === 'aac' && isSafari(navigator.userAgent)) {
      this.warmupFrames = 4096;
      this.delay! += this.warmupFrames;
    }
    this.source = new EncodedAudioPacketSource(codec);
    this.encoder = new AudioEncoder({
      output: (chunk, meta) => {
        const packet = EncodedPacket.fromEncodedChunk(chunk);
        const packetFrames = Math.round(packet.duration * config.sampleRate);
        try {
          if (this.delay === null) {
            const description = meta?.decoderConfig?.description;
            if (!description) throw new Error('Opus 编码器没有返回预卷信息');
            const bytes = ArrayBuffer.isView(description)
              ? new Uint8Array(description.buffer, description.byteOffset, description.byteLength)
              : new Uint8Array(description);
            if (bytes.length < 19 || new TextDecoder().decode(bytes.subarray(0, 8)) !== 'OpusHead')
              throw new Error('Opus 编码器返回了无效的预卷信息');
            this.delay = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint16(
              10,
              true
            );
          }
          const timing = audioPacketTiming(
            this.cursor,
            packetFrames,
            this.delay,
            this.totalFrames,
            config.sampleRate
          );
          this.cursor += packetFrames;
          if (timing)
            this.writes = this.writes.then(() =>
              this.source.add(
                packet.clone(timing),
                codec === 'aac' && meta?.decoderConfig
                  ? { ...meta, decoderConfig: normalizeAacDecoderConfig(meta.decoderConfig) }
                  : meta
              )
            );
          // Attach a rejection handler immediately, even while the encoder emits several outputs synchronously.
          void this.writes.catch((error) => {
            this.failure = error;
          });
        } catch (error) {
          this.failure = error;
        }
      },
      error: (error) => {
        this.failure = error;
      }
    });
    this.encoder.configure(codec === 'aac' ? config : { ...config, codec: 'opus' });
    if (this.warmupFrames) {
      const silence = new AudioData({
        format: 'f32-planar',
        data: new Float32Array(this.warmupFrames * 2),
        sampleRate: config.sampleRate,
        numberOfChannels: 2,
        numberOfFrames: this.warmupFrames,
        timestamp: 0
      });
      try {
        this.encoder.encode(silence);
      } finally {
        silence.close();
      }
    }
  }
  async add(sample: AudioSample) {
    if (this.failure) throw this.failure;
    const shifted = this.warmupFrames ? sample.clone() : undefined;
    shifted?.setTimestamp(sample.timestamp + this.warmupFrames / config.sampleRate);
    const data = (shifted ?? sample).toAudioData();
    try {
      this.encoder.encode(data);
    } finally {
      data.close();
      shifted?.close();
    }
    if (this.encoder.encodeQueueSize >= 4)
      await new Promise((resolve) =>
        this.encoder.addEventListener('dequeue', resolve, { once: true })
      );
    await this.writes;
    if (this.failure) throw this.failure;
  }
  async close() {
    if (this.closed) return;
    this.closed = true;
    try {
      await this.encoder.flush();
      await this.writes;
      if (this.failure) throw this.failure;
    } finally {
      if (this.encoder.state !== 'closed') this.encoder.close();
      this.source.close();
    }
  }
  cancel() {
    this.closed = true;
    if (this.encoder.state !== 'closed') this.encoder.close();
    this.source.close();
  }
}

export class GaplessAacSource extends TimedAudioPacketSource {
  constructor(delay: number, totalFrames: number) {
    super('aac', delay, totalFrames);
  }
}

/** OpusHead supplies the delay. WebM timestamps have millisecond precision; tail discard is not exposed by the muxer. */
export class TimedOpusSource extends TimedAudioPacketSource {
  constructor(totalFrames: number) {
    super('opus', null, totalFrames);
  }
}
