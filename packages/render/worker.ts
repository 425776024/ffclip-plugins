import { SceneRenderer } from './renderer';
import { exportProject } from './export';
import { PreviewDocument, presentationProject } from './projection';

const scope = self as unknown as {
  postMessage(value: any, transfer?: Transferable[]): void;
  onmessage: ((event: MessageEvent) => void) | null;
};
let renderer: SceneRenderer | undefined, abort: AbortController | undefined;
let previewAbort: AbortController | undefined;
const document = new PreviewDocument();
let mediaUrls: Record<string, string> = {};
const writes = new Map<number, { resolve: () => void; reject: (error: Error) => void }>();
let writeId = 0;
const request = (type: string, values: Record<string, unknown>, bytes?: Uint8Array) =>
  new Promise<void>((resolve, reject) => {
    const id = ++writeId;
    writes.set(id, { resolve, reject });
    if (bytes) {
      const copy = Uint8Array.from(bytes);
      scope.postMessage({ type, id, ...values, data: copy }, [copy.buffer]);
    } else scope.postMessage({ type, id, ...values });
  });
scope.onmessage = async (event) => {
  const data = event.data;
  if (data.type === 'invalidate') {
    previewAbort?.abort();
    renderer?.invalidatePrefetch();
    return;
  }
  if (data.type === 'ack') {
    const pending = writes.get(data.id);
    writes.delete(data.id);
    data.error ? pending?.reject(new Error(data.error)) : pending?.resolve();
    return;
  }
  if (data.type === 'cancel') {
    abort?.abort();
    for (const p of writes.values()) p.reject(new Error('用户取消导出'));
    writes.clear();
    return;
  }
  if (data.type === 'dispose') {
    abort?.abort();
    previewAbort?.abort();
    renderer?.dispose();
    renderer = undefined;
    return;
  }
  try {
    if (data.type === 'init') {
      mediaUrls = data.urls || {};
      renderer = new SceneRenderer(data.canvas, (id) => mediaUrls[id], {
        textWorkers: true,
        ...data.options
      });
      await renderer.ready;
      scope.postMessage({ type: 'ready' });
    } else if (data.type === 'render') {
      if (!renderer) throw new Error('渲染器未初始化');
      if (!document.receive(data)) {
        scope.postMessage({
          type: 'resync',
          id: data.id,
          generation: data.generation,
          documentRevision: document.revision
        });
        return;
      }
      // URL map is refreshed after each media import without recreating GPU state.
      if (data.urls) mediaUrls = data.urls;
      previewAbort = new AbortController();
      const signal = previewAbort.signal;
      const documentRevision = document.revision;
      const project = presentationProject(document.project!, data.presentation);
      const report = await renderer.render(project, data.time, data.width, data.height, signal, {
        prefetch: data.prefetch === true
      });
      if (signal.aborted) throw new DOMException('帧已过期', 'AbortError');
      scope.postMessage({
        type: 'frame',
        id: data.id,
        generation: data.generation,
        documentRevision,
        report
      });
      if (data.prefetch) renderer.prepareAhead(project, data.time, data.width, data.height);
    } else if (data.type === 'export') {
      abort = new AbortController();
      const result = await exportProject(
        data.project,
        data.format,
        data.urls,
        {
          progress: (value) => scope.postMessage({ type: 'progress', value }),
          configure: (value) => request('configure', { value }),
          write: (position, bytes) => request('chunk', { position }, bytes),
          audio: (sampleOffset, sampleRate, channels, bytes) =>
            request('audio', { sampleOffset, sampleRate, channels }, bytes),
          frame: (index, bytes) => request('frame', { index }, bytes)
        },
        abort.signal,
        !!data.localEncoder,
        {
          preferredEncoding: data.preferredEncoding,
          htmlPrefetch: data.htmlPrefetch,
          textWorkers: data.textWorkers
        }
      );
      scope.postMessage({ type: 'complete', result });
    }
  } catch (error) {
    scope.postMessage({
      type: error instanceof Error && error.name === 'AbortError' ? 'cancelled' : 'error',
      id: data.id,
      generation: data.generation,
      error: error instanceof Error ? error.message : String(error),
      ...(typeof error === 'object' && error && 'code' in error && error.code === 'RENDER_BUDGET'
        ? {
            code: error.code,
            requiredBytes: (error as any).requiredBytes,
            budgetBytes: (error as any).budgetBytes
          }
        : {})
    });
  }
};
