import type { HtmlContent } from '../core/types';

export function htmlSourceTime(content: Pick<HtmlContent, 'duration'>, sourceTime: number) {
  if (!Number.isFinite(sourceTime)) throw new RangeError('HTML 动画时间无效');
  return Math.max(0, Math.min(Math.round(sourceTime), content.duration - 1));
}

const check = (signal?: AbortSignal) => {
  if (signal?.aborted) throw new DOMException('HTML 动画帧已过期', 'AbortError');
};
type HtmlFrame = Awaited<ReturnType<HtmlFrameClient['readFrame']>>;
export interface HtmlFrameRequest {
  itemId: string;
  html: HtmlContent;
  sourceTime: number;
}

/** Chromium rasterizes DOM/GSAP; the scene compositor owns the returned transparent bitmap. */
export class HtmlFrameClient {
  private token?: Promise<string>;
  private readonly lifetime = new AbortController();
  private readonly requests = new Map<string, { itemId: string; controller: AbortController }>();
  private readonly ahead = new Map<
    string,
    { itemId: string; bytes: number; controller: AbortController; promise: Promise<HtmlFrame> }
  >();
  private aheadBytes = 0;
  private disposed = false;

  constructor(private readonly endpoint = '/api/html-frames') {}

  private url() {
    return new URL(this.endpoint, globalThis.location?.href);
  }

  private credentials() {
    if (!this.token) {
      const request = (async () => {
        const response = await fetch(new URL('./bootstrap', this.url()), {
          cache: 'no-store',
          credentials: 'same-origin',
          signal: this.lifetime.signal
        });
        if (!response.ok) throw new Error('无法连接 HTML 动画渲染服务');
        const data = await response.json();
        if (typeof data.token !== 'string' || !data.token)
          throw new Error('HTML 动画渲染服务未提供会话凭证');
        return data.token;
      })();
      this.token = request;
      request.catch(() => {
        if (this.token === request) this.token = undefined;
      });
    }
    return this.token;
  }

  retain(activeIds: ReadonlySet<string>) {
    for (const [key, request] of this.requests)
      if (!activeIds.has(request.itemId)) {
        request.controller.abort();
        this.requests.delete(key);
      }
    for (const [key, entry] of this.ahead) if (!activeIds.has(entry.itemId)) this.dropAhead(key);
  }

  async frame(itemId: string, html: HtmlContent, sourceTime: number, signal?: AbortSignal) {
    check(signal);
    const key = this.frameKey(itemId, html, sourceTime),
      entry = this.ahead.get(key);
    if (!entry) return this.readFrame(itemId, html, sourceTime, signal);
    this.ahead.delete(key);
    this.aheadBytes -= entry.bytes;
    const frame = await entry.promise;
    if (this.disposed || signal?.aborted) {
      frame.close();
      check(signal);
      throw new Error('HTML 动画渲染器已关闭');
    }
    return frame;
  }

  private frameKey(itemId: string, html: HtmlContent, sourceTime: number) {
    return JSON.stringify([itemId, html, htmlSourceTime(html, sourceTime)]);
  }

  private dropAhead(key: string) {
    const entry = this.ahead.get(key);
    if (!entry) return;
    this.ahead.delete(key);
    this.aheadBytes -= entry.bytes;
    entry.controller.abort();
    void entry.promise.then(
      (frame) => frame.close(),
      () => {}
    );
  }

  /** At most two exact successor bitmaps, reserving their full RGBA extent up front. */
  prefetch(inputs: HtmlFrameRequest[], signal?: AbortSignal) {
    if (this.disposed || signal?.aborted) return;
    const keys = new Set(
      inputs.map((input) => this.frameKey(input.itemId, input.html, input.sourceTime))
    );
    for (const key of this.ahead.keys()) if (!keys.has(key)) this.dropAhead(key);
    for (const input of inputs) {
      const key = this.frameKey(input.itemId, input.html, input.sourceTime);
      if (this.ahead.has(key)) continue;
      const bytes = input.html.width * input.html.height * 4;
      if (this.ahead.size >= 2 || this.aheadBytes + bytes > 64 * 1024 * 1024) break;
      const controller = new AbortController(),
        cancel = () => controller.abort();
      signal?.addEventListener('abort', cancel, { once: true });
      // Predictive requests have independent ownership; they cannot cancel the
      // current item's foreground request. Chromium still seeks each DOM in order.
      const promise = this.readFrame(
        input.itemId,
        input.html,
        input.sourceTime,
        controller.signal,
        key
      ).finally(() => signal?.removeEventListener('abort', cancel));
      promise.catch(() => {}); // A future failure surfaces only when that frame is consumed.
      this.ahead.set(key, { itemId: input.itemId, bytes, controller, promise });
      this.aheadBytes += bytes;
    }
  }

  private async readFrame(
    itemId: string,
    html: HtmlContent,
    sourceTime: number,
    signal?: AbortSignal,
    requestId = itemId
  ) {
    if (this.disposed) throw new Error('HTML 动画渲染器已关闭');
    check(signal);
    const tickTime = htmlSourceTime(html, sourceTime),
      controller = new AbortController(),
      cancel = () => controller.abort();
    this.requests.get(requestId)?.controller.abort();
    this.requests.set(requestId, { itemId, controller });
    signal?.addEventListener('abort', cancel, { once: true });
    this.lifetime.signal.addEventListener('abort', cancel, { once: true });
    let bitmap: ImageBitmap | undefined;
    try {
      let response: Response | undefined;
      for (let attempt = 0; attempt < 2; attempt++) {
        const token = await this.credentials();
        check(controller.signal);
        response = await fetch(this.url(), {
          method: 'POST',
          headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${token}` },
          credentials: 'same-origin',
          cache: 'no-store',
          body: JSON.stringify({ html, time: tickTime }),
          signal: controller.signal
        });
        if (response.status !== 401 || attempt) break;
        this.token = undefined;
      }
      if (!response!.ok) {
        const data = await response!.json().catch(() => ({}));
        throw new Error(data.error || `HTML 动画渲染失败 (${response!.status})`);
      }
      const blob = await response!.blob();
      const state = response!.headers.get('X-HTML-State');
      check(controller.signal);
      bitmap = await createImageBitmap(blob);
      check(controller.signal);
      if (bitmap.width !== html.width || bitmap.height !== html.height)
        throw new Error('HTML 动画渲染尺寸与模板不一致');
      const owned = bitmap;
      bitmap = undefined;
      let closed = false;
      return {
        frame: owned,
        width: owned.width,
        height: owned.height,
        timestamp: tickTime / 120000,
        tickTime,
        transparent: html.transparent,
        identity: JSON.stringify(['html-clip', html, state && /^[a-f0-9]{64}$/.test(state) ? state : tickTime]),
        close: () => {
          if (closed) return;
          closed = true;
          owned.close();
        }
      };
    } finally {
      bitmap?.close();
      signal?.removeEventListener('abort', cancel);
      this.lifetime.signal.removeEventListener('abort', cancel);
      if (this.requests.get(requestId)?.controller === controller) this.requests.delete(requestId);
    }
  }

  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    this.lifetime.abort();
    for (const key of this.ahead.keys()) this.dropAhead(key);
    for (const { controller } of this.requests.values()) controller.abort();
    this.requests.clear();
    this.token = undefined;
  }
}
