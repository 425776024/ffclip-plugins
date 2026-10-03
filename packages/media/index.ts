import { ByteCache } from './runtime.mjs';
import { MediaArtifactStore } from './artifacts';
import type { MediaMetadata } from './browser';

export type IndexedMediaMetadata = Omit<MediaMetadata, 'videoDecodable' | 'audioDecodable'>;
type Page = { timestamps: Float64Array; durations: Float64Array };
type Observation = { timestamp: number; duration: number };
const pageSeconds = 10;
const metadataKey = (identity: string) => `index-metadata-v1:${identity}`;
const pageKey = (identity: string, page: number) => `index-pts-v1:${identity}:${page}`;
function mergePage(previous: Page | undefined, observations: Observation[]): Page {
  const values = new Map<number, number>();
  previous?.timestamps.forEach((timestamp, index) =>
    values.set(timestamp, previous.durations[index])
  );
  for (const value of observations)
    if (Number.isFinite(value.timestamp) && Number.isFinite(value.duration) && value.duration > 0)
      values.set(value.timestamp, value.duration);
  // This is a sparse index of observed frame intervals, not a guessed CFR index.
  // Dropping old observations only causes a future real decode, never wrong PTS.
  const entries = [...values].sort((a, b) => a[0] - b[0]).slice(-4096);
  return {
    timestamps: Float64Array.from(entries, (entry) => entry[0]),
    durations: Float64Array.from(entries, (entry) => entry[1])
  };
}

/** Immutable source facts shared between independent decoder cursors via bounded IDB pages. */
export class MediaIndexStore {
  private metadataCache = new ByteCache<IndexedMediaMetadata>(1024 * 1024);
  private pages = new ByteCache<Page>(2 * 1024 * 1024);
  private loading = new Map<string, Promise<Page | undefined>>();
  constructor(private readonly artifacts: MediaArtifactStore) {}
  stats() {
    return { indexBytes: this.metadataCache.bytes + this.pages.bytes, indexHits: this.pages.hits };
  }
  clear() {
    this.metadataCache.clear();
    this.pages.clear();
  }
  async metadata(identity: string) {
    const key = metadataKey(identity),
      cached = this.metadataCache.get(key);
    if (cached) return cached;
    const stored = await this.artifacts.get<IndexedMediaMetadata>(key);
    if (stored)
      this.metadataCache.set(
        key,
        Object.freeze(stored),
        new TextEncoder().encode(JSON.stringify(stored)).length
      );
    return stored;
  }
  async saveMetadata(identity: string, value: MediaMetadata) {
    const { videoDecodable: _video, audioDecodable: _audio, ...metadata } = value;
    const key = metadataKey(identity),
      bytes = new TextEncoder().encode(JSON.stringify(metadata)).length;
    this.metadataCache.set(key, Object.freeze(metadata), bytes);
    await this.artifacts.put(key, metadata, bytes);
  }
  private async page(key: string) {
    const cached = this.pages.get(key);
    if (cached) return cached;
    if (!this.loading.has(key))
      this.loading.set(
        key,
        this.artifacts
          .get<Page>(key)
          .then((value) => {
            if (value)
              this.pages.set(key, value, value.timestamps.byteLength + value.durations.byteLength);
            return value;
          })
          .finally(() => this.loading.delete(key))
      );
    return this.loading.get(key);
  }
  async resolve(identity: string, time: number): Promise<Observation | undefined> {
    const page = await this.page(pageKey(identity, Math.floor(time / pageSeconds)));
    if (!page) return;
    let low = 0,
      high = page.timestamps.length;
    while (low < high) {
      const middle = (low + high) >>> 1;
      if (page.timestamps[middle] <= time + 1e-8) low = middle + 1;
      else high = middle;
    }
    const index = low - 1;
    if (index >= 0 && time < page.timestamps[index] + page.durations[index] - 1e-8)
      return { timestamp: page.timestamps[index], duration: page.durations[index] };
  }
  async observe(identity: string, observations: Observation[]) {
    const groups = new Map<number, Observation[]>();
    for (const value of observations) {
      if (!Number.isFinite(value.duration) || value.duration <= 0) continue;
      const first = Math.floor(value.timestamp / pageSeconds),
        last = Math.floor((value.timestamp + value.duration - 1e-8) / pageSeconds);
      for (let page = first; page <= Math.min(last, first + 16); page++) {
        if (!groups.has(page)) groups.set(page, []);
        groups.get(page)!.push(value);
      }
    }
    await Promise.all(
      [...groups].map(async ([page, values]) => {
        const key = pageKey(identity, page),
          cached = mergePage(await this.page(key), values);
        this.pages.set(key, cached, cached.timestamps.byteLength + cached.durations.byteLength);
        await this.artifacts.update<Page>(key, (previous) => {
          const value = mergePage(previous, values);
          return { value, bytes: value.timestamps.byteLength + value.durations.byteLength };
        });
      })
    );
  }
}
