import {
  Input, ALL_FORMATS, UrlSource, VideoSampleSink, AudioSampleSink, EncodedPacketSink, MatroskaInputFormat, OggInputFormat,
  type InputVideoTrack, type InputAudioTrack, type VideoSample
} from 'mediabunny';
import { mapTimelineToSource, evaluateAudio, seconds, ticks, duration, type Project } from '../core/project.mjs';
import { ByteCache, MediaScheduler, PeakAccumulator, abortError, throwIfAborted, thumbnailGeometry, sampleLinear, sourceCacheIdentity, type PeakLevel } from './runtime.mjs';
import { MediaArtifactStore } from './artifacts';
import { MediaIndexStore } from './index';
import { setMediaForegroundActivity, waitForMediaBackground } from './activity';
export { setMediaForegroundActivity } from './activity';
import { readMatroskaCodecDelay } from './container-timing.mjs';

export interface MediaMetadata {
  kind: 'video' | 'audio' | 'image';
  duration: number; // seconds, normalized to a common media origin
  width: number; height: number; hasAudio: boolean;
  sourceOrigin: number; videoCodec: string | null; audioCodec: string | null;
  videoDecodable: boolean; audioDecodable: boolean;
  sampleRate: number; channels: number;
  rotation: number; colorSpace?: VideoColorSpaceInit;
}
export interface MediaFrame {
  frame: ImageBitmap;
  timestamp: number; duration: number; width: number; height: number;
  close(): void;
}
export interface FrameOptions { signal?: AbortSignal; width?: number; height?: number; priority?: number }
export interface ThumbnailOptions extends FrameOptions { displayWidth?: number; displayHeight?: number; dpr?: number; fit?: 'cover' | 'contain'; quality?: 'interactive' | 'final'; crop?: {left:number;right:number;top:number;bottom:number} }
export interface MediaVideoReader { frameAt(time: number): Promise<MediaFrame>; close(): Promise<void> }
export interface AudioBlock {
  data: Float32Array[]; timestamp: number; sampleRate: number; numberOfFrames: number;
}
export interface WaveformChunk {
  start: number; end: number; sampleRate: number; channels: number; levels: PeakLevel[];
}
export interface WaveformResult {
  begin: number; end: number; channels: { min: Float32Array; max: Float32Array }[];
}
type Source = { input: Input; video: InputVideoTrack | null; audio: InputAudioTrack | null; metadata: MediaMetadata; opusCodecDelay?: Promise<number> };
type SourceEntry = { value: Promise<Source>; input: Input; users: number; touched: number; retired: boolean };

/** Cache and consumers hold independent references to the same immutable bitmap. */
class BitmapEntry {
  private users = 1;
  constructor(readonly bitmap: ImageBitmap, readonly timestamp: number, readonly duration: number,
    public coverageEnd = timestamp + duration) {}
  retain() { this.users++; return this; }
  close() { if (--this.users === 0) this.bitmap.close(); }
  lease(): MediaFrame {
    this.retain();
    let closed = false;
    return { frame: this.bitmap, timestamp: this.timestamp, duration: this.duration, width: this.bitmap.width, height: this.bitmap.height,
      close: () => { if (!closed) { closed = true; this.close(); } } };
  }
}

const IMAGE_PATTERN = /\.(?:png|jpe?g|webp|bmp|avif)(?:[?#]|$)/i;
const finiteTime = (time: number) => {
  if (!Number.isFinite(time)) throw new RangeError('Invalid media time');
  return Math.max(0, time);
};

/** Browser-only codec path: no process, FFmpeg, server codec extension, or full-file PCM decode. */
export class MediaEngine {
  private readonly sources = new Map<string, SourceEntry>();
  private readonly frames = new ByteCache<BitmapEntry>(96 * 1024 * 1024);
  private readonly pcm = new ByteCache<AudioBlock>(32 * 1024 * 1024);
  private readonly peaks = new ByteCache<WaveformChunk>(16 * 1024 * 1024);
  private readonly jobs = new MediaScheduler(2);
  private readonly audioJobs = new MediaScheduler(1);
  private readonly thumbnailJobs = new MediaScheduler(1);
  private readonly waveformJobs = new MediaScheduler(1);
  private readonly artifacts = new MediaArtifactStore();
  private readonly index = new MediaIndexStore(this.artifacts);
  private persistentThumbnailHits = 0;
  private thumbnailDecodes = 0;
  private disposed = false;
  private readonly sourceVersions = new Map<string, number>();
  private identityEpoch = 0;

  /** Call when a source binding changes; callers should also version authorized URLs. */
  invalidate(url?: string) {
    this.index.clear();
    if (url) {
      const identity = sourceCacheIdentity(url);
      this.sourceVersions.set(identity, (this.sourceVersions.get(identity) ?? 0) + 1);
    }
    if (!url || this.sourceVersions.size > 4096) { this.identityEpoch++; this.sourceVersions.clear(); }
    for (const cache of [this.frames, this.pcm, this.peaks]) {
      if (!url) cache.clear();
      else for (const key of cache.entries.keys()) if (key.includes(`${sourceCacheIdentity(url)}#media-v1:`)) cache.delete(key);
    }
    for (const [key, entry] of this.sources) if (!url || sourceCacheIdentity(key) === sourceCacheIdentity(url)) {
      this.sources.delete(key); entry.retired = true;
      if (!entry.users) entry.input.dispose();
    }
  }
  release(url: string) { this.invalidate(url); }
  stats() {
    return { sources: this.sources.size, frameBytes: this.frames.bytes, pcmBytes: this.pcm.bytes, waveformBytes: this.peaks.bytes,
      frameHits: this.frames.hits, frameMisses: this.frames.misses, activeJobs: this.jobs.active + this.audioJobs.active + this.thumbnailJobs.active + this.waveformJobs.active,
      completed: this.jobs.completed + this.audioJobs.completed + this.thumbnailJobs.completed + this.waveformJobs.completed,
      cancelled: this.jobs.cancelled + this.audioJobs.cancelled + this.thumbnailJobs.cancelled + this.waveformJobs.cancelled,
      persistentThumbnailHits: this.persistentThumbnailHits, thumbnailDecodes: this.thumbnailDecodes, ...this.index.stats(),
      persistentHits: this.artifacts.counters.hits, persistentWrites: this.artifacts.counters.writes };
  }
  persistentUsage() { return this.artifacts.usage(); }
  private identity(url: string) { const source = sourceCacheIdentity(url); return `${source}#media-v1:${this.identityEpoch}:${this.sourceVersions.get(source) ?? 0}`; }
  private async acquire(url: string, signal?: AbortSignal): Promise<{ source: Source; release: () => void }> {
    throwIfAborted(signal);
    if (this.disposed) throw new Error('Media engine is disposed');
    let entry = this.sources.get(url);
    if (!entry) {
      const identity = this.identity(url);
      const input = new Input({ formats: ALL_FORMATS, source: new UrlSource(url, {
        maxCacheSize: 4 * 1024 * 1024, parallelism: 1,
        getRetryDelay: (attempt) => attempt < 2 ? 0.2 * (attempt + 1) : null,
        requestInit: { credentials: 'same-origin' }
      }) });
      entry = { input, users: 0, touched: performance.now(), retired: false, value: this.inspect(input).then(source => {
        if (!this.disposed) void this.index.saveMetadata(identity, source.metadata).catch(() => {});
        return source;
      }) };
      // Ensure failures occurring before the first await cannot be unhandled.
      entry.value.catch(() => {});
      this.sources.set(url, entry);
    }
    entry.users++; entry.touched = performance.now();
    const owner = entry;
    let released = false;
    const release = () => {
      if (released) return;
      released = true; owner.users--;
      if (owner.retired && !owner.users) owner.input.dispose();
      this.trimSources();
    };
    try {
      const source = await owner.value;
      throwIfAborted(signal);
      return { source, release };
    } catch (error) {
      release();
      if (!signal?.aborted && this.sources.get(url) === owner) {
        this.sources.delete(url); owner.retired = true;
        if (!owner.users) owner.input.dispose();
      }
      throw error;
    }
  }
  private trimSources() {
    if (this.sources.size <= 6) return;
    const idle = [...this.sources].filter(([, e]) => !e.users).sort((a, b) => a[1].touched - b[1].touched);
    for (const [key, entry] of idle) {
      if (this.sources.size <= 6) break;
      this.sources.delete(key); entry.retired = true; entry.input.dispose();
    }
  }
  private async inspect(input: Input): Promise<Source> {
    if (!await input.canRead()) throw new Error('无法读取素材容器；请使用受支持的 MP4、WebM 或音频文件。');
    const [video, audio] = await Promise.all([input.getPrimaryVideoTrack(), input.getPrimaryAudioTrack()]);
    if (!video && !audio) throw new Error('素材没有可用的音视频轨道。');
    // FLAC establishes its blocking strategy while reading the first packet.
    // Initialize that cursor before a duration seek (Mediabunny 1.61 shares it).
    const first = await input.getFirstTimestamp();
    const end = await input.computeDuration();
    const origin = Math.max(0, first);
    const [width, height, videoCodec, audioCodec, videoDecodable, audioDecodable, sampleRate, channels, rotation, colorSpace] = await Promise.all([
      video?.getDisplayWidth() ?? 0, video?.getDisplayHeight() ?? 0, video?.getCodec() ?? null, audio?.getCodec() ?? null,
      video?.canDecode() ?? false, audio?.canDecode() ?? false, audio?.getSampleRate() ?? 0, audio?.getNumberOfChannels() ?? 0,
      video?.getRotation() ?? 0, video?.getColorSpace()
    ]);
    if (!Number.isFinite(end) || end <= origin) throw new Error('无法确认素材时长。');
    return { input, video, audio, metadata: { kind: video ? 'video' : 'audio', duration: end - origin, width, height, hasAudio: Boolean(audio),
      sourceOrigin: origin, videoCodec, audioCodec, videoDecodable, audioDecodable, sampleRate, channels, rotation, colorSpace } };
  }
  async probe(url: string, options: { signal?: AbortSignal; kind?: string } = {}): Promise<MediaMetadata> {
    if (options.kind === 'image' || IMAGE_PATTERN.test(url)) {
      const frame = await this.image(url, options);
      try { return { kind: 'image', duration: 5, width: frame.width, height: frame.height, hasAudio: false, sourceOrigin: 0,
        videoCodec: null, audioCodec: null, videoDecodable: true, audioDecodable: false, sampleRate: 0, channels: 0, rotation: 0 }; }
      finally { frame.close(); }
    }
    const lease = await this.acquire(url, options.signal);
    try { return { ...lease.source.metadata }; } finally { lease.release(); }
  }
  async image(url: string, options: FrameOptions = {}): Promise<MediaFrame> {
    const key = `image:${this.identity(url)}:${options.width ?? 0}:${options.height ?? 0}`;
    const existing = this.frames.get(key);
    if (existing) { throwIfAborted(options.signal); return existing.lease(); }
    return this.jobs.run(key, async (signal) => {
      const response = await fetch(url, { signal, credentials: 'same-origin' });
      if (!response.ok) throw new Error(`读取图片失败 (${response.status})`);
      let bitmap = await createImageBitmap(await response.blob());
      const scale = Math.min(1, options.width ? options.width / bitmap.width : 1, options.height ? options.height / bitmap.height : 1);
      if (scale < 1) {
        const original = bitmap;
        try { bitmap = await createImageBitmap(original, { resizeWidth: Math.max(1, Math.round(original.width * scale)), resizeHeight: Math.max(1, Math.round(original.height * scale)), resizeQuality: 'high' }); }
        finally { original.close(); }
      }
      if (signal.aborted) { bitmap.close(); throw abortError(); }
      const entry = new BitmapEntry(bitmap, 0, Infinity);
      this.frames.set(key, entry.retain(), bitmap.width * bitmap.height * 4);
      return entry;
    }, { ...options, consume: (entry) => entry.lease(), afterDispatch: (entry) => entry.close() });
  }
  private async bitmap(sample: VideoSample, origin: number, options: FrameOptions): Promise<BitmapEntry> {
    const scale = Math.min(options.width ? options.width / sample.displayWidth : 1,
      options.height ? options.height / sample.displayHeight : 1, 1);
    const width = Math.max(1, Math.round(sample.displayWidth * scale));
    const height = Math.max(1, Math.round(sample.displayHeight * scale));
    if (width * height > 32 * 1024 * 1024) throw new Error('素材帧尺寸超出浏览器媒体预算。');
    const canvas = new OffscreenCanvas(width, height);
    const context = canvas.getContext('2d', { alpha: true });
    if (!context) throw new Error('当前浏览器不支持离屏图像绘制。');
    sample.draw(context, 0, 0, width, height);
    return new BitmapEntry(canvas.transferToImageBitmap(), sample.timestamp - origin, sample.duration);
  }
  async videoFrame(url: string, time: number, options: FrameOptions = {}): Promise<MediaFrame> {
    if ((options.priority ?? 0) < 5) setMediaForegroundActivity(true);
    time = finiteTime(time);
    const key = `frame:${this.identity(url)}:${Math.round(time * 1e6)}:${options.width ?? 0}:${options.height ?? 0}`;
    const existing = this.frames.get(key);
    if (existing) { throwIfAborted(options.signal); return existing.lease(); }
    return this.jobs.run(key, async (signal) => {
      const { source, release } = await this.acquire(url, signal);
      try {
        if (!source.video || !source.metadata.videoDecodable) throw new Error(`当前浏览器无法解码视频：${source.metadata.videoCodec ?? '未知编码'}`);
        const sink = new VideoSampleSink(source.video);
        const sample = await sink.getSample(source.metadata.sourceOrigin + Math.min(time, Math.max(0, source.metadata.duration - 0.000001)));
        if (!sample) throw new Error('目标时刻没有可用的视频帧。');
        try {
          throwIfAborted(signal);
          const result = await this.bitmap(sample, source.metadata.sourceOrigin, options);
          if (signal.aborted) { result.close(); throw abortError(); }
          this.frames.set(key, result.retain(), result.bitmap.width * result.bitmap.height * 4);
          return result;
        } finally { sample.close(); }
      } finally { release(); }
    }, { ...options, consume: (entry) => entry.lease(), afterDispatch: (entry) => entry.close() });
  }
  /** A clip/export cursor keeps a bounded decoder alive for monotonically increasing requests. */
  async createVideoReader(url: string, options: FrameOptions = {}): Promise<MediaVideoReader> {
    const identity = this.identity(url);
    const { source, release } = await this.acquire(url, options.signal);
    if (this.disposed || this.identity(url) !== identity) { release(); throw abortError(); }
    if (!source.video || !source.metadata.videoDecodable) { release(); throw new Error('当前浏览器无法解码此视频。'); }
    const sink = new VideoSampleSink(source.video), origin = source.metadata.sourceOrigin;
    const historyPrefix = `reader-frame:${identity}:${options.width ?? 0}:${options.height ?? 0}:`;
    let iterator: AsyncGenerator<VideoSample> | undefined;
    let current: VideoSample | undefined, next: VideoSample | undefined;
    let bitmap: BitmapEntry | undefined, closed = false, lastRequest = -Infinity;
    let serial: Promise<unknown> = Promise.resolve();
    const clear = async () => {
      current?.close(); next?.close(); bitmap?.close();
      current = next = undefined; bitmap = undefined;
      await iterator?.return(undefined); iterator = undefined;
    };
    const check = () => {
      if (closed || this.disposed || this.identity(url) !== identity) throw abortError();
      throwIfAborted(options.signal);
    };
    const frameAt = async (time: number): Promise<MediaFrame> => {
      setMediaForegroundActivity(true);
      check();
      const target = origin + Math.min(finiteTime(time), Math.max(0, source.metadata.duration - 0.000001));
      // Reuse real decoded PTS intervals before flushing a backwards cursor.
      // The engine's existing byte budget owns these immutable bitmap references.
      let covering: string | undefined, latestPts = -Infinity;
      for (const [key, entry] of this.frames.entries) {
        const frame = entry.value;
        if (key.startsWith(historyPrefix) && frame.timestamp <= target - origin &&
            target - origin < frame.coverageEnd && frame.timestamp > latestPts) {
          covering = key; latestPts = frame.timestamp;
        }
      }
      if (covering) {
        check();
        // A history hit does not move the physical decoder or its last request.
        return this.frames.get(covering)!.lease();
      }
      try {
        if (!iterator || time < lastRequest || time - lastRequest > 2) {
          await clear(); check(); iterator = sink.samples(target); lastRequest = time;
          const first = await iterator.next(); current = first.value;
          const second = first.done ? undefined : await iterator.next(); next = second?.value;
          check();
        }
        lastRequest = time;
        while (next && next.timestamp <= target) {
          current?.close(); current = next; next = undefined;
          bitmap?.close(); bitmap = undefined;
          const following = await iterator!.next(); next = following.value;
          check();
        }
        check();
        if (!current) throw new Error('目标时刻没有可用的视频帧。');
        if (!bitmap) {
          bitmap = await this.bitmap(current, origin, options);
          check();
          // The next observed PTS is the exclusive display boundary, including
          // VFR gaps/overlaps. Preserve the existing hold-last-frame behavior.
          bitmap.coverageEnd = next ? next.timestamp - origin : source.metadata.duration;
          this.frames.set(historyPrefix + current.timestamp, bitmap.retain(),
            bitmap.bitmap.width * bitmap.bitmap.height * 4);
          // Tiny sources also need a count bound. Retain history nearest the
          // actual cursor, independently of whether its pixels were visited.
          const history = [...this.frames.entries].filter(([key]) => key.startsWith(historyPrefix));
          if (history.length > 128) {
            history.sort((a, b) =>
              Math.abs(b[1].value.timestamp - (target - origin)) -
              Math.abs(a[1].value.timestamp - (target - origin)));
            for (const [key] of history.slice(0, history.length - 128)) this.frames.delete(key);
          }
        }
        return bitmap.lease();
      } catch (error) {
        await clear();
        throw error;
      }
    };
    return {
      frameAt: (time) => {
        const result = serial.then(() => frameAt(time));
        serial = result.catch(() => {});
        return result;
      },
      close: async () => { if (closed) return; closed = true; await serial; await clear(); release(); }
    };
  }
  /** Sequential decoding, used by render/export without seeking for every output frame. */
  async *videoFrames(url: string, begin: number, end: number, options: FrameOptions = {}): AsyncGenerator<MediaFrame> {
    const { source, release } = await this.acquire(url, options.signal);
    try {
      if (!source.video || !source.metadata.videoDecodable) throw new Error('当前浏览器无法解码此视频。');
      const origin = source.metadata.sourceOrigin;
      for await (const sample of new VideoSampleSink(source.video).samples(begin + origin, end + origin)) {
        try {
          throwIfAborted(options.signal);
          const result = await this.bitmap(sample, origin, options);
          try { yield result.lease(); } finally { result.close(); }
        } finally { sample.close(); }
      }
    } finally { release(); }
  }
  /** One decoder walks the sorted requested PTS list; callers own returned leases. */
  async thumbnails(url: string, times: number[], options: ThumbnailOptions = {}): Promise<MediaFrame[]> {
    if (times.length > 256) throw new RangeError('Thumbnail batches are limited to 256 cells');
    const identity = this.identity(url), displayWidth = options.displayWidth ?? 80, displayHeight = options.displayHeight ?? 48;
    const dpr = (options.dpr ?? 1) * (options.quality === 'interactive' ? .5 : 1), fit = options.fit ?? 'cover';
    const crop = options.crop ?? { left: 0, right: 0, top: 0, bottom: 0 };
    if (Object.values(crop).some(value => !Number.isFinite(value) || value < 0 || value >= 1) || crop.left + crop.right >= 1 || crop.top + crop.bottom >= 1) throw new RangeError('Invalid thumbnail crop');
    const cropKey = [crop.left,crop.right,crop.top,crop.bottom].join(',');
    const key = `thumbnails-v3:${identity}:${displayWidth}:${displayHeight}:${dpr}:${fit}:${cropKey}:${times.map(time => Math.round(finiteTime(time) * 1e6)).join(',')}`;
    return this.thumbnailJobs.run(key, async signal => {
      const result: BitmapEntry[] = [], observations: { timestamp: number; duration: number }[] = [];
      let lease: Awaited<ReturnType<MediaEngine['acquire']>> | undefined;
      try {
        let metadata = await this.index.metadata(identity);
        if (!metadata) { await waitForMediaBackground(signal); lease = await this.acquire(url, signal); metadata = lease.source.metadata; }
        const croppedWidth = metadata.width * (1 - crop.left - crop.right), croppedHeight = metadata.height * (1 - crop.top - crop.bottom);
        const geometry = thumbnailGeometry(croppedWidth, croppedHeight, displayWidth, displayHeight, dpr, fit);
        const scale = geometry.drawWidth / croppedWidth;
        const frameKey = (time: number) => `thumbnail-v3:${identity}:${Math.round(time * 1e6)}:${geometry.width}:${geometry.height}:${fit}:${cropKey}`;
        const pending: { time: number; index: number }[] = [];
        for (let index = 0; index < times.length; index++) {
          throwIfAborted(signal);
          const time = Math.min(finiteTime(times[index]), Math.max(0, metadata.duration - 0.000001));
          const observation = await this.index.resolve(identity, time);
          const canonical = observation?.timestamp ?? time;
          const cached = this.frames.get(frameKey(canonical));
          if (cached) result[index] = cached.retain();
          else {
            const stored = await this.artifacts.get<{ blob: Blob; timestamp: number; duration: number }>(frameKey(canonical));
            if (stored?.blob) {
              const bitmap = await createImageBitmap(stored.blob);
              const entry = new BitmapEntry(bitmap, stored.timestamp, stored.duration);
              this.frames.set(frameKey(canonical), entry.retain(), bitmap.width * bitmap.height * 4);
              result[index] = entry; this.persistentThumbnailHits++;
            } else pending.push({ time, index });
          }
        }
        pending.sort((a, b) => a.time - b.time);
        let index = 0;
        if (pending.length) {
          await waitForMediaBackground(signal);
          lease ||= await this.acquire(url, signal);
          const source = lease.source;
          if (!source.video || !source.metadata.videoDecodable) throw new Error('当前浏览器无法解码此视频。');
          for await (const sample of new VideoSampleSink(source.video).samplesAtTimestamps(pending.map(({ time }) => metadata.sourceOrigin + time))) {
          if (!sample) throw new Error('缩略图时刻没有视频帧。');
          try {
            await waitForMediaBackground(signal);
            const canvas = new OffscreenCanvas(geometry.width, geometry.height), context = canvas.getContext('2d');
            if (!context) throw new Error('当前浏览器不支持缩略图画布。');
            sample.draw(context, geometry.x - crop.left * metadata.width * scale, geometry.y - crop.top * metadata.height * scale, metadata.width * scale, metadata.height * scale);
            const timestamp = sample.timestamp - metadata.sourceOrigin;
            const blob = options.quality === 'interactive' ? undefined : await canvas.convertToBlob({ type: 'image/png' });
            throwIfAborted(signal);
            const entry = new BitmapEntry(canvas.transferToImageBitmap(), timestamp, sample.duration);
            const target = pending[index++]; result[target.index] = entry; this.thumbnailDecodes++;
            this.frames.set(frameKey(timestamp), entry.retain(), geometry.width * geometry.height * 4);
            observations.push({ timestamp, duration: sample.duration });
            if (blob) {
              await this.artifacts.put(frameKey(timestamp), { blob, timestamp, duration: sample.duration }, blob.size + 16);
              if (!(sample.duration > 0) && timestamp !== target.time)
                await this.artifacts.put(frameKey(target.time), { blob, timestamp, duration: sample.duration }, blob.size + 16);
            }
          } finally { sample.close(); }
          }
        }
        await this.index.observe(identity, observations);
        return result;
      } catch (error) { for (const frame of result) frame?.close(); throw error; }
      finally { lease?.release(); }
    }, { ...options, priority: options.priority ?? 5, consume: (entries) => entries.map((entry) => entry.lease()), afterDispatch: (entries) => entries.forEach((entry) => entry.close()) });
  }
  async *audioChunks(url: string, begin: number, end: number, options: { signal?: AbortSignal } = {}): AsyncGenerator<AudioBlock> {
    const { source, release } = await this.acquire(url, options.signal);
    try {
      if (!source.audio || !source.metadata.audioDecodable) throw new Error(`当前浏览器无法解码音频：${source.metadata.audioCodec ?? '未知编码'}`);
      // Audio key packets can still require transform overlap / codec preroll.
      // Seeking AAC straight to zero skips its negative-PTS priming packets and
      // damages the first audible samples even though the timeline remains aligned.
      const { audioCodec, sampleRate, sourceOrigin } = source.metadata;
      const warmupFrames = audioCodec === 'vorbis' ? 8192 : audioCodec === 'mp3' ? 4608 : 4096;
      const preroll = ['aac', 'opus', 'mp3', 'vorbis'].includes(audioCodec || '') ? Math.max(.12, warmupFrames / sampleRate) : 0;
      let opusTimestampCorrection = 0;
      if (audioCodec === 'opus') {
        const description = (await source.audio.getDecoderConfig())?.description;
        const bytes = description && (ArrayBuffer.isView(description)
          ? new Uint8Array(description.buffer, description.byteOffset, description.byteLength)
          : new Uint8Array(description));
        if (bytes && bytes.length >= 19 && [79, 112, 117, 115, 72, 101, 97, 100].every((value, index) => bytes[index] === value)) {
          // Mediabunny 1.61 anchors first decoded PCM to the first packet PTS.
          // OpusHead pre-skip has already removed samples in AudioDecoder, so
          // restore their time on every decoder instance (including local seeks).
          opusTimestampCorrection = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint16(10, true) / 48000;
          const format = await source.input.getFormat();
          if (format instanceof MatroskaInputFormat) {
            // Conventional WebM encoders carry CodecDelay rather than negative
            // block PTS. Do not apply both conventions to the same stream.
            source.opusCodecDelay ||= readMatroskaCodecDelay(url, source.audio.id).catch(error => { source.opusCodecDelay = undefined; throw error; });
            opusTimestampCorrection -= await source.opusCodecDelay;
          } else if (format instanceof OggInputFormat) {
            const first = await source.audio.getFirstTimestamp();
            const selected = await new EncodedPacketSink(source.audio).getKeyPacket(begin + sourceOrigin - preroll, { metadataOnly: true });
            // Ogg demuxing already subtracts pre-skip and clamps the very first
            // packet to zero. Its first decoder output is already correctly at
            // zero; later decoder instances still need their pre-skip restored.
            if (first === 0 && (!selected || selected.timestamp === first)) opusTimestampCorrection = 0;
          }
        }
      }
      for await (const sample of new AudioSampleSink(source.audio).samples(begin + sourceOrigin - preroll, end + sourceOrigin)) {
        try {
          throwIfAborted(options.signal);
          const timestamp = sample.timestamp + opusTimestampCorrection - sourceOrigin;
          const frameOffset = Math.max(0, Math.round((begin - timestamp) * sample.sampleRate));
          const frameEnd = Math.min(sample.numberOfFrames, Math.round((end - timestamp) * sample.sampleRate));
          const frameCount = frameEnd - frameOffset;
          if (frameCount <= 0) continue;
          const data = Array.from({ length: sample.numberOfChannels }, (_, planeIndex) => {
            const plane = new Float32Array(frameCount);
            sample.copyTo(plane, { planeIndex, format: 'f32-planar', frameOffset, frameCount });
            return plane;
          });
          yield { data, timestamp: timestamp + frameOffset / sample.sampleRate, sampleRate: sample.sampleRate, numberOfFrames: frameCount };
        } finally { sample.close(); }
      }
    } finally { release(); }
  }
  private async pcmChunk(url: string, index: number, signal?: AbortSignal): Promise<AudioBlock> {
    const key = `pcm:${this.identity(url)}:${index}`;
    const cached = this.pcm.get(key); if (cached) return cached;
    return this.audioJobs.run(key, async (jobSignal) => {
      const metadata = await this.probe(url, { signal: jobSignal });
      const sampleRate = metadata.sampleRate;
      if (!metadata.hasAudio || sampleRate <= 0) throw new Error('素材没有可用音频。');
      const count = Math.max(0, Math.min(sampleRate, Math.ceil((metadata.duration - index) * sampleRate)));
      const data = Array.from({ length: metadata.channels }, () => new Float32Array(count));
      if (count) for await (const block of this.audioChunks(url, index, index + 1, { signal: jobSignal })) {
        const offset = Math.round((block.timestamp - index) * sampleRate);
        const first = Math.max(0, -offset), last = Math.min(block.numberOfFrames, count - offset);
        if (last <= first) continue;
        for (let ch = 0; ch < data.length; ch++) data[ch].set(block.data[ch].subarray(first, last), offset + first);
      }
      throwIfAborted(jobSignal);
      const block = { data, timestamp: index, sampleRate, numberOfFrames: count };
      this.pcm.set(key, block, count * data.length * 4);
      return block;
    }, { signal, priority: -10 });
  }
  async audioWindow(url: string, begin: number, end: number, options: { signal?: AbortSignal } = {}): Promise<AudioBlock[]> {
    const first = Math.max(0, Math.floor(begin)), last = Math.max(first, Math.floor(end));
    if (last - first > 30) throw new RangeError('Read audio in bounded windows of at most 30 seconds');
    const blocks = [];
    for (let index = first; index <= last; index++) {
      throwIfAborted(options.signal);
      blocks.push(await this.pcmChunk(url, index, options.signal));
    }
    return blocks;
  }
  private async waveformChunk(url: string, index: number, signal?: AbortSignal): Promise<WaveformChunk> {
    const key = `peaks-v2:${this.identity(url)}:${index}`;
    const existing = this.peaks.get(key); if (existing) return existing;
    return this.waveformJobs.run(key, async (jobSignal) => {
      const stored = await this.artifacts.get<WaveformChunk>(key);
      if (stored?.levels?.length && stored.sampleRate > 0) {
        this.peaks.set(key, stored, waveformBytes(stored)); return stored;
      }
      const metadata = await this.probe(url, { signal: jobSignal });
      const start = index * 10, end = Math.min(metadata.duration, start + 10);
      if (end <= start || !metadata.hasAudio) throw new Error('波形区间没有音频。');
      const accumulator = new PeakAccumulator(Math.ceil((end - start) * metadata.sampleRate), metadata.channels, 256);
      await waitForMediaBackground(jobSignal);
      for await (const block of this.audioChunks(url, start, end, { signal: jobSignal })) {
        await waitForMediaBackground(jobSignal);
        accumulator.add(block.data, Math.round((block.timestamp - start) * metadata.sampleRate));
      }
      throwIfAborted(jobSignal);
      const chunk = { start, end, sampleRate: metadata.sampleRate, channels: metadata.channels, levels: accumulator.finish() };
      this.peaks.set(key, chunk, waveformBytes(chunk));
      void this.artifacts.put(key, chunk, waveformBytes(chunk));
      return chunk;
    }, { signal, priority: 10 });
  }
  async waveform(url: string, begin: number, end: number, columns: number, options: { signal?: AbortSignal; onProgress?: (seconds: number) => void } = {}): Promise<WaveformResult> {
    begin = finiteTime(begin); end = finiteTime(end);
    if (end <= begin || !Number.isSafeInteger(columns) || columns < 1 || columns > 8192) throw new RangeError('Invalid visible waveform range');
    const metadata = await this.probe(url, options);
    const channels = Array.from({ length: metadata.channels }, () => ({ min: new Float32Array(columns).fill(Infinity), max: new Float32Array(columns).fill(-Infinity) }));
    const secondsPerColumn = (end - begin) / columns;
    // Process chunks one at a time: a one-hour viewport never holds an hour of PCM.
    for (let index = Math.floor(begin / 10); index * 10 < Math.min(end, metadata.duration); index++) {
      throwIfAborted(options.signal);
      const chunk = await this.waveformChunk(url, index, options.signal);
      options.onProgress?.(Math.min(end, chunk.end));
      const idealBucketFrames = secondsPerColumn * chunk.sampleRate;
      let level = chunk.levels[0];
      for (const candidate of chunk.levels) if (candidate.bucketFrames <= idealBucketFrames) level = candidate;
      const bucketSeconds = level.bucketFrames / chunk.sampleRate;
      const firstColumn = Math.max(0, Math.floor((chunk.start - begin) / secondsPerColumn));
      const lastColumn = Math.min(columns, Math.ceil((chunk.end - begin) / secondsPerColumn));
      for (let col = firstColumn; col < lastColumn; col++) {
        const first = Math.max(0, Math.floor((begin + col * secondsPerColumn - chunk.start) / bucketSeconds));
        const last = Math.min(level.channels[0].min.length, Math.ceil((begin + (col + 1) * secondsPerColumn - chunk.start) / bucketSeconds));
        for (let ch = 0; ch < channels.length; ch++) for (let bucket = first; bucket < last; bucket++) {
          channels[ch].min[col] = Math.min(channels[ch].min[col], level.channels[ch].min[bucket]);
          channels[ch].max[col] = Math.max(channels[ch].max[col], level.channels[ch].max[bucket]);
        }
      }
    }
    for (const channel of channels) for (let col = 0; col < columns; col++) {
      if (!Number.isFinite(channel.min[col])) channel.min[col] = 0;
      if (!Number.isFinite(channel.max[col])) channel.max[col] = 0;
    }
    return { begin, end, channels };
  }
  async *mixAudio(project: Project, mediaUrl: (id: string) => string, beginTicks: number, endTicks: number,
    options: { sampleRate?: number; channels?: number; blockFrames?: number; signal?: AbortSignal } = {}): AsyncGenerator<AudioBlock> {
    const sampleRate = options.sampleRate ?? 48000, channelCount = options.channels ?? 2, blockFrames = options.blockFrames ?? 4096;
    if (channelCount !== 1 && channelCount !== 2) throw new Error('浏览器混音支持单声道或双声道输出。');
    if (!Number.isSafeInteger(sampleRate) || sampleRate < 8000 || sampleRate > 96000 || !Number.isSafeInteger(blockFrames) || blockFrames < 128 || blockFrames > 16384) throw new RangeError('Invalid mixer configuration');
    const totalFrames = Math.max(0, Math.ceil(seconds(endTicks - beginTicks) * sampleRate));
    const items = project.timeline.tracks.filter((track) => !track.muted).flatMap((track) => track.items)
      .filter((item) => item.enabled && item.clip.type !== 'text' && !item.clip.audio.muted);
    const assetById = new Map(project.assets.map((asset) => [asset.id, asset]));
    for (let cursor = 0; cursor < totalFrames; cursor += blockFrames) {
      throwIfAborted(options.signal);
      const count = Math.min(blockFrames, totalFrames - cursor);
      const blockBegin = beginTicks + cursor / sampleRate * 120000;
      const blockEnd = beginTicks + (cursor + count) / sampleRate * 120000;
      const data = Array.from({ length: channelCount }, () => new Float32Array(count));
      for (const item of items) {
        const asset = assetById.get(item.clip.assetId);
        if (!asset?.hasAudio || item.placement.end <= blockBegin || item.placement.begin >= blockEnd) continue;
        const first = Math.max(0, Math.ceil(seconds(item.placement.begin - blockBegin) * sampleRate));
        const last = Math.min(count, Math.ceil(seconds(item.placement.end - blockBegin) * sampleRate));
        const firstSource = seconds(mapTimelineToSource(item, blockBegin + first / sampleRate * 120000));
        const lastSource = seconds(mapTimelineToSource(item, blockBegin + (last - 1) / sampleRate * 120000));
        const windows = await this.audioWindow(mediaUrl(asset.id), Math.min(firstSource, lastSource), Math.max(firstSource, lastSource) + 1 / sampleRate, options);
        if (windows[0]?.data.length > 2) throw new Error('多声道素材需先选择单声道或立体声轨道；当前不能准确混合其声道布局。');
        for (let frame = first; frame < last; frame++) {
          const timeline = blockBegin + frame / sampleRate * 120000;
          const sourceTime = seconds(mapTimelineToSource(item, timeline));
          const audio = evaluateAudio(item, timeline);
          if (audio.muted) continue;
          const window = windows.find((block) => sourceTime >= block.timestamp && sourceTime < block.timestamp + block.numberOfFrames / block.sampleRate);
          if (!window) continue;
          const position = (sourceTime - window.timestamp) * window.sampleRate;
          for (let ch = 0; ch < channelCount; ch++) {
            const value = channelCount === 1 && window.data.length === 2
              ? (sampleLinear(window.data[0], position) + sampleLinear(window.data[1], position)) / 2
              : sampleLinear(window.data[Math.min(ch, window.data.length - 1)], position);
            data[ch][frame] += value * audio.gainLinear;
          }
        }
      }
      // A shared hard limiter keeps preview/export PCM finite and in range.
      for (const channel of data) for (let i = 0; i < channel.length; i++) channel[i] = Math.max(-1, Math.min(1, channel[i]));
      yield { data, timestamp: seconds(beginTicks) + cursor / sampleRate, sampleRate, numberOfFrames: count };
    }
  }
  dispose() {
    if (this.disposed) return;
    this.disposed = true; this.jobs.dispose(); this.audioJobs.dispose(); this.thumbnailJobs.dispose(); this.waveformJobs.dispose();
    this.frames.clear(); this.pcm.clear(); this.peaks.clear(); this.index.clear();
    for (const entry of this.sources.values()) { entry.retired = true; entry.input.dispose(); }
    this.sources.clear();
  }
}

function waveformBytes(chunk: WaveformChunk) {
  return chunk.levels.reduce((sum, level) => sum + level.channels.reduce((bytes, ch) => bytes + ch.min.byteLength + ch.max.byteLength, 0), 0);
}

/** Bounded Web Audio lookahead. Web Audio's monotonic clock is the transport master. */
export class PreviewAudioPlayer {
  private context?: AudioContext;
  private controller?: AbortController;
  private nodes = new Set<AudioBufferSourceNode>();
  private project?: Project;
  private originTicks = 0;
  private originClock = Number.NaN;
  private onFailure?: (error: unknown) => void;
  constructor(private readonly engine: MediaEngine, private readonly mediaUrl: (id: string) => string, onFailure?: (error: unknown) => void) { this.onFailure = onFailure; }
  async play(project: Project, beginTicks: number) {
    this.stop(); this.project = project; this.originTicks = beginTicks; this.originClock = Number.NaN;
    const controller = this.controller = new AbortController();
    this.context ??= new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
    try {
      await this.context.resume();
      throwIfAborted(controller.signal);
      const iterator = this.engine.mixAudio(project, this.mediaUrl, beginTicks, duration(project), { signal: controller.signal, blockFrames: 4096 });
      const first = await iterator.next();
      if (controller.signal.aborted) { await iterator.return(undefined); throw abortError(); }
      this.originClock = this.context.currentTime + 0.06;
      void this.schedule(beginTicks, controller, iterator, first).catch((error) => {
        if (!controller.signal.aborted) { this.stop(); this.onFailure?.(error); }
      });
    } catch (error) {
      if (this.controller === controller) this.stop();
      throw error;
    }
  }
  private async schedule(beginTicks: number, controller: AbortController, iterator: AsyncGenerator<AudioBlock>, first: IteratorResult<AudioBlock>) {
    const context = this.context!;
    try { for (let next = first; !next.done; next = await iterator.next()) {
      const block = next.value;
      throwIfAborted(controller.signal);
      const start = this.originClock + block.timestamp - seconds(beginTicks);
      while (start - context.currentTime > 0.4) {
        await new Promise<void>((resolve) => setTimeout(resolve, 20));
        throwIfAborted(controller.signal);
      }
      const buffer = context.createBuffer(block.data.length, block.numberOfFrames, block.sampleRate);
      block.data.forEach((plane, channel) => buffer.copyToChannel(plane as Float32Array<ArrayBuffer>, channel));
      const node = context.createBufferSource(); node.buffer = buffer; node.connect(context.destination);
      this.nodes.add(node); node.onended = () => { this.nodes.delete(node); node.disconnect(); };
      const late = Math.max(0, context.currentTime - start);
      if (late < buffer.duration) node.start(Math.max(start, context.currentTime), late);
      else { this.nodes.delete(node); node.disconnect(); }
    } } finally { await iterator.return(undefined); }
  }
  seek(timelineTicks: number) { return this.project ? this.play(this.project, timelineTicks) : Promise.resolve(); }
  currentTimeTicks() { return this.controller && this.context && Number.isFinite(this.originClock) ? this.originTicks + ticks(Math.max(0, this.context.currentTime - this.originClock)) : this.originTicks; }
  stop() {
    if (this.controller && this.context) this.originTicks = this.currentTimeTicks();
    this.controller?.abort(); this.controller = undefined;
    for (const node of this.nodes) { try { node.stop(); } catch {} node.disconnect(); }
    this.nodes.clear();
  }
  async dispose() { this.stop(); await this.context?.close(); this.context = undefined; }
}

export const sharedMediaEngine = new MediaEngine();
