import {
  createTemplatePlayer,
  type TemplatePlayer,
  type TemplateRenderResult
} from '../packages/render/text';
import { recipes, resolveRecipe } from '../packages/text-wasm/src/recipes.mjs';
import { sha256 } from '../packages/text-wasm/dist/index.mjs';
import type { TextContent } from '../packages/core/types';

const query = new URLSearchParams(location.search);
const button = document.querySelector<HTMLButtonElement>('#run')!;
const status = document.querySelector<HTMLElement>('#status')!;
const frames = document.querySelector<HTMLElement>('#frames')!;
const reportUrl = query.get('report');
const W = 1920,
  H = 1080;
const text = query.get('text') || '灵感花开';
const count = Math.max(3, Math.min(30, Number(query.get('count')) || 6));
const report: any = {
  kind: 'full-hd-text-cpu-profile',
  phase: 'idle',
  complete: false,
  resolution: [W, H],
  timingScope:
    'Synchronous JS/WASM call boundary plus canvas draw; excludes pixel readback, hashing, async resource/font loading and scene/GPU compositing',
  count,
  text,
  results: [],
  logs: []
};
let stopped = false;
function check(value: unknown, message: string): asserts value {
  if (!value) throw new Error(message);
}
async function publish() {
  status.textContent = JSON.stringify(report, null, 2);
  if (reportUrl)
    await fetch(`${reportUrl}/report`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-QA-Report': 'render' },
      body: JSON.stringify(report)
    }).catch(() => {});
}
function statistics(values: number[]) {
  const sorted = [...values].sort((a, b) => a - b);
  return {
    count: sorted.length,
    mean: values.reduce((sum, value) => sum + value, 0) / Math.max(1, values.length),
    median: sorted[Math.floor(sorted.length / 2)] ?? null,
    p95: sorted[Math.min(sorted.length - 1, Math.ceil(sorted.length * 0.95) - 1)] ?? null,
    min: sorted[0] ?? null,
    max: sorted.at(-1) ?? null
  };
}
async function yieldFrame() {
  check(!stopped, 'Cancelled');
  await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
}
function alphaSummary(data: Uint8Array | Uint8ClampedArray) {
  let visible = 0,
    opaque = 0;
  for (let i = 3; i < data.length; i += 4) {
    if (data[i]) visible++;
    if (data[i] === 255) opaque++;
  }
  return { visiblePixels: visible, opaquePixels: opaque };
}
function measureFrame(result: TemplateRenderResult) {
  check(result.frame && result.timings, 'Expected the profiled native WASM path');
  const { data, ...frame } = result.frame;
  return { ms: result.ms, rasterBounds: result.rasterBounds, frame };
}
async function digests(player: TemplatePlayer, result: TemplateRenderResult) {
  check(result.frame, 'Native frame missing');
  const started = performance.now();
  const full = await player.pixels();
  const readbackMs = performance.now() - started;
  check(full.width === W && full.height === H, 'Full HD output resolution changed');
  check(full.data.byteLength === W * H * 4, 'Full HD RGBA byte length changed');
  const hashStarted = performance.now();
  const [rgbaDigest, canvasDigest] = await Promise.all([
    sha256(
      new Uint8Array(
        result.frame.data.buffer,
        result.frame.data.byteOffset,
        result.frame.data.byteLength
      )
    ),
    sha256(new Uint8Array(full.data.buffer, full.data.byteOffset, full.data.byteLength))
  ]);
  return {
    rgbaDigest,
    canvasDigest,
    nativeRgbaBytes: result.frame.data.byteLength,
    fullCanvasRgbaBytes: full.data.byteLength,
    fullCanvasAlpha: alphaSummary(full.data),
    readbackMs,
    hashMs: performance.now() - hashStarted
  };
}
function summarize(samples: any[]) {
  const timings = [
    'nativeRenderMs',
    'resultReadMs',
    'pixelPointerMs',
    'pixelAllocationMs',
    'rowCopyMs',
    'renderTotalMs',
    'imageDataMs',
    'clearRectMs',
    'putImageDataMs',
    'drawTotalMs'
  ];
  return {
    playerMs: statistics(samples.map((sample) => sample.ms)),
    stages: Object.fromEntries(
      timings.map((stage) => [
        stage,
        statistics(samples.map((sample) => Number(sample.frame.timings[stage] || 0)))
      ])
    ),
    nativeCallFraction:
      samples.reduce((sum, sample) => sum + sample.frame.timings.nativeRenderMs, 0) /
      Math.max(
        0.000001,
        samples.reduce((sum, sample) => sum + sample.frame.timings.drawTotalMs, 0)
      )
  };
}
function templates(): Array<NonNullable<TextContent['template']>> {
  if (query.has('recipe'))
    return [{ id: 'profile-custom-flower', version: 1, recipe: JSON.parse(query.get('recipe')!) }];
  if (!query.has('templates'))
    return [
      {
        id: 'skill-forward-flower',
        version: 1,
        recipe: {
          base: 'flower-style-38',
          backdrop: 'bubble-nine-slice',
          animation: 'anim-lua-letter-transform'
        }
      }
    ];
  return query
    .get('templates')!
    .split(',')
    .map((id) => ({ id: id.trim(), version: 1 }));
}
async function one(template: NonNullable<TextContent['template']>) {
  const recipe = resolveRecipe(template);
  check(
    !recipe.external,
    'This benchmark isolates native raster templates; external GPU templates use a different path'
  );
  const section = document.createElement('section');
  section.className = 'frame';
  const label = document.createElement('p');
  label.textContent = `${template.id} · ${text} · ${W} × ${H}`;
  const canvas = document.createElement('canvas');
  canvas.width = W;
  canvas.height = H;
  section.append(label, canvas);
  frames.append(section);
  const result: any = {
    template,
    recipe,
    text,
    resolution: [W, H],
    sameTime: [],
    changingTime: []
  };
  report.results.push(result);
  report.phase = `${template.id}:loading`;
  await publish();
  const initialization = performance.now();
  const player = await createTemplatePlayer(canvas, template, text, W, H);
  result.initializationMs = performance.now() - initialization;
  result.durationUs = player.durationUs;
  const requestedTime = query.has('timeUs') ? Number(query.get('timeUs')) : recipe.timeUs;
  check(
    Number.isSafeInteger(requestedTime) && requestedTime >= 0,
    'timeUs must be a nonnegative integer'
  );
  const time = Math.max(0, Math.min(requestedTime, player.durationUs - 1));
  try {
    report.phase = `${template.id}:cold`;
    await publish();
    await yieldFrame();
    const cold = await player.render(time, { profile: true });
    result.cold = { ...measureFrame(cold), ...(await digests(player, cold)) };
    check(
      result.cold.fullCanvasAlpha.visiblePixels > 100,
      'Blank output cannot count as performance evidence'
    );
    for (let i = 0; i < count; i++) {
      report.phase = `${template.id}:same-time-warm-${i + 1}/${count}`;
      await publish();
      await yieldFrame();
      const rendered = await player.render(time, { profile: true });
      result.sameTime.push({ ...measureFrame(rendered), ...(await digests(player, rendered)) });
    }
    for (let i = 0; i < count; i++) {
      const next = Math.max(
        0,
        Math.min(player.durationUs - 1, Math.round(time + (i - count / 2) * 16667))
      );
      report.phase = `${template.id}:changing-time-warm-${i + 1}/${count}`;
      await publish();
      await yieldFrame();
      const rendered = await player.render(next, { profile: true });
      result.changingTime.push({ ...measureFrame(rendered), ...(await digests(player, rendered)) });
    }
    report.phase = `${template.id}:reverse-parity`;
    await publish();
    await yieldFrame();
    const restored = await player.render(time, { profile: true });
    result.restored = { ...measureFrame(restored), ...(await digests(player, restored)) };
    result.sameTimeSummary = summarize(result.sameTime);
    result.changingTimeSummary = summarize(result.changingTime);
    result.sameTimeDigestStable = result.sameTime.every(
      (sample: any) =>
        sample.rgbaDigest === result.cold.rgbaDigest &&
        sample.canvasDigest === result.cold.canvasDigest
    );
    result.reverseDigestStable =
      result.restored.rgbaDigest === result.cold.rgbaDigest &&
      result.restored.canvasDigest === result.cold.canvasDigest;
    result.passed = result.sameTimeDigestStable && result.reverseDigestStable;
    if (reportUrl) {
      const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve));
      if (blob)
        await fetch(`${reportUrl}/image?name=${encodeURIComponent(`full-hd-${template.id}`)}`, {
          method: 'POST',
          headers: { 'X-QA-Report': 'render' },
          body: blob
        }).catch(() => {});
    }
    check(result.passed, 'Warm or reverse seek changed RGBA output');
  } finally {
    player.dispose();
    await publish();
  }
}
async function run() {
  if (button.disabled) return;
  button.disabled = true;
  stopped = false;
  frames.replaceChildren();
  report.results = [];
  report.logs = [];
  report.complete = false;
  report.phase = 'starting';
  report.started = new Date().toISOString();
  report.userAgent = navigator.userAgent;
  report.hardwareConcurrency = navigator.hardwareConcurrency;
  delete report.error;
  try {
    check([...text].length === 4, 'Use exactly four Unicode characters for this benchmark');
    check(Number.isInteger(count), 'count must be an integer');
    const requested = templates();
    check(requested.length > 0 && requested.length <= 6, 'Choose 1–6 native templates');
    for (const template of requested) await one(template);
    report.phase = 'complete';
    report.complete = true;
    report.completed = new Date().toISOString();
  } catch (error) {
    report.phase = stopped ? 'cancelled' : 'failed';
    report.error = error instanceof Error ? error.stack : String(error);
  } finally {
    button.disabled = false;
    await publish();
  }
}
button.addEventListener('click', () => void run());
document.querySelector('#stop')!.addEventListener('click', () => {
  stopped = true;
});
void publish();
