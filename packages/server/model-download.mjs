// Fixed endpoints only; both serve the same revision-pinned, SHA256-verified manifest.
export const MODEL_SOURCES = Object.freeze(['https://huggingface.co', 'https://hf-mirror.com']);

export function modelDownloadError(message) {
  return Object.assign(new Error(message), { statusCode: 503, code: 'MODEL_DOWNLOAD_FAILED' });
}

async function openDownload({ source, path, size, fetchImpl, signal, timeoutMs }) {
  const controller = new AbortController();
  let reader,
    closed = false;
  const close = () => {
    if (closed) return;
    closed = true;
    signal.removeEventListener('abort', abort);
    controller.abort();
    // A broken stream must not hold up failover or cancellation.
    if (reader) void reader.cancel().catch(() => {});
  };
  const abort = () => {
    controller.abort(signal.reason);
    close();
  };
  signal.addEventListener('abort', abort, { once: true });
  if (signal.aborted) abort();
  const wait = async (operation) => {
    controller.signal.throwIfAborted();
    let rejectAbort;
    const aborted = new Promise((_, reject) => {
      rejectAbort = () => reject(controller.signal.reason);
      controller.signal.addEventListener('abort', rejectAbort, { once: true });
    });
    const timer = setTimeout(
      () => controller.abort(modelDownloadError(`模型下载超时：${path}`)),
      timeoutMs
    );
    try {
      return await Promise.race([operation(), aborted]);
    } catch (error) {
      if (signal.aborted) throw signal.reason;
      if (error.code === 'MODEL_DOWNLOAD_FAILED') throw error;
      throw modelDownloadError(`模型下载失败：${path} (${error.message || error})`);
    } finally {
      clearTimeout(timer);
      controller.signal.removeEventListener('abort', rejectAbort);
    }
  };
  try {
    const response = await wait(() => {
      const request = Promise.resolve(
        fetchImpl(`${source}/${path}`, { signal: controller.signal })
      );
      // Also discard late responses from transports that ignore AbortSignal.
      void request
        .then((response) => {
          if (controller.signal.aborted) void response.body?.cancel().catch(() => {});
        })
        .catch(() => {});
      return request;
    });
    if (!response.ok || !response.body) {
      void response.body?.cancel().catch(() => {});
      throw modelDownloadError(`模型下载失败 (${response.status})：${path}`);
    }
    if (
      response.headers.has('content-length') &&
      Number(response.headers.get('content-length')) !== size
    ) {
      void response.body.cancel().catch(() => {});
      throw modelDownloadError(`模型大小不匹配：${path}`);
    }
    reader = response.body.getReader();
    const read = () => wait(() => reader.read());
    // Follow redirects and receive actual bytes before declaring an endpoint usable.
    let first;
    do {
      first = await read();
    } while (!first.done && !first.value.length);
    if (first.done && size) throw modelDownloadError(`模型校验失败（空响应）：${path}`);
    return { source, first, read, close };
  } catch (error) {
    close();
    throw error;
  }
}

/** Hedge a slow source with the other endpoint; retain only the first usable stream. */
export function selectModelDownload({
  sources,
  path,
  size,
  fetchImpl,
  signal,
  hedgeDelayMs = 500,
  timeoutMs = 15000
}) {
  signal.throwIfAborted();
  return new Promise((resolve, reject) => {
    const attempts = [],
      errors = [];
    let settled = false,
      timer;
    const finish = (winner) => {
      settled = true;
      clearTimeout(timer);
      signal.removeEventListener('abort', abort);
      for (const attempt of attempts) if (attempt !== winner) attempt.controller.abort();
    };
    const abort = () => {
      finish();
      reject(signal.reason);
    };
    const launch = () => {
      clearTimeout(timer);
      if (settled || attempts.length === sources.length) return;
      const attempt = { source: sources[attempts.length], controller: new AbortController() };
      attempts.push(attempt);
      void openDownload({
        source: attempt.source,
        path,
        size,
        fetchImpl,
        timeoutMs,
        signal: AbortSignal.any([signal, attempt.controller.signal])
      })
        .then((download) => {
          if (settled) return download.close();
          finish(attempt);
          resolve(download);
        })
        .catch((error) => {
          if (settled) return;
          errors.push(`${attempt.source}: ${error.message || error}`);
          if (errors.length === sources.length) {
            finish();
            reject(modelDownloadError(`所有模型下载源均失败：${errors.join('；')}`));
          } else {
            // A definite error starts the alternative immediately, without the hedge delay.
            launch();
          }
        });
      if (attempts.length < sources.length) timer = setTimeout(launch, hedgeDelayMs);
    };
    signal.addEventListener('abort', abort, { once: true });
    launch();
  });
}
