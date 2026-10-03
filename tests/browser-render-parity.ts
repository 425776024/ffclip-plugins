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
async function pixels(canvas: HTMLCanvasElement) {
  const blob = await new Promise<Blob>((resolve, reject) =>
    canvas.toBlob((b) => (b ? resolve(b) : reject(Error('Canvas capture failed'))))
  );
  const frame = await createImageBitmap(blob),
    copy = new OffscreenCanvas(canvas.width, canvas.height),
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
    resolve: (v: any) => void;
    reject: (e: Error) => void;
    timer: ReturnType<typeof setTimeout>;
  };
  id = 0;
  constructor(readonly name: string) {
    this.canvas.width = 1280;
    this.canvas.height = 720;
    canvases.append(this.canvas);
    this.worker =
      name === 'baseline'
        ? new Worker(baseline, { type: 'module' })
        : new Worker(new URL('../packages/render/worker.ts', import.meta.url), { type: 'module' });
    this.worker.onmessage = ({ data }) => {
      const pending = this.pending;
      if (!pending) return;
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
    const canvas = this.canvas.transferControlToOffscreen();
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
  dispose() {
    this.worker.postMessage({ type: 'dispose' });
    this.worker.terminate();
  }
}
async function run() {
  report.kind = 'render-parity-diagnosis';
  report.phase = 'importing';
  report.started = new Date().toISOString();
  report.results = [];
  delete report.error;
  await publish();
  const client = new VideoCutClient(location.origin);
  let session = '';
  try {
    await client.connect();
    const imported = await client.importMedia(root + '/still.png');
    const asset = imported.asset || imported;
    const base = createProject('Parity isolation');
    base.canvas = { width: 1280, height: 720 };
    const item = addAsset(base, asset);
    addText(base, { content: 'Shared compositor 共享合成器', fontSize: 64 });
    const authored = editTimeline(base, [
      { action: 'set_transform', itemId: item.id, scaleX: 0.65, scaleY: 0.65 }
    ]).project;
    session = (await client.createSession(authored)).id;
    const urls = Object.fromEntries(
      authored.assets.map((a) => [
        a.id,
        client.mediaUrl(session, a.id) + '&source=' + encodeURIComponent(a.sourceIdentity || a.id)
      ])
    );
    for (const ids of [
      [],
      ['blur'],
      ['glow'],
      ['lut'],
      ['blur', 'glow'],
      ['blur', 'glow', 'lut']
    ]) {
      const name = ids.join('-') || 'none';
      const effects = ids.map((id) => ({
        action: 'add_effect',
        itemId: item.id,
        templateId: id,
        parameters:
          id === 'blur'
            ? { radius: 12 }
            : id === 'glow'
              ? { radius: 16, strength: 0.3 }
              : { preset: 'warm', amount: 0.7 }
      }));
      const project = effects.length ? editTimeline(authored, effects as any).project : authored;
      const cursors = [new Cursor('baseline'), new Cursor('current')];
      try {
        for (const cursor of cursors) await cursor.init(urls);
        for (const position of [0, 39]) {
          report.phase = name + '-' + position;
          await publish();
          const next = editTimeline(project, [
            {
              action: 'set_transform',
              itemId: item.id,
              positionX: position,
              positionY: position ? 4 : 0
            }
          ]).project;
          const frames: Uint8ClampedArray[] = [];
          const draws: any[] = [];
          const presentation: any[] = [];
          for (const cursor of cursors) {
            draws.push(await cursor.frame(next, urls));
            const immediate = await pixels(cursor.canvas);
            // Worker GPU completion and the HTML placeholder's presentation are separate.
            await new Promise<void>((resolve) =>
              requestAnimationFrame(() => requestAnimationFrame(() => resolve()))
            );
            const settled = await pixels(cursor.canvas);
            presentation.push({ name: cursor.name, ...diff(immediate, settled) });
            frames.push(settled);
            const blob = await new Promise<Blob | null>((resolve) => cursor.canvas.toBlob(resolve));
            if (blob)
              await fetch(
                'http://127.0.0.1:4332/image?name=' + name + '-' + position + '-' + cursor.name,
                { method: 'POST', headers: { 'X-QA-Report': 'render' }, body: blob }
              );
          }
          const difference = diff(frames[0], frames[1]);
          let changed = 0,
            minX = 1280,
            minY = 720,
            maxX = -1,
            maxY = -1;
          for (let i = 0; i < frames[0].length; i += 4)
            if (
              Math.max(...[0, 1, 2].map((c) => Math.abs(frames[0][i + c] - frames[1][i + c]))) > 16
            ) {
              changed++;
              const x = (i / 4) % 1280,
                y = Math.floor(i / 4 / 1280);
              minX = Math.min(x, minX);
              minY = Math.min(y, minY);
              maxX = Math.max(x, maxX);
              maxY = Math.max(y, maxY);
            }
          report.results.push({
            name,
            position,
            ...difference,
            changed,
            bounds: [minX, minY, maxX, maxY],
            draws: draws.map((d) => d.report),
            presentation,
            passed: difference.meanByteError <= 1
          });
          await publish();
        }
      } finally {
        for (const cursor of cursors) cursor.dispose();
      }
    }
    report.phase = 'complete';
    report.completed = new Date().toISOString();
    await publish();
  } catch (error) {
    report.phase = 'failed';
    report.error = String(error);
    await publish();
  } finally {
    if (session) await client.closeSession(session);
  }
}
document.querySelector('#run')!.addEventListener('click', () => void run());
