/** Browser-independent scheduling and numeric media primitives. */
export function abortError() {
  return new DOMException('Media work was cancelled', 'AbortError');
}

export function throwIfAborted(signal) {
  if (signal?.aborted) throw signal.reason instanceof Error ? signal.reason : abortError();
}

/** Authorized URLs may change with sessions; a verified filesystem version does not. */
export function sourceCacheIdentity(url) {
  try {
    const parsed = new URL(url, globalThis.location?.href);
    const source = parsed.searchParams.get('source');
    // Accept only the server's complete dev:ino:size:mtime signature, never a
    // bare byte size or caller label which could alias unrelated media.
    if (source && /^\d+:\d+:\d+:\d+(?:\.\d+)?$/.test(source))
      return `${parsed.origin}/media-source/${source}`;
  } catch {}
  return url;
}

/** Byte-accounted LRU. Values may own resources through close(). */
export class ByteCache {
  constructor(maximumBytes) {
    this.maximumBytes = maximumBytes;
    this.bytes = 0;
    this.entries = new Map();
    this.hits = 0;
    this.misses = 0;
  }
  get(key) {
    const entry = this.entries.get(key);
    if (!entry) { this.misses++; return undefined; }
    this.hits++;
    this.entries.delete(key);
    this.entries.set(key, entry);
    return entry.value;
  }
  set(key, value, bytes) {
    if (!Number.isSafeInteger(bytes) || bytes < 0) throw new RangeError('Invalid cache byte size');
    this.delete(key);
    this.entries.set(key, { value, bytes });
    this.bytes += bytes;
    while (this.bytes > this.maximumBytes && this.entries.size) this.delete(this.entries.keys().next().value);
    return value;
  }
  delete(key) {
    const entry = this.entries.get(key);
    if (!entry) return;
    this.entries.delete(key);
    this.bytes -= entry.bytes;
    entry.value?.close?.();
  }
  clear() { for (const key of this.entries.keys()) this.delete(key); }
}

/** One producer per key; cancellation belongs to consumers, not the shared work. */
export class MediaScheduler {
  constructor(concurrency = 2) {
    if (!Number.isSafeInteger(concurrency) || concurrency < 1) throw new RangeError('Invalid concurrency');
    this.concurrency = concurrency;
    this.active = 0;
    this.jobs = new Map();
    this.queue = [];
    this.disposed = false;
    this.completed = 0;
    this.cancelled = 0;
  }
  run(key, produce, { signal, priority = 0, consume = (value) => value, afterDispatch } = {}) {
    if (this.disposed || signal?.aborted) return Promise.reject(abortError());
    let job = this.jobs.get(key);
    if (!job) {
      job = { key, produce, priority, afterDispatch, controller: new AbortController(), consumers: new Set(), started: false };
      this.jobs.set(key, job);
      this.queue.push(job);
    }
    job.priority = Math.min(job.priority, priority);
    return new Promise((resolve, reject) => {
      const consumer = { resolve, reject, consume, cleanup: () => {} };
      const cancel = () => {
        if (!job.consumers.delete(consumer)) return;
        consumer.cleanup();
        reject(abortError());
        if (!job.consumers.size) {
          this.cancelled++;
          job.controller.abort();
          if (this.jobs.get(key) === job) this.jobs.delete(key);
          if (!job.started) this.queue = this.queue.filter((candidate) => candidate !== job);
        }
      };
      consumer.cleanup = () => signal?.removeEventListener('abort', cancel);
      job.consumers.add(consumer);
      signal?.addEventListener('abort', cancel, { once: true });
      queueMicrotask(() => this.pump());
    });
  }
  pump() {
    this.queue.sort((a, b) => a.priority - b.priority);
    while (!this.disposed && this.active < this.concurrency && this.queue.length) {
      const job = this.queue.shift();
      if (!job.consumers.size) continue;
      job.started = true;
      this.active++;
      Promise.resolve().then(() => {
        throwIfAborted(job.controller.signal);
        return job.produce(job.controller.signal);
      }).then((value) => {
        this.completed++;
        for (const c of job.consumers) {
          c.cleanup();
          try { c.resolve(c.consume(value)); } catch (error) { c.reject(error); }
        }
        job.afterDispatch?.(value);
      }, (error) => {
        for (const c of job.consumers) { c.cleanup(); c.reject(error); }
      }).finally(() => {
        job.consumers.clear();
        if (this.jobs.get(job.key) === job) this.jobs.delete(job.key);
        this.active--;
        this.pump();
      });
    }
  }
  dispose() {
    this.disposed = true;
    for (const job of this.jobs.values()) {
      job.controller.abort();
      for (const c of job.consumers) { c.cleanup(); c.reject(abortError()); }
      job.consumers.clear();
    }
    this.jobs.clear();
    this.queue.length = 0;
  }
}

/** Retains channel extrema, including quiet negative and positive transients. */
export class PeakAccumulator {
  constructor(numberOfFrames, channels, bucketFrames = 256) {
    if (!Number.isSafeInteger(numberOfFrames) || numberOfFrames < 0 || !Number.isSafeInteger(channels) || channels < 1 || !Number.isSafeInteger(bucketFrames) || bucketFrames < 1) throw new RangeError('Invalid waveform shape');
    this.numberOfFrames = numberOfFrames;
    this.bucketFrames = bucketFrames;
    const count = Math.ceil(numberOfFrames / bucketFrames);
    this.channels = Array.from({ length: channels }, () => ({ min: new Float32Array(count).fill(Infinity), max: new Float32Array(count).fill(-Infinity) }));
  }
  add(data, offsetFrames) {
    for (let ch = 0; ch < this.channels.length; ch++) {
      const source = data[Math.min(ch, data.length - 1)];
      if (!source) continue;
      const output = this.channels[ch];
      const start = Math.max(0, -offsetFrames);
      const end = Math.min(source.length, this.numberOfFrames - offsetFrames);
      for (let i = start; i < end; i++) {
        const bucket = Math.floor((i + offsetFrames) / this.bucketFrames);
        const value = Number.isFinite(source[i]) ? source[i] : 0;
        output.min[bucket] = Math.min(output.min[bucket], value);
        output.max[bucket] = Math.max(output.max[bucket], value);
      }
    }
  }
  finish() {
    for (const channel of this.channels) for (let i = 0; i < channel.min.length; i++) {
      if (!Number.isFinite(channel.min[i])) channel.min[i] = 0;
      if (!Number.isFinite(channel.max[i])) channel.max[i] = 0;
    }
    const levels = [{ bucketFrames: this.bucketFrames, channels: this.channels }];
    while (levels.at(-1).channels[0].min.length > 1) {
      const previous = levels.at(-1);
      const channels = previous.channels.map(({ min, max }) => {
        const lower = new Float32Array(Math.ceil(min.length / 2));
        const upper = new Float32Array(lower.length);
        for (let i = 0; i < lower.length; i++) {
          lower[i] = Math.min(min[i * 2], min[Math.min(i * 2 + 1, min.length - 1)]);
          upper[i] = Math.max(max[i * 2], max[Math.min(i * 2 + 1, max.length - 1)]);
        }
        return { min: lower, max: upper };
      });
      levels.push({ bucketFrames: previous.bucketFrames * 2, channels });
    }
    return levels;
  }
}

export function rasterSize(width, height, displayHeight, dpr = 1) {
  const targetHeight = Math.min(512, Math.max(32, Math.ceil(displayHeight * Math.max(1, dpr) / 8) * 8));
  const scale = Math.min(1, targetHeight / Math.max(1, height));
  return { width: Math.max(1, Math.round(width * scale)), height: Math.max(1, Math.round(height * scale)) };
}

/** Pixel geometry for a cropped thumbnail slot after rotation and pixel-aspect correction. */
export function thumbnailGeometry(sourceWidth, sourceHeight, displayWidth = 80, displayHeight = 48, dpr = 1, fit = 'cover') {
  for (const value of [sourceWidth, sourceHeight, displayWidth, displayHeight, dpr])
    if (!Number.isFinite(value) || value <= 0) throw new RangeError('Invalid thumbnail dimensions');
  if (!['cover', 'contain'].includes(fit)) throw new RangeError('Invalid thumbnail fit');
  const ratio = displayWidth / displayHeight;
  const wantedWidth = Math.min(1024, Math.ceil(displayWidth * Math.min(4, dpr)));
  const wantedHeight = Math.min(1024, Math.ceil(displayHeight * Math.min(4, dpr)));
  const availableWidth = fit === 'cover' ? Math.min(sourceWidth, sourceHeight * ratio) : Math.max(sourceWidth, sourceHeight * ratio);
  const width = Math.max(1, Math.min(wantedWidth, Math.round(availableWidth)));
  const height = Math.max(1, Math.min(wantedHeight, Math.round(width / ratio)));
  const scale = fit === 'cover' ? Math.max(width / sourceWidth, height / sourceHeight) : Math.min(width / sourceWidth, height / sourceHeight);
  return { width, height, x: (width - sourceWidth * scale) / 2, y: (height - sourceHeight * scale) / 2,
    drawWidth: sourceWidth * scale, drawHeight: sourceHeight * scale };
}

/** Linear sample interpolation, with explicit silence outside the decoded window. */
export function sampleLinear(data, position) {
  if (!data || position < 0 || position >= data.length || !Number.isFinite(position)) return 0;
  const a = Math.floor(position), fraction = position - a;
  return data[a] * (1 - fraction) + data[Math.min(a + 1, data.length - 1)] * fraction;
}
