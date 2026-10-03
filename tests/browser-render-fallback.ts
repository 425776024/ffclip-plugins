import { VideoCutClient } from '../packages/client/index.mjs';
import {
  createProject,
  addAsset,
  addText,
  ticks,
  type Project
} from '../packages/core/project.mjs';
import { SceneRenderer } from '../packages/render/renderer';
import { sharedMediaEngine } from '../packages/media/browser';
import { exportProject } from '../packages/render/export';
import { recipes } from '../packages/text-wasm/src/recipes.mjs';

const query = new URLSearchParams(location.search),
  fixtures =
    query.get('fixtures') || '/Users/jxinfa/WebstormProjects/videocut/.local/architecture-qa',
  reportUrl = query.get('report') || 'http://127.0.0.1:4332',
  preview = document.querySelector<HTMLCanvasElement>('#preview')!,
  decoded = document.querySelector<HTMLCanvasElement>('#decoded')!,
  button = document.querySelector<HTMLButtonElement>('#run')!,
  client = new VideoCutClient(location.origin),
  report: any = { kind: 'render-canvas2d', phase: 'idle', results: [], logs: [] };
async function publish() {
  document.querySelector('#status')!.textContent = JSON.stringify(report, null, 2);
  await fetch(`${reportUrl}/report`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-QA-Report': 'render' },
    body: JSON.stringify(report)
  }).catch(() => {});
}
async function record(name: string, details: any, passed: boolean) {
  report.results.push({ name, ...details, passed });
  await publish();
  if (!passed) throw new Error(`${name} failed`);
}
function project(name: string) {
  const value = createProject(name);
  value.canvas = { width: 640, height: 360 };
  return value;
}
function clip(p: Project, asset: any, start = 0, length = 0.6, source = 0, trackId?: string) {
  const item = addAsset(p, asset, { start: ticks(start), trackId, validate: false });
  item.placement.end = ticks(start + length);
  item.clip.source = { begin: ticks(source), end: ticks(source + length) };
  if (item.clip.type === 'video' || item.clip.type === 'audio') item.clip.audio.muted = true;
  return item;
}
async function rejected(name: string, p: Project, time: number, urls: Record<string, string>) {
  const scene = new SceneRenderer(new OffscreenCanvas(640, 360), (id) => urls[id]);
  let error = '';
  try {
    await scene.render(p, ticks(time), 640, 360);
  } catch (value) {
    error = String(value);
  } finally {
    scene.dispose();
  }
  await record(name, { error }, /WebGPU/.test(error));
}
async function run() {
  button.disabled = true;
  report.started = new Date().toISOString();
  report.phase = 'importing';
  report.results = [];
  report.logs = [];
  const previous = Object.getOwnPropertyDescriptor(navigator, 'gpu');
  let scene: SceneRenderer | undefined, outputUrl: string | undefined;
  try {
    // Fresh realm: sharedGpu has not been initialized. This exercises its actual unavailable branch.
    Object.defineProperty(navigator, 'gpu', { value: undefined, configurable: true });
    await client.connect();
    const imported = await Promise.all(
      ['timecode.mp4', 'still.png'].map((name) => client.importMedia(`${fixtures}/${name}`))
    );
    const [video, still] = imported.map((value) => value.asset || value),
      p = project('Canvas2D fallback');
    clip(p, video, 0, 0.6, 0.3);
    const image = clip(p, still);
    Object.assign(image.clip.visual, {
      positionX: 100,
      positionY: -30,
      rotationDegrees: 12,
      scaleX: 0.55,
      scaleY: 0.55,
      opacity: 0.7,
      blendMode: 'screen',
      crop: { left: 0.1, top: 0, right: 0, bottom: 0.1 }
    });
    const text = addText(p, {
      content: '基础 Canvas2D',
      fontSize: 40,
      color: '#ffffff',
      length: ticks(0.6)
    });
    text.clip.visual.positionY = 100;
    const session = await client.createSession(p),
      urls = Object.fromEntries(
        p.assets.map((asset) => [asset.id, client.mediaUrl(session.id, asset.id)])
      );
    scene = new SceneRenderer(preview, (id) => urls[id]);
    report.phase = 'canvas2d-preview';
    await publish();
    const rendered = await scene.render(p, ticks(0.2), 640, 360),
      reference = await scene.readPixels();
    await record(
      'canvas2d-media-text-transform-blend',
      {
        backend: rendered.backend,
        bounds: rendered.bounds.length,
        width: rendered.width,
        height: rendered.height
      },
      rendered.backend === 'canvas2d' && rendered.bounds.length === 3
    );
    const chunks: { position: number; data: Uint8Array }[] = [];
    let extent = 0,
      configuration: any;
    report.phase = 'canvas2d-browser-export';
    await publish();
    const receipt = await exportProject(
      p,
      'webm',
      urls,
      {
        async configure(value) {
          configuration = value;
        },
        async write(position, data) {
          chunks.push({ position, data: data.slice() });
          extent = Math.max(extent, position + data.length);
        },
        async frame() {
          throw new Error('Unexpected local encoding path');
        },
        async audio() {
          throw new Error('Unexpected audio track');
        },
        progress(value) {
          report.progress = value;
        }
      },
      new AbortController().signal,
      false,
      { preferredEncoding: 'browser' }
    );
    const bytes = new Uint8Array(extent);
    for (const chunk of chunks) bytes.set(chunk.data, chunk.position);
    outputUrl = URL.createObjectURL(new Blob([bytes], { type: 'video/webm' }));
    const frame = await sharedMediaEngine.videoFrame(outputUrl, 0.2);
    const context = decoded.getContext('2d')!;
    context.drawImage(frame.frame, 0, 0, 640, 360);
    const actual = context.getImageData(0, 0, 640, 360).data,
      timestamp = frame.timestamp;
    frame.close();
    let total = 0,
      nonBlack = 0;
    const histogram = new Uint32Array(256);
    for (let i = 0; i < reference.length; i += 4) {
      if (reference[i] + reference[i + 1] + reference[i + 2] > 12) nonBlack++;
      for (let c = 0; c < 3; c++) {
        const delta = Math.abs(reference[i + c] - actual[i + c]);
        total += delta;
        histogram[delta]++;
      }
    }
    const n = (reference.length / 4) * 3;
    let count = 0,
      p99 = 0;
    for (let i = 0; i < 256; i++) {
      count += histogram[i];
      if (count >= n * 0.99) {
        p99 = i;
        break;
      }
    }
    const mae = total / n;
    await record(
      'canvas2d-browser-export-decode',
      { configuration, receipt, bytes: extent, timestamp, mae, p99, nonBlack },
      configuration.encoding === 'browser' &&
        !configuration.hasAudio &&
        extent > 1000 &&
        mae < 8 &&
        p99 < 100 &&
        nonBlack > 100 &&
        Math.abs(timestamp - 0.2) < 0.034
    );
    for (const [name, canvas] of [
      ['canvas2d-preview', preview],
      ['canvas2d-export', decoded]
    ] as const) {
      const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve));
      if (blob)
        await fetch(`${reportUrl}/image?name=${name}`, {
          method: 'POST',
          headers: { 'X-QA-Report': 'render' },
          body: blob
        });
    }
    const fx = structuredClone(p);
    fx.timeline.tracks
      .flatMap((track) => track.items)
      .find((item) => item.id === image.id)!.clip.effects = [
      { id: 'unsupported-blur', templateId: 'blur', enabled: true, parameters: { radius: 8 } }
    ];
    await rejected('canvas2d-reject-effect', fx, 0.2, urls);
    const transition = project('unsupported-transition'),
      from = clip(transition, video, 0, 0.4, 0.4),
      track = transition.timeline.tracks.find((value) =>
        value.items.some((item) => item.id === from.id)
      )!,
      to = clip(transition, video, 0.4, 0.4, 2, track.id);
    transition.timeline.transitions = [
      {
        id: 'unsupported-dissolve',
        fromItemId: from.id,
        toItemId: to.id,
        templateId: 'dissolve',
        duration: ticks(0.4),
        parameters: {}
      }
    ];
    await rejected('canvas2d-reject-transition', transition, 0.4, urls);
    const template = project('unsupported-template'),
      recipe = recipes.find((value) => value.external)!;
    addText(template, {
      template: { id: recipe.id, version: 1 },
      content: recipe.text,
      length: ticks(0.6)
    });
    await rejected('canvas2d-reject-complex-template', template, 0.2, urls);
    report.phase = 'complete';
    report.completed = new Date().toISOString();
  } catch (error) {
    report.phase = 'failed';
    report.logs.push(error instanceof Error ? error.stack : String(error));
  } finally {
    scene?.dispose();
    if (outputUrl) {
      sharedMediaEngine.release(outputUrl);
      URL.revokeObjectURL(outputUrl);
    }
    if (previous) Object.defineProperty(navigator, 'gpu', previous);
    else delete (navigator as any).gpu;
    button.disabled = false;
    await publish();
  }
}
button.addEventListener('click', () => void run());
void publish();
