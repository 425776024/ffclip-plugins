import { throwIfAborted, abortError } from './runtime.mjs';
let deadline = 0,
  lastSent = -Infinity,
  channel: BroadcastChannel | undefined;
function initialize() {
  if (!channel && typeof self !== 'undefined' && typeof BroadcastChannel !== 'undefined') {
    channel = new BroadcastChannel('videocut-media-foreground-v1');
    channel.onmessage = (event) => {
      const ttl = Number(event.data?.ttl);
      if (Number.isFinite(ttl) && ttl >= 0 && ttl <= 2000)
        deadline = Math.max(deadline, performance.now() + ttl);
    };
  }
}
/** Cross-worker pressure signal contains no source identifiers or private data. */
export function setMediaForegroundActivity(active: boolean, ttlMs = 180) {
  initialize();
  const now = performance.now(),
    ttl = active ? Math.max(0, Math.min(2000, ttlMs)) : 0;
  deadline = active ? Math.max(deadline, now + ttl) : 0;
  if (!active || now - lastSent >= 70) {
    channel?.postMessage({ ttl });
    lastSent = now;
  }
}
export async function waitForMediaBackground(signal?: AbortSignal) {
  initialize();
  while (performance.now() < deadline) {
    throwIfAborted(signal);
    await new Promise<void>((resolve, reject) => {
      const abort = () => {
        clearTimeout(timer);
        reject(abortError());
      };
      const timer = setTimeout(
        () => {
          signal?.removeEventListener('abort', abort);
          resolve();
        },
        Math.min(80, Math.max(1, deadline - performance.now()))
      );
      signal?.addEventListener('abort', abort, { once: true });
    });
  }
  throwIfAborted(signal);
}
