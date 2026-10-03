import type { MediaFrame, MediaMetadata, FrameOptions, ThumbnailOptions, WaveformResult } from './browser';
import { abortError } from './runtime.mjs';

/** Small main-thread RPC facade. Codecs, peaks, and resizing stay in the worker. */
export class MediaWorkerClient {
  private worker?: Worker;
  private serial = 0;
  private readonly pending = new Map<number, { resolve: (value: unknown) => void; reject: (error: unknown) => void; cleanup: () => void }>();
  private getWorker() {
    if (!this.worker) {
      this.worker = new Worker(new URL('./worker.ts', import.meta.url), { type: 'module', name: 'videocut-media' });
      this.worker.onmessage = ({ data }) => {
        const request = this.pending.get(data.id);
        if (!request) {
          const values = Array.isArray(data.result) ? data.result : [data.result];
          for (const value of values) value?.frame?.close?.();
          return;
        }
        this.pending.delete(data.id); request.cleanup();
        if (data.error) request.reject(Object.assign(new Error(data.error.message), { name: data.error.name }));
        else request.resolve(data.result);
      };
      this.worker.onerror = (event) => {
        for (const request of this.pending.values()) { request.cleanup(); request.reject(new Error(event.message || 'Media worker failed')); }
        this.pending.clear(); this.worker?.terminate(); this.worker = undefined;
      };
    }
    return this.worker;
  }
  private call<T>(method: string, args: unknown[], signal?: AbortSignal): Promise<T> {
    if (signal?.aborted) return Promise.reject(abortError());
    const worker = this.getWorker(), id = ++this.serial;
    return new Promise((resolve, reject) => {
      const cancel = () => { this.pending.delete(id); signal?.removeEventListener('abort', cancel); worker.postMessage({ type: 'cancel', id }); reject(abortError()); };
      this.pending.set(id, { resolve: (value) => resolve(value as T), reject, cleanup: () => signal?.removeEventListener('abort', cancel) });
      signal?.addEventListener('abort', cancel, { once: true });
      worker.postMessage({ id, method, args });
    });
  }
  private owned(value: Omit<MediaFrame, 'close'>): MediaFrame { return { ...value, close: () => value.frame.close() }; }
  probe(url: string, options: { signal?: AbortSignal; kind?: string } = {}) { return this.call<MediaMetadata>('probe', [url, { kind: options.kind }], options.signal); }
  async videoFrame(url: string, time: number, options: FrameOptions = {}) {
    const { signal, ...config } = options;
    return this.owned(await this.call<Omit<MediaFrame, 'close'>>('videoFrame', [url, time, config], signal));
  }
  async image(url: string, options: FrameOptions = {}) {
    const { signal, ...config } = options;
    return this.owned(await this.call<Omit<MediaFrame, 'close'>>('image', [url, config], signal));
  }
  async thumbnails(url: string, times: number[], options: ThumbnailOptions = {}) {
    const { signal, ...config } = options;
    return (await this.call<Omit<MediaFrame, 'close'>[]>('thumbnails', [url, times, config], signal)).map((value) => this.owned(value));
  }
  waveform(url: string, begin: number, end: number, columns: number, options: { signal?: AbortSignal } = {}) {
    return this.call<WaveformResult>('waveform', [url, begin, end, columns], options.signal);
  }
  invalidate(url?: string) { return this.call('invalidate', [url]); }
  stats() { return this.call<Record<string, number>>('stats', []); }
  persistentUsage() { return this.call<Record<string, number>>('persistentUsage', []); }
  dispose() {
    for (const request of this.pending.values()) { request.cleanup(); request.reject(abortError()); }
    this.pending.clear(); this.worker?.terminate(); this.worker = undefined;
  }
}

export const sharedMediaClient = new MediaWorkerClient();
