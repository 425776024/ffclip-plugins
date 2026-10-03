export type ArtifactKind = 'thumbnail' | 'waveform' | 'index';
type Usage = { key: string; kind: ArtifactKind; bytes: number; touched: number };
const budgets: Record<ArtifactKind, number> = {
  thumbnail: 64 * 1024 ** 2,
  waveform: 56 * 1024 ** 2,
  index: 8 * 1024 ** 2
};
const kindOf = (key: string): ArtifactKind =>
  key.startsWith('thumbnail') ? 'thumbnail' : key.startsWith('peaks') ? 'waveform' : 'index';

/** Shared, disposable artifacts only. Stored keys never contain paths, tokens or URLs. */
export class MediaArtifactStore {
  private database?: Promise<IDBDatabase | null>;
  private hashes = new Map<string, Promise<string>>();
  readonly counters = { hits: 0, misses: 0, writes: 0, evictions: 0 };
  constructor(
    private readonly maximumBytes = 128 * 1024 * 1024,
    private readonly databaseName = 'videocut-media-artifacts-v1'
  ) {}
  private async key(value: string) {
    let pending = this.hashes.get(value);
    if (!pending) {
      pending = crypto.subtle
        .digest('SHA-256', new TextEncoder().encode(value))
        .then(
          (bytes) =>
            `${kindOf(value)}:${Array.from(new Uint8Array(bytes), (b) => b.toString(16).padStart(2, '0')).join('')}`
        );
      this.hashes.set(value, pending);
      if (this.hashes.size > 512) this.hashes.delete(this.hashes.keys().next().value!);
    }
    return pending;
  }
  private open(): Promise<IDBDatabase | null> {
    return (this.database ??= new Promise((resolve) => {
      if (typeof indexedDB === 'undefined') return resolve(null);
      const request = indexedDB.open(this.databaseName, 3);
      request.onupgradeneeded = () => {
        const db = request.result;
        for (const name of ['artifacts', 'usage']) {
          if (!db.objectStoreNames.contains(name)) db.createObjectStore(name, { keyPath: 'key' });
          else request.transaction!.objectStore(name).clear();
        }
      };
      request.onsuccess = () => {
        request.result.onversionchange = () => {
          request.result.close();
          this.database = undefined;
        };
        resolve(request.result);
      };
      request.onerror = request.onblocked = () => resolve(null);
    }));
  }
  async get<T>(identity: string): Promise<T | undefined> {
    const db = await this.open();
    if (!db) return undefined;
    const key = await this.key(identity);
    return new Promise((resolve) => {
      const transaction = db.transaction(['artifacts', 'usage'], 'readwrite');
      const request = transaction.objectStore('artifacts').get(key);
      request.onsuccess = () => {
        const entry = request.result;
        if (entry) {
          this.counters.hits++;
          transaction
            .objectStore('usage')
            .put({ key, kind: kindOf(identity), bytes: entry.bytes, touched: Date.now() });
        } else this.counters.misses++;
        resolve(entry?.value);
      };
      request.onerror = transaction.onabort = () => resolve(undefined);
    });
  }
  private evict(transaction: IDBTransaction) {
    const usage = transaction.objectStore('usage'),
      artifacts = transaction.objectStore('artifacts');
    const rows: Usage[] = [],
      request = usage.openCursor();
    request.onsuccess = () => {
      const cursor = request.result;
      if (cursor) {
        rows.push(cursor.value);
        cursor.continue();
        return;
      }
      const sums = { thumbnail: 0, waveform: 0, index: 0 };
      let total = 0;
      for (const row of rows) {
        sums[row.kind] += row.bytes;
        total += row.bytes;
      }
      rows.sort((a, b) => a.touched - b.touched);
      for (const row of rows) {
        if (total <= this.maximumBytes && sums[row.kind] <= budgets[row.kind]) continue;
        artifacts.delete(row.key);
        usage.delete(row.key);
        total -= row.bytes;
        sums[row.kind] -= row.bytes;
        this.counters.evictions++;
      }
    };
  }
  async put(identity: string, value: unknown, bytes: number): Promise<void> {
    return this.update(identity, () => ({ value, bytes }));
  }
  /** Read/merge/write stays in one IDB transaction, including across workers. */
  async update<T>(
    identity: string,
    merge: (previous: T | undefined) => { value: T; bytes: number }
  ): Promise<void> {
    const db = await this.open();
    if (!db) return;
    const key = await this.key(identity),
      kind = kindOf(identity);
    await new Promise<void>((resolve) => {
      const transaction = db.transaction(['artifacts', 'usage'], 'readwrite');
      const artifacts = transaction.objectStore('artifacts'),
        request = artifacts.get(key);
      request.onsuccess = () => {
        const next = merge(request.result?.value);
        if (
          !Number.isSafeInteger(next.bytes) ||
          next.bytes < 0 ||
          next.bytes > Math.min(this.maximumBytes, budgets[kind])
        )
          return;
        artifacts.put({ key, value: next.value, bytes: next.bytes });
        transaction.objectStore('usage').put({ key, kind, bytes: next.bytes, touched: Date.now() });
        this.counters.writes++;
        this.evict(transaction);
      };
      transaction.oncomplete = transaction.onerror = transaction.onabort = () => resolve();
    });
  }
  async usage() {
    const db = await this.open();
    const result = {
      bytes: 0,
      thumbnailBytes: 0,
      waveformBytes: 0,
      indexBytes: 0,
      entries: 0,
      maximumBytes: this.maximumBytes
    };
    if (!db) return result;
    return new Promise<typeof result>((resolve) => {
      const transaction = db.transaction('usage', 'readonly'),
        request = transaction.objectStore('usage').openCursor();
      request.onsuccess = () => {
        const cursor = request.result;
        if (!cursor) {
          resolve(result);
          return;
        }
        const row = cursor.value as Usage;
        result.bytes += row.bytes;
        result[`${row.kind}Bytes`] += row.bytes;
        result.entries++;
        cursor.continue();
      };
      request.onerror = transaction.onabort = () => resolve(result);
    });
  }
  async close() {
    const database = await this.database;
    database?.close();
    this.database = undefined;
    this.hashes.clear();
  }
}
