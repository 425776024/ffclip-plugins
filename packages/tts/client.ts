import type { TtsProgress, TtsRequest, TtsResult } from './types';
export type { TtsProgress, TtsRequest, TtsResult, TtsBackend, TtsDtype } from './types';

/** One synthesis per client; cancellation stops computation, including an active GPU run. */
export class TtsWorkerClient {
  private worker?: Worker;
  private active?: { reject: (error: Error) => void; cleanup: () => void };
  synthesize(request: TtsRequest, options: { signal?: AbortSignal; onProgress?: (progress: TtsProgress) => void } = {}): Promise<TtsResult> {
    if (options.signal?.aborted) return Promise.reject(new DOMException('Speech synthesis cancelled', 'AbortError'));
    if (this.active) return Promise.reject(new Error('This speech worker is already synthesizing'));
    return new Promise((resolve, reject) => {
      const worker = new Worker(new URL('./worker.ts', import.meta.url), { type: 'module', name: 'videocut-tts' });
      this.worker = worker;
      const cancel = () => this.dispose();
      const cleanup = () => {
        options.signal?.removeEventListener('abort', cancel);
        worker.terminate();
        this.worker = undefined;
        this.active = undefined;
      };
      this.active = { reject, cleanup };
      worker.onmessage = ({ data }) => {
        if (data.type === 'progress') { options.onProgress?.(data.progress); return; }
        cleanup();
        if (data.error) reject(Object.assign(new Error(data.error.message), { name: data.error.name || 'Error' }));
        else resolve(data.result);
      };
      worker.onerror = (event) => { cleanup(); reject(new Error(event.message || 'Speech worker failed')); };
      worker.onmessageerror = () => { cleanup(); reject(new Error('Speech worker returned an invalid message')); };
      options.signal?.addEventListener('abort', cancel, { once: true });
      try { worker.postMessage(request); }
      catch (error) { cleanup(); reject(error); }
    });
  }
  dispose() {
    const active = this.active;
    active?.cleanup();
    active?.reject(new DOMException('Speech synthesis cancelled', 'AbortError'));
    this.worker?.terminate();
    this.worker = undefined;
  }
}
