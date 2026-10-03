import type { TextContent } from '../core/types';
import type { TextFrame } from '../text-wasm/dist/index.mjs';
import { resolveRecipe } from '../text-wasm/src/recipes.mjs';
import type { TextLayoutInfo } from '../core/text-layout.mjs';
export type PreviewTextFrame = TextFrame & { textLayout?: TextLayoutInfo };

export interface TextFrameRequest {
  /** Exact immutable source identity, including template, text, extent and native sample time. */
  key: string;
  template: NonNullable<TextContent['template']>;
  text: string;
  width: number;
  height: number;
  timeUs: number;
  layout?: { layoutWidth?: number; projectWidth: number; projectHeight: number };
}
export interface TextWorkerLike {
  postMessage(message: unknown, transfer?: Transferable[]): void;
  terminate(): void;
  onmessage: ((event: MessageEvent) => void) | null;
  onerror: ((event: ErrorEvent) => void) | null;
  onmessageerror: ((event: MessageEvent) => void) | null;
}
export interface TextPrefetchOptions {
  workerFactory?: () => TextWorkerLike;
  workers?: number;
  maxBytes?: number;
  maxEntries?: number;
}
export interface TextPrefetchStats {
  readyHits: number;
  awaitedHits: number;
  rendered: number;
  bytes: number;
  active: number;
  queued: number;
  staleDrops: number;
  workers: number;
}
export type TextWorkerReply =
  | { id: number; frame: PreviewTextFrame }
  | { id: number; error: { message: string; name?: string; stack?: string; code?: string } };

export class TextWorkerUnavailableError extends Error {
  readonly code = 'TEXT_WORKER_UNAVAILABLE';
  constructor(message = 'Native text worker is unavailable') {
    super(message);
    this.name = 'TextWorkerUnavailableError';
  }
}
const abortError = () => new DOMException('Native text frame request was cancelled', 'AbortError');
function freezeMetadata(value: unknown): void {
  if (
    !value ||
    typeof value !== 'object' ||
    ArrayBuffer.isView(value) ||
    value instanceof ArrayBuffer ||
    Object.isFrozen(value)
  )
    return;
  for (const child of Object.values(value)) freezeMetadata(child);
  Object.freeze(value);
}
function validFrame(frame: TextFrame, input: TextFrameRequest) {
  return (
    frame &&
    frame.ok === true &&
    frame.data instanceof Uint8ClampedArray &&
    [frame.width, frame.height, frame.originX, frame.originY, frame.timeUs, frame.rowBytes].every(
      Number.isSafeInteger
    ) &&
    frame.width > 0 &&
    frame.height > 0 &&
    frame.width <= input.width &&
    frame.height <= input.height &&
    frame.timeUs >= 0 &&
    frame.originX >= -frame.width &&
    frame.originX <= input.width &&
    frame.originY >= -frame.height &&
    frame.originY <= input.height &&
    frame.rowBytes === frame.width * 4 &&
    frame.data.byteLength === frame.width * frame.height * 4
  );
}

/** Only native flower packages are eligible; unsupported templates are real input errors. */
export function validateTextFrameRequest(input: TextFrameRequest) {
  if (
    !input ||
    typeof input.key !== 'string' ||
    !input.key ||
    typeof input.text !== 'string' ||
    !input.text ||
    !Number.isInteger(input.width) ||
    !Number.isInteger(input.height) ||
    input.width < 1 ||
    input.height < 1 ||
    input.width > 4096 ||
    input.height > 4096 ||
    input.width * input.height > 8388608 ||
    !Number.isSafeInteger(input.timeUs) ||
    input.timeUs < 0 ||
    !input.template
  )
    throw new Error('Invalid native text frame request');
  const recipe = resolveRecipe(input.template);
  if (recipe.external || !['flower-style-03', 'flower-style-38'].includes(recipe.base))
    throw new Error('Native text prefetch supports only flower-style-03 and flower-style-38');
}
interface Waiter {
  resolve(frame: TextFrame): void;
  reject(error: Error): void;
  signal?: AbortSignal;
  abort?: () => void;
}
interface Job {
  id: number;
  input: TextFrameRequest;
  generation: number;
  foreground: boolean;
  background: boolean;
  discarded: boolean;
  waiters: Set<Waiter>;
}
interface Slot {
  worker?: TextWorkerLike;
  job?: Job;
}

/**
 * Two lazy native workers and a bounded LRU of owned RGBA snapshots.
 * Cached frames/data are shared immutable entries: callers must neither mutate
 * their pixels nor transfer their buffers. The worker transfers ownership once
 * to this pool; returned bytes never borrow a live WASM heap.
 */
export class TextPrefetchPool {
  private readonly factory: () => TextWorkerLike;
  private readonly slots: Slot[];
  private readonly maxBytes: number;
  private readonly maxEntries: number;
  private readonly cache = new Map<string, TextFrame>();
  private readonly jobs = new Map<string, Job>();
  private queue: Job[] = [];
  private generation = 0;
  private sequence = 0;
  private disposed = false;
  private pumping = false;
  private counters = { readyHits: 0, awaitedHits: 0, rendered: 0, bytes: 0, staleDrops: 0 };

  constructor(options: TextPrefetchOptions = {}) {
    const workers = options.workers ?? 2;
    this.maxBytes = options.maxBytes ?? 64 * 1024 * 1024;
    this.maxEntries = options.maxEntries ?? 64;
    if (
      !Number.isInteger(workers) ||
      workers < 1 ||
      workers > 2 ||
      !Number.isSafeInteger(this.maxBytes) ||
      this.maxBytes < 0 ||
      !Number.isSafeInteger(this.maxEntries) ||
      this.maxEntries < 0
    )
      throw new Error('Invalid native text prefetch limits');
    this.slots = Array.from({ length: workers }, () => ({}));
    this.factory =
      options.workerFactory ||
      (() => new Worker(new URL('./text-worker.ts', import.meta.url), { type: 'module' }));
  }

  request(input: TextFrameRequest, signal?: AbortSignal): Promise<TextFrame> {
    if (this.disposed || signal?.aborted) return Promise.reject(abortError());
    try {
      validateTextFrameRequest(input);
    } catch (error) {
      return Promise.reject(error);
    }
    const ready = this.cache.get(input.key);
    if (ready) {
      this.cache.delete(input.key);
      this.cache.set(input.key, ready);
      this.counters.readyHits++;
      return Promise.resolve(ready);
    }
    let job = this.jobs.get(input.key);
    if (job) {
      this.counters.awaitedHits++;
      job.foreground = true;
    } else {
      job = this.createJob(input, true, false);
      this.jobs.set(input.key, job);
      this.queue.push(job);
    }
    const pending = job;
    const promise = new Promise<TextFrame>((resolve, reject) => {
      const waiter: Waiter = { resolve, reject, signal };
      waiter.abort = () => {
        if (!pending.waiters.delete(waiter)) return;
        signal?.removeEventListener('abort', waiter.abort!);
        reject(abortError());
        if (!pending.waiters.size && !pending.background) {
          pending.discarded = true;
          if (this.jobs.get(pending.input.key) === pending) this.jobs.delete(pending.input.key);
          this.queue = this.queue.filter((queued) => queued !== pending);
        } else if (!pending.waiters.size) {
          pending.foreground = false;
        }
      };
      pending.waiters.add(waiter);
      signal?.addEventListener('abort', waiter.abort, { once: true });
      if (signal?.aborted) waiter.abort();
    });
    this.pump();
    return promise;
  }

  /** Background errors are consumed; no rejected promise is retained or exposed. */
  prefetch(inputs: TextFrameRequest[]) {
    if (this.disposed) return;
    const valid = inputs.filter((input) => {
      try {
        validateTextFrameRequest(input);
        return true;
      } catch {
        return false;
      }
    });
    const window = new Set(valid.map((input) => input.key));
    this.queue = this.queue.filter((job) => {
      if (job.foreground || job.waiters.size || window.has(job.input.key)) return true;
      job.discarded = true;
      if (this.jobs.get(job.input.key) === job) this.jobs.delete(job.input.key);
      this.counters.staleDrops++;
      return false;
    });
    for (const input of valid) {
      if (this.cache.has(input.key) || this.jobs.has(input.key)) continue;
      if (this.queue.length >= 6) break;
      const job = this.createJob(input, false, true);
      this.jobs.set(input.key, job);
      this.queue.push(job);
      this.pump();
    }
  }

  invalidate() {
    if (this.disposed) return;
    this.generation++;
    for (const job of this.jobs.values()) {
      job.discarded = true;
      this.settle(job, undefined, abortError());
    }
    this.jobs.clear();
    this.queue = [];
    // Active WASM calls finish naturally. Their obsolete generation is never
    // installed, while already cached exact immutable identities remain valid.
  }

  dispose() {
    if (this.disposed) return;
    this.invalidate();
    this.disposed = true;
    for (const slot of this.slots) {
      this.closeWorker(slot);
      slot.job = undefined;
    }
    this.cache.clear();
    this.counters.bytes = 0;
  }

  stats(): TextPrefetchStats {
    return {
      ...this.counters,
      active: this.slots.filter((slot) => slot.job).length,
      queued: this.queue.length,
      workers: this.slots.filter((slot) => slot.worker).length
    };
  }

  private createJob(input: TextFrameRequest, foreground: boolean, background: boolean): Job {
    return {
      id: ++this.sequence,
      input: { ...input, template: structuredClone(input.template) },
      generation: this.generation,
      foreground,
      background,
      discarded: false,
      waiters: new Set()
    };
  }

  private pump() {
    if (this.disposed || this.pumping) return;
    this.pumping = true;
    try {
      while (this.queue.length) {
        const slot = this.slots.find((slot) => !slot.job);
        if (!slot) break;
        const foreground = this.queue.findIndex((job) => job.foreground);
        const [job] = this.queue.splice(foreground < 0 ? 0 : foreground, 1);
        if (job.discarded) continue;
        slot.job = job;
        try {
          if (!slot.worker) this.openWorker(slot);
          slot.worker!.postMessage({ id: job.id, input: job.input });
        } catch (error) {
          this.closeWorker(slot);
          this.finish(
            slot,
            undefined,
            new TextWorkerUnavailableError(error instanceof Error ? error.message : String(error))
          );
        }
      }
    } finally {
      this.pumping = false;
    }
  }

  private openWorker(slot: Slot) {
    const worker = this.factory();
    slot.worker = worker;
    worker.onmessage = (event) => {
      if (slot.worker !== worker) return;
      const job = slot.job,
        data = event.data as TextWorkerReply;
      if (!job) return;
      if (!data || typeof data !== 'object' || data.id !== job.id) {
        this.workerFailure(slot, 'Native text worker returned an invalid response');
        return;
      }
      if ('error' in data) {
        if (!data.error || typeof data.error.message !== 'string') {
          this.workerFailure(slot, 'Native text worker returned an invalid error response');
          return;
        }
        const error = new Error(data.error.message);
        error.name = data.error.name || 'Error';
        if (data.error.stack) error.stack = data.error.stack;
        // Native input/resource failures intentionally never become an
        // unavailable-worker error eligible for a source fallback.
        this.finish(slot, undefined, error);
      } else if (!validFrame(data.frame, job.input)) {
        this.workerFailure(slot, 'Native text worker returned invalid RGBA pixels');
      } else {
        this.counters.rendered++;
        this.finish(slot, data.frame);
      }
    };
    worker.onerror = (event) => {
      if (slot.worker !== worker) return;
      event.preventDefault?.();
      this.workerFailure(slot, event.message || 'Native text worker failed');
    };
    worker.onmessageerror = () => {
      if (slot.worker !== worker) return;
      this.workerFailure(slot, 'Native text worker message could not be decoded');
    };
  }

  private workerFailure(slot: Slot, message: string) {
    this.closeWorker(slot);
    this.finish(slot, undefined, new TextWorkerUnavailableError(message));
  }

  private closeWorker(slot: Slot) {
    if (!slot.worker) return;
    slot.worker.onmessage = slot.worker.onerror = slot.worker.onmessageerror = null;
    slot.worker.terminate();
    slot.worker = undefined;
  }

  private finish(slot: Slot, frame?: TextFrame, error?: Error) {
    const job = slot.job;
    if (!job) return;
    slot.job = undefined;
    if (this.jobs.get(job.input.key) === job) this.jobs.delete(job.input.key);
    if (job.discarded || job.generation !== this.generation || this.disposed) {
      this.counters.staleDrops++;
      this.settle(job, undefined, abortError());
    } else {
      if (frame) {
        freezeMetadata(frame);
        this.store(job.input.key, frame);
      }
      this.settle(job, frame, error);
    }
    this.pump();
  }

  private store(key: string, frame: TextFrame) {
    const bytes = frame.data.byteLength;
    if (!this.maxEntries || bytes > this.maxBytes) return;
    while (
      this.cache.size &&
      (this.cache.size >= this.maxEntries || this.counters.bytes + bytes > this.maxBytes)
    ) {
      const oldest = this.cache.keys().next().value!;
      this.counters.bytes -= this.cache.get(oldest)!.data.byteLength;
      this.cache.delete(oldest);
    }
    this.cache.set(key, frame);
    this.counters.bytes += bytes;
  }

  private settle(job: Job, frame?: TextFrame, error?: Error) {
    for (const waiter of job.waiters) {
      if (waiter.abort) waiter.signal?.removeEventListener('abort', waiter.abort);
      if (frame) waiter.resolve(frame);
      else waiter.reject(error || new Error('Native text worker produced no frame'));
    }
    job.waiters.clear();
  }
}
