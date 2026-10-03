import {
  VideoCutClient,
  createProject,
  addAsset,
  addText,
  editTimeline,
  type Project
} from '../packages/client/index.mjs';
const root = '/Users/jxinfa/WebstormProjects/videocut/.local/architecture-qa';
const baseline =
  '/@fs/Users/jxinfa/WebstormProjects/videocut/.local/architecture-baseline/package/dist/web/assets/worker-C3JgJ-tK.js';
const status = document.querySelector('#status')!,
  canvases = document.querySelector('#canvases')!;
const report: any = {
  phase: 'idle',
  scope: 'Pre-cache-upgrade npm package versus current; not historical Electron',
  size: [1280, 720],
  autoQuality: false,
  baselineSha256: 'a4256b70c42232f3274b8d9ed8da7afdeb233e7754995a88a55a322a488af49c',
  results: []
};
async function publish() {
  status.textContent = JSON.stringify(report, null, 2);
  await fetch('http://127.0.0.1:4332/report', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-QA-Report': 'render' },
    body: JSON.stringify(report)
  }).catch(() => {});
}
function stats(values: number[]) {
  const v = [...values].sort((a, b) => a - b);
  return {
    count: v.length,
    median: v[Math.floor(v.length / 2)],
    p95: v[Math.min(v.length - 1, Math.floor(v.length * 0.95))],
    max: v.at(-1)
  };
}
async function pixels(bytes: ArrayBuffer) {
  const frame = await createImageBitmap(new Blob([bytes], { type: 'image/png' })),
    copy = new OffscreenCanvas(frame.width, frame.height),
    ctx = copy.getContext('2d')!;
  ctx.drawImage(frame, 0, 0);
  frame.close();
  return ctx.getImageData(0, 0, copy.width, copy.height).data;
}
function diff(a: Uint8ClampedArray, b: Uint8ClampedArray) {
  if (a.length !== b.length) throw Error('Resolution changed');
  let sum = 0,
    max = 0,
    litA = 0,
    litB = 0;
  for (let i = 0; i < a.length; i++) {
    const d = Math.abs(a[i] - b[i]);
    sum += d;
    max = Math.max(max, d);
    if (i % 4 !== 3) {
      if (a[i] > 10) litA++;
      if (b[i] > 10) litB++;
    }
  }
  if (litA < a.length * 0.05 || litB < b.length * 0.05)
    throw Error('Empty render cannot count as pixel parity');
  return { meanByteError: sum / a.length, maxByteError: max, litA, litB };
}
class Cursor {
  worker: Worker;
  canvas = document.createElement('canvas');
  pending?: {
    id?: number;
    type: string;
    resolve: (v: any) => void;
    reject: (e: Error) => void;
    timer: ReturnType<typeof setTimeout>;
  };
  id = 0;
  disposed = false;
  constructor(readonly name: string) {
    this.canvas.width = 1280;
    this.canvas.height = 720;
    canvases.append(this.canvas);
    const workerUrl = new URL('./performance-capture.worker.ts', import.meta.url);
    workerUrl.searchParams.set('backend', name);
    this.worker = new Worker(workerUrl, { type: 'module' });
    this.worker.onmessage = ({ data }) => {
      const pending = this.pending;
      if (!pending) return;
      if (data.id !== pending.id || (data.type !== pending.type && data.type !== 'error')) return;
      clearTimeout(pending.timer);
      this.pending = undefined;
      if (data.type === 'error') pending.reject(Error(data.error));
      else pending.resolve(data.report || data);
    };
    this.worker.onerror = (event) => {
      const p = this.pending;
      if (p) {
        clearTimeout(p.timer);
        this.pending = undefined;
        p.reject(Error(event.message));
      }
    };
  }
  send(value: any, transfer: Transferable[] = []) {
    return new Promise<any>((resolve, reject) => {
      this.pending = {
        id: value.id,
        type:
          value.type === 'init'
            ? 'ready'
            : value.type === 'capture'
              ? 'captured'
              : value.type === 'dispose'
                ? 'disposed'
                : 'frame',
        resolve,
        reject,
        timer: setTimeout(() => {
          this.pending = undefined;
          reject(Error(this.name + ' worker timeout'));
        }, 30000)
      };
      this.worker.postMessage(value, transfer);
    });
  }
  async init(urls: Record<string, string>) {
    // A detached surface has no HTML placeholder presentation lifecycle. Both
    // implementations render into the same kind of owned worker surface.
    const canvas = new OffscreenCanvas(1280, 720);
    await this.send({ type: 'init', canvas, urls }, [canvas]);
  }
  async frame(project: Project, urls: Record<string, string>) {
    const start = performance.now();
    const rendered = await this.send({
      type: 'render',
      id: ++this.id,
      generation: 1,
      project,
      time: 0,
      width: 1280,
      height: 720,
      urls
    });
    if (rendered.width !== 1280 || rendered.height !== 720) throw Error('Fixed quality violated');
    return { roundTripMs: performance.now() - start, report: rendered };
  }
  async capture() {
    const captured = await this.send({ type: 'capture', id: ++this.id });
    const bitmap = await createImageBitmap(new Blob([captured.bytes], { type: 'image/png' }));
    this.canvas.getContext('2d')!.drawImage(bitmap, 0, 0);
    bitmap.close();
    return captured;
  }
  async dispose() {
    if (this.disposed) return;
    this.disposed = true;
    try {
      await this.send({ type: 'dispose', id: ++this.id });
    } finally {
      this.worker.terminate();
      this.canvas.remove();
    }
  }
}
async function run() {
  report.capture = 'detached-worker-offscreen-canvas';
  report.phase = 'importing';
  report.started = new Date().toISOString();
  report.userAgent = navigator.userAgent;
  report.results = [];
  delete report.error;
  await publish();
  const client = new VideoCutClient(location.origin),
    cursors: Cursor[] = [];
  let session = '';
  try {
    await client.connect();
    const asset = await client.importMedia(root + '/still.png');
    let project = createProject('Fixed-quality A/B');
    project.canvas = { width: 1280, height: 720 };
    const item = addAsset(project, asset);
    addText(project, { content: 'Shared compositor 共享合成器', fontSize: 64 });
    project = editTimeline(project, [
      { action: 'set_transform', itemId: item.id, scaleX: 0.65, scaleY: 0.65 },
      { action: 'add_effect', itemId: item.id, templateId: 'blur', parameters: { radius: 12 } },
      {
        action: 'add_effect',
        itemId: item.id,
        templateId: 'glow',
        parameters: { radius: 16, strength: 0.3 }
      },
      {
        action: 'add_effect',
        itemId: item.id,
        templateId: 'lut',
        parameters: { preset: 'warm', amount: 0.7 }
      }
    ]).project;
    session = (await client.createSession(project)).id;
    const urls = Object.fromEntries(
      project.assets.map((a) => [
        a.id,
        client.mediaUrl(session, a.id) + '&source=' + encodeURIComponent(a.sourceIdentity || a.id)
      ])
    );
    const references = new Map<string, Uint8ClampedArray>();
    for (let round = 0; round < 4; round++) {
      for (const name of round % 2 ? ['current', 'baseline'] : ['baseline', 'current']) {
        report.phase = `${name}-round-${round}`;
        await publish();
        const cursor = new Cursor(name);
        cursors.push(cursor);
        await cursor.init(urls);
        const cold = await cursor.frame(project, urls),
          repeat: number[] = [],
          move: number[] = [],
          wall: number[] = [];
        let last = cold;
        for (let i = 0; i < 40; i++) {
          last = await cursor.frame(project, urls);
          repeat.push(last.report.frameMs);
          wall.push(last.roundTripMs);
        }
        for (let i = 0; i < 40; i++) {
          const next = editTimeline(project, [
            { action: 'set_transform', itemId: item.id, positionX: i, positionY: i % 7 }
          ]).project;
          last = await cursor.frame(next, urls);
          move.push(last.report.frameMs);
        }
        const capture = await cursor.capture();
        if (capture.width !== 1280 || capture.height !== 720)
          throw Error('Captured resolution changed');
        const image = await pixels(capture.bytes);
        references.set(name, image);
        report.results.push({
          name,
          round,
          coldMs: cold.report.frameMs,
          repeat: stats(repeat),
          move: stats(move),
          repeatRoundTrip: stats(wall),
          final: last.report
        });
        await cursor.dispose();
        await publish();
      }
      report.pixelComparison = diff(references.get('baseline')!, references.get('current')!);
      if (report.pixelComparison.meanByteError > 1)
        throw Error('A/B pixels changed; timing alone cannot establish same-quality improvement');
    }
    report.phase = 'complete';
    report.completed = new Date().toISOString();
    await publish();
  } catch (error) {
    report.phase = 'failed';
    report.error = String(error);
    await publish();
  } finally {
    for (const c of cursors) await c.dispose();
    if (session) await client.closeSession(session);
  }
}
document.querySelector('#run')!.addEventListener('click', () => void run());
