import type { VisionRequest, VisionResult, VisionProgress } from './types';
export class VisionWorkerClient {
  private worker?: Worker;
  private active?: { reject: (error: Error) => void; cleanup: () => void };
  analyze(
    request: VisionRequest,
    options: { signal?: AbortSignal; onProgress?: (progress: VisionProgress) => void } = {}
  ): Promise<VisionResult> {
    if (options.signal?.aborted)
      return Promise.reject(new DOMException('Vision cancelled', 'AbortError'));
    if (this.active) return Promise.reject(new Error('Vision worker is already running'));
    return new Promise((resolve, reject) => {
      const worker = (this.worker = new Worker(new URL('./worker.ts', import.meta.url), {
        type: 'module',
        name: 'videocut-vision'
      }));
      const cancel = () => this.dispose();
      const cleanup = () => {
        options.signal?.removeEventListener('abort', cancel);
        worker.terminate();
        this.worker = undefined;
        this.active = undefined;
      };
      this.active = { reject, cleanup };
      worker.onmessage = ({ data }) => {
        if (data.type === 'progress') {
          options.onProgress?.(data.progress);
          return;
        }
        cleanup();
        if (data.error)
          reject(
            Object.assign(new Error(data.error.message), { name: data.error.name || 'Error' })
          );
        else resolve(data.result);
      };
      worker.onerror = (event) => {
        cleanup();
        reject(new Error(event.message || 'Vision worker failed'));
      };
      worker.onmessageerror = () => {
        cleanup();
        reject(new Error('Invalid Vision worker message'));
      };
      options.signal?.addEventListener('abort', cancel, { once: true });
      try {
        worker.postMessage(request);
      } catch (error) {
        cleanup();
        reject(error);
      }
    });
  }
  dispose() {
    const active = this.active;
    active?.cleanup();
    active?.reject(new DOMException('Vision cancelled', 'AbortError'));
    this.worker?.terminate();
    this.worker = undefined;
  }
}
