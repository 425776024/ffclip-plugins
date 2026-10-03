import { VideoCutClient } from '../packages/client/index.mjs';
import { ticks } from '../packages/core/project.mjs';
import type { Project } from '../packages/core/types';
import { SceneRenderer } from '../packages/render/renderer';
import { createTemplatePlayer } from '../packages/render/text';
import { TextPrefetchPool } from '../packages/render/text-prefetch';
import { sha256, type TextFrame } from '../packages/text-wasm/dist/index.mjs';

const W = 1920,
  H = 1080;
const query = new URLSearchParams(location.search),
  client = new VideoCutClient(location.origin);
const status = document.querySelector<HTMLElement>('#status')!,
  frames = document.querySelector<HTMLElement>('#frames')!;
const runButton = document.querySelector<HTMLButtonElement>('#run')!,
  compareButton = document.querySelector<HTMLButtonElement>('#compare')!;
const buttons = [runButton, compareButton];
const projectUrl =
  query.get('project') ||
  '/@fs/Users/jxinfa/WebstormProjects/videocut/.local/html-evidence/performance-fixed-quality-session.json';
const textWorkers = query.get('textWorkers') !== 'off',
  reportUrl = query.get('report');
const report: any = {
  kind: 'full-hd-original-scene-profile',
  phase: 'idle',
  complete: false,
  results: [],
  resolution: [W, H],
  textWorkers,
  projectUrl,
  matrixFramesPerCase: 6,
  timingScope:
    'Actual SceneRenderer source preparation and WebGPU composition, separate cold and 30Hz changing-tick warm samples; does not infer real-time playback FPS from sequential samples'
};
let original: Project,
  sessionId = '',
  native: Project['timeline']['tracks'][number]['items'][number];

function check(value: unknown, message: string): asserts value {
  if (!value) throw new Error(message);
}
function statistics(values: number[]) {
  const ordered = [...values].sort((a, b) => a - b);
  return {
    count: values.length,
    mean: values.length ? values.reduce((sum, value) => sum + value, 0) / values.length : null,
    median: ordered[Math.floor(ordered.length / 2)] ?? null,
    p95: ordered[Math.ceil(ordered.length * 0.95) - 1] ?? null,
    min: ordered[0] ?? null,
    max: ordered.at(-1) ?? null
  };
}
function timingSummary(samples: any[]) {
  const fields = new Map<string, number[]>();
  const visit = (value: any, path: string) => {
    if (typeof value === 'number' && Number.isFinite(value)) {
      if (!fields.has(path)) fields.set(path, []);
      fields.get(path)!.push(value);
    } else if (value && typeof value === 'object' && !Array.isArray(value))
      for (const [key, entry] of Object.entries(value)) visit(entry, path ? `${path}.${key}` : key);
  };
  for (const sample of samples) visit(sample.timings, '');
  return {
    availableFrames: samples.filter((sample) => sample.timings).length,
    stages: Object.fromEntries([...fields].map(([key, values]) => [key, statistics(values)]))
  };
}
function sources(project: Project) {
  return project.timeline.tracks.flatMap((track) =>
    track.items.map((item) => {
      const asset = project.assets.find((asset) => asset.id === item.clip.assetId);
      return {
        itemId: item.id,
        kind: item.clip.type,
        enabled: item.enabled,
        name: item.name,
        originalDimensions: item.clip.html
          ? {
              width: item.clip.html.width,
              height: item.clip.html.height,
              from: 'authored-html-viewport'
            }
          : asset
            ? { width: asset.width, height: asset.height, from: 'original-media-probe' }
            : { ...project.canvas, from: 'native-text-original-render-target' },
        placement: item.placement,
        source: item.clip.source,
        text: item.clip.text,
        assetId: asset?.id
      };
    })
  );
}
async function publish() {
  const json = JSON.stringify(report, null, 2);
  status.textContent = json;
  document.querySelector('#raw-report')!.textContent = json;
  if (reportUrl)
    await fetch(`${reportUrl}/report`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-QA-Report': 'render' },
      body: json
    }).catch(() => {});
}
async function yieldFrame() {
  await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
}
async function baseline() {
  const snapshot = query.has('session')
    ? await client.getSession(query.get('session')!)
    : await (async () => {
        const response = await fetch(projectUrl, { cache: 'no-store' });
        check(response.ok, `Cannot read original QA project: ${response.status}`);
        const baseline = await response.json();
        return client.createSession(baseline.project || baseline);
      })();
  original = snapshot.project;
  sessionId = snapshot.id;
  check(
    original.canvas.width === W && original.canvas.height === H,
    'Original project must remain 1920 × 1080'
  );
  check(
    original.frameRate.numerator / original.frameRate.denominator === 30,
    'Original project must retain 30Hz frame rate'
  );
  const text = original.timeline.tracks
    .flatMap((track) => track.items)
    .find((item) => item.clip.text?.template);
  check(
    text?.clip.text?.content === '灵感花开',
    'Keep the original four-character heavy flower template'
  );
  native = text;
  check(
    original.timeline.tracks.some((track) => track.items.some((item) => item.clip.html)),
    'Original project must retain HTML animation'
  );
  report.session = sessionId;
  report.frameRate = original.frameRate;
  report.originalSources = sources(original);
  report.originalProjectSha256 = await sha256(new TextEncoder().encode(JSON.stringify(original)));
}
function canvas(label: string) {
  const title = document.createElement('p'),
    canvas = document.createElement('canvas');
  title.textContent = label;
  canvas.width = W;
  canvas.height = H;
  frames.append(title, canvas);
  return canvas;
}
function summarize(samples: any[]) {
  return {
    frameMs: statistics(samples.map((sample) => sample.frameMs)),
    timings: timingSummary(samples),
    sourceMs: Object.fromEntries(
      ['video', 'text', 'html-clip'].map((kind) => [
        kind,
        statistics(
          samples.flatMap((sample) =>
            sample.media
              .filter((source: any) => source.kind === kind)
              .map((source: any) => source.sourceMs)
          )
        )
      ])
    ),
    uniqueTimes: [...new Set(samples.map((sample) => sample.time))],
    sizes: [...new Set(samples.map((sample) => `${sample.width}x${sample.height}`))]
  };
}
async function matrix() {
  const cases = [
    { id: 'media-only', native: false, html: false },
    { id: 'native-only', native: true, html: false },
    { id: 'html-only', native: false, html: true },
    { id: 'native-and-html', native: true, html: true }
  ];
  for (const test of cases) {
    const project = structuredClone(original);
    for (const track of project.timeline.tracks)
      for (const item of track.items) {
        if ((item.clip.text?.template && !test.native) || (item.clip.html && !test.html))
          item.enabled = false;
      }
    const output = canvas(`${test.id} · 1920 × 1080 · textWorkers=${textWorkers}`),
      init = performance.now();
    const renderer = new SceneRenderer(output, (id) => client.mediaUrl(sessionId, id), {
      textWorkers
    });
    const result: any = {
      ...test,
      textWorkers,
      sources: sources(project),
      cold: null,
      warm: [],
      reverse: null
    };
    report.results.push(result);
    try {
      await renderer.ready;
      result.rendererInitializationMs = performance.now() - init;
      report.phase = `${test.id}:cold`;
      await publish();
      await yieldFrame();
      result.cold = await renderer.render(project, ticks(2), W, H);
      renderer.prepareAhead(project, ticks(2), W, H);
      for (let index = 1; index <= 4; index++) {
        await yieldFrame();
        const time = ticks(2) + index * 4000;
        const sample = await renderer.render(project, time, W, H);
        result.warm.push(sample);
        renderer.prepareAhead(project, time, W, H);
      }
      renderer.invalidatePrefetch();
      result.reverse = await renderer.render(project, ticks(2), W, H);
      result.warmSummary = summarize(result.warm);
      result.sameFullHd = [result.cold, ...result.warm, result.reverse].every(
        (sample) => sample.width === W && sample.height === H
      );
      check(result.sameFullHd, 'Matrix render dimensions changed');
      result.passed = true;
    } finally {
      renderer.dispose();
      await publish();
    }
  }
}
function full(frame: TextFrame) {
  const data = new Uint8Array(W * H * 4);
  check(
    frame.data.length === frame.width * frame.height * 4,
    'Original native source RGBA must be tight'
  );
  const left = Math.max(0, frame.originX),
    right = Math.min(W, frame.originX + frame.width);
  for (let y = Math.max(0, frame.originY); y < Math.min(H, frame.originY + frame.height); y++) {
    if (right <= left) break;
    const source = ((y - frame.originY) * frame.width + left - frame.originX) * 4;
    data.set(frame.data.subarray(source, source + (right - left) * 4), (y * W + left) * 4);
  }
  return data;
}
function difference(a: Uint8Array, b: Uint8Array) {
  check(
    a.byteLength === W * H * 4 && b.byteLength === a.byteLength,
    'Direct comparison requires full-HD RGBA'
  );
  let changedBytes = 0,
    changedPixels = 0,
    maxByteError = 0,
    visiblePixels = 0;
  let firstMismatch: any = null;
  for (let i = 0; i < a.length; i += 4) {
    let changed = false;
    if (a[i + 3] || b[i + 3]) visiblePixels++;
    for (let c = 0; c < 4; c++)
      if (a[i + c] !== b[i + c]) {
        changed = true;
        changedBytes++;
        maxByteError = Math.max(maxByteError, Math.abs(a[i + c] - b[i + c]));
        firstMismatch ||= {
          x: (i / 4) % W,
          y: Math.floor(i / 4 / W),
          channel: c,
          off: a[i + c],
          on: b[i + c]
        };
      }
    if (changed) changedPixels++;
  }
  return {
    comparedBytes: a.byteLength,
    changedBytes,
    changedPixels,
    maxByteError,
    firstMismatch,
    visiblePixels,
    equal: changedBytes === 0
  };
}
async function pixels() {
  const nativeCanvas = new OffscreenCanvas(W, H),
    player = await createTemplatePlayer(
      nativeCanvas,
      native.clip.text!.template!,
      native.clip.text!.content,
      W,
      H
    );
  const pool = new TextPrefetchPool(),
    result: any = { mode: 'pool-source-and-composite-pixel-ab', source: [], composite: [] };
  report.results.push(result);
  try {
    for (const timeUs of [1500000, 1533333, 1500000]) {
      report.phase = `native-source-pixel-ab:${timeUs}`;
      await publish();
      await yieldFrame();
      const off = await player.render(timeUs, { profile: true });
      check(off.frame, 'Original native source frame missing');
      const on = await pool.request({
        key: JSON.stringify([native.clip.text, W, H, timeUs]),
        template: native.clip.text!.template!,
        text: native.clip.text!.content,
        width: W,
        height: H,
        timeUs
      });
      const a = full(off.frame),
        b = full(on),
        { data: _offData, ...offMetadata } = off.frame,
        { data: _onData, ...onMetadata } = on;
      result.source.push({
        timeUs,
        ...difference(a, b),
        off: offMetadata,
        on: onMetadata,
        offSha256: await sha256(a),
        onSha256: await sha256(b),
        pool: pool.stats()
      });
    }
  } finally {
    player.dispose();
    pool.dispose();
  }
  const offCanvas = canvas('完整合成 · 预渲染关闭'),
    onCanvas = canvas('完整合成 · 预渲染开启');
  const off = new SceneRenderer(offCanvas, (id) => client.mediaUrl(sessionId, id), {
    textWorkers: false
  });
  const on = new SceneRenderer(onCanvas, (id) => client.mediaUrl(sessionId, id), {
    textWorkers: true
  });
  try {
    await Promise.all([off.ready, on.ready]);
    for (const time of [ticks(2), ticks(2) + 4000, ticks(1.6)]) {
      report.phase = `full-composite-pixel-ab:${time}`;
      await publish();
      await yieldFrame();
      const before = await off.render(original, time, W, H),
        a = await off.readPixels();
      const after = await on.render(original, time, W, H, undefined, {
          prefetch: true,
          htmlPrefetch: true
        }),
        b = await on.readPixels();
      check(
        before.backend === 'webgpu' && after.backend === 'webgpu',
        'Full composite evidence requires actual WebGPU'
      );
      result.composite.push({
        time,
        ...difference(a, b),
        off: before,
        on: after,
        offSha256: await sha256(a),
        onSha256: await sha256(b)
      });
    }
    result.passed = [...result.source, ...result.composite].every(
      (sample) => sample.equal && sample.visiblePixels > 100
    );
    check(result.passed, 'Pool changed original source or full WebGPU composite pixels');
  } finally {
    off.dispose();
    on.dispose();
    await publish();
  }
}
async function run(mode: 'matrix' | 'pixels') {
  if (runButton.disabled) return;
  buttons.forEach((button) => (button.disabled = true));
  frames.replaceChildren();
  report.results = [];
  report.complete = false;
  report.passed = false;
  report.started = new Date().toISOString();
  report.userAgent = navigator.userAgent;
  report.mode = mode;
  delete report.error;
  try {
    report.phase = 'original-project-loading';
    await publish();
    await baseline();
    if (mode === 'matrix') await matrix();
    else await pixels();
    report.passed = report.results.every((result: any) => result.passed);
    report.complete = true;
    report.phase = 'complete';
  } catch (error) {
    report.phase = 'failed';
    report.error = error instanceof Error ? error.stack : String(error);
  } finally {
    report.completed = new Date().toISOString();
    buttons.forEach((button) => (button.disabled = false));
    await publish();
  }
}
runButton.addEventListener('click', () => void run('matrix'));
compareButton.addEventListener('click', () => void run('pixels'));
document.querySelector('#download')!.addEventListener('click', () => {
  const url = URL.createObjectURL(
      new Blob([JSON.stringify(report, null, 2)], { type: 'application/json' })
    ),
    link = document.createElement('a');
  link.href = url;
  link.download = `full-hd-scene-${report.mode || 'profile'}.json`;
  link.click();
  URL.revokeObjectURL(url);
});
void publish();
