import {
  createTextEngine,
  loadTextTemplate,
  sha256,
  type TextEngine,
  type TextFrame,
  type TemplateBundle
} from '../packages/text-wasm/dist/index.mjs';
import { composeRecipe, resolveRecipe } from '../packages/text-wasm/src/recipes.mjs';
import {
  chooseTemplateFonts,
  rewriteTemplateFonts,
  type SystemTemplateFont,
  type SystemTemplateFontCatalog
} from '../packages/text-wasm/src/system-fonts.mjs';
import type { TextContent } from '../packages/core/types';

const W = 1920,
  H = 1080;
const query = new URLSearchParams(location.search);
const button = document.querySelector<HTMLButtonElement>('#run')!;
const download = document.querySelector<HTMLButtonElement>('#download')!;
const status = document.querySelector<HTMLElement>('#status')!;
const frames = document.querySelector<HTMLElement>('#frames')!;
const reportUrl = query.get('report');
const baselineRoot =
  query.get('baseline') ||
  '/@fs/Users/jxinfa/WebstormProjects/videocut/.local/qa/wasm-native-opt/baseline/';
const baselineSdkUrl = new URL(
  query.get('baselineSdk') || `${baselineRoot}index.mjs`,
  location.origin
).href;
const baselineWasmUrl = new URL(
  query.get('baselineWasm') || `${baselineRoot}videocut-text.wasm`,
  location.origin
).href;
const currentRoot =
  query.get('current') ||
  '/@fs/Users/jxinfa/WebstormProjects/videocut/.local/qa/wasm-native-opt/current/';
const currentSdkUrl = new URL(query.get('currentSdk') || `${currentRoot}index.mjs`, location.origin)
  .href;
const currentWasmUrl = new URL(
  query.get('currentWasm') || `${currentRoot}videocut-text.wasm`,
  location.origin
).href;
const originalTemplate = {
  id: 'skill-forward-flower',
  version: 1 as const,
  recipe: {
    base: 'flower-style-38',
    backdrop: 'bubble-nine-slice',
    animation: 'anim-lua-letter-transform'
  }
};
type Case = {
  id: string;
  text: string;
  template: NonNullable<TextContent['template']>;
  times: number[];
  nonzeroShadow?: boolean;
};
const cases: Case[] = [
  {
    id: 'flower38-original',
    text: '灵感花开',
    template: originalTemplate,
    times: [0, 100000, 350000, 1000000, 1500000, 2900000, 3500000, 350000]
  },
  {
    id: 'flower03-original',
    text: '心动',
    template: { id: 'quality-flower03', version: 1, recipe: { base: 'flower-style-03' } },
    times: [350000, 1500000, 350000]
  },
  {
    id: 'flower03-composed',
    text: '心动',
    template: { id: 'layered-flower', version: 1 },
    times: [350000, 1500000, 350000]
  },
  {
    id: 'flower38-nonzero-shadow-control',
    text: '灵感花开',
    template: originalTemplate,
    nonzeroShadow: true,
    times: [350000, 1500000, 350000]
  }
];
const report: any = {
  kind: 'full-hd-native-pixel-quality-ab',
  phase: 'idle',
  complete: false,
  passed: false,
  resolution: [W, H],
  fullFrameRgbaBytes: W * H * 4,
  comparison:
    'Direct byte-by-byte native straight RGBA; native crop copied to its original full-HD coordinates without scaling, filtering, canvas readback or lossy encoding',
  digestRole: 'Additional reproducibility metadata; never used in place of direct byte comparison',
  baselineSdkUrl,
  baselineWasmUrl,
  currentSdkUrl,
  currentWasmUrl,
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
async function yieldFrame() {
  check(!stopped, 'Cancelled');
  await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
}
async function bytes(url: string) {
  const response = await fetch(url, { cache: 'no-store' });
  check(response.ok, `Resource request failed: ${response.status} ${url}`);
  return new Uint8Array(await response.arrayBuffer());
}
async function systemFonts() {
  const response = await fetch('/api/fonts');
  check(response.ok, 'Cannot read this computer’s system font catalog');
  const catalog = (await response.json()) as SystemTemplateFontCatalog;
  const { sans, cjk } = chooseTemplateFonts(catalog);
  const fonts = await Promise.all(
    [...new Map([sans, cjk].map((font) => [font.id, font])).values()].map(async (font) => {
      const resource = await fetch(`/api/fonts/${encodeURIComponent(font.id)}`);
      check(resource.ok, `System font unavailable: ${font.family}`);
      return {
        ...font,
        mediaType: resource.headers.get('Content-Type') || 'font/ttf',
        bytes: new Uint8Array(await resource.arrayBuffer())
      };
    })
  );
  report.fonts = await Promise.all(
    fonts.map(async ({ bytes: value, mediaType, ...font }) => ({
      ...font,
      mediaType,
      bytes: value.byteLength,
      sha256: await sha256(value)
    }))
  );
  report.fontDefaults = catalog.defaults;
  return { catalog, fonts };
}
function fullRgba(frame: TextFrame) {
  check(
    frame.data.length === frame.width * frame.height * 4,
    'Native output is not tightly packed RGBA'
  );
  check(
    [frame.originX, frame.originY, frame.width, frame.height].every(Number.isInteger),
    'Native crop must use integer coordinates'
  );
  const data = new Uint8ClampedArray(W * H * 4);
  const x = Math.max(0, frame.originX),
    right = Math.min(W, frame.originX + frame.width);
  const top = Math.max(0, frame.originY),
    bottom = Math.min(H, frame.originY + frame.height);
  if (right <= x || bottom <= top) return data;
  for (let y = top; y < bottom; y++) {
    const source = ((y - frame.originY) * frame.width + x - frame.originX) * 4;
    data.set(frame.data.subarray(source, source + (right - x) * 4), (y * W + x) * 4);
  }
  return data;
}
function compare(a: Uint8ClampedArray, b: Uint8ClampedArray) {
  check(
    a.length === b.length && a.length === W * H * 4,
    'Comparison must cover every full-HD RGBA byte'
  );
  let changedBytes = 0,
    changedPixels = 0,
    changedVisiblePixels = 0,
    maxByteError = 0,
    changedAlphaPixels = 0,
    maxAlphaError = 0;
  let firstMismatch: any = null;
  const channel = ['r', 'g', 'b', 'a'];
  for (let pixel = 0; pixel < W * H; pixel++) {
    let changed = false;
    for (let c = 0; c < 4; c++) {
      const i = pixel * 4 + c,
        error = Math.abs(a[i] - b[i]);
      if (!error) continue;
      changed = true;
      changedBytes++;
      maxByteError = Math.max(maxByteError, error);
      if (c === 3) {
        changedAlphaPixels++;
        maxAlphaError = Math.max(maxAlphaError, error);
      }
      firstMismatch ||= {
        byteIndex: i,
        x: pixel % W,
        y: Math.floor(pixel / W),
        channel: channel[c],
        baseline: a[i],
        current: b[i]
      };
    }
    if (changed) {
      changedPixels++;
      if (a[pixel * 4 + 3] || b[pixel * 4 + 3]) changedVisiblePixels++;
    }
  }
  return {
    comparedBytes: a.length,
    changedBytes,
    changedPixels,
    changedVisiblePixels,
    maxByteError,
    changedAlphaPixels,
    maxAlphaError,
    firstMismatch,
    equal: changedBytes === 0
  };
}
function alphaSummary(data: Uint8ClampedArray) {
  let visiblePixels = 0,
    opaquePixels = 0;
  for (let i = 3; i < data.length; i += 4) {
    if (data[i]) visiblePixels++;
    if (data[i] === 255) opaquePixels++;
  }
  return { visiblePixels, opaquePixels };
}
async function frameEvidence(frame: TextFrame, full: Uint8ClampedArray) {
  const { data, timings: _timings, ...metadata } = frame;
  return {
    frame: metadata,
    fullFrameAlpha: alphaSummary(full),
    fullFrameSha256: await sha256(new Uint8Array(full.buffer)),
    nativeCropSha256: await sha256(new Uint8Array(data.buffer, data.byteOffset, data.byteLength))
  };
}
function setNonzeroShadow(bundle: TemplateBundle) {
  const composition = bundle.composition as any;
  const targets: any[] = [];
  function visit(value: any) {
    if (!value || typeof value !== 'object') return;
    if (
      value.type === 'shadow' &&
      value.layer_id === 'shadow-1' &&
      value.material?.material?.kind === 'texture'
    )
      targets.push(value);
    for (const entry of Object.values(value)) visit(entry);
  }
  visit(composition);
  check(
    targets.length === 1,
    'Nonzero control requires exactly the original flower38 shadow-1 texture'
  );
  const target = targets[0];
  check(
    target.material.material.opacity === 0 && target.blur_radius > 0,
    'Original control shadow must be alpha 0 with its original nonzero blur'
  );
  const before = structuredClone(target);
  target.material.material.opacity = 0.35;
  return {
    path: 'composition.document.*.materials.layers.shadow-1.material.material.opacity',
    before,
    after: structuredClone(target)
  };
}
function canvases(id: string) {
  const section = document.createElement('section'),
    title = document.createElement('p'),
    pair = document.createElement('div');
  title.textContent = `${id} · 原始画布 ${W} × ${H}`;
  pair.className = 'pair';
  const create = (caption: string) => {
    const figure = document.createElement('figure'),
      label = document.createElement('figcaption'),
      canvas = document.createElement('canvas');
    label.textContent = caption;
    canvas.width = W;
    canvas.height = H;
    figure.append(label, canvas);
    pair.append(figure);
    return canvas;
  };
  const baseline = create('优化前'),
    current = create('优化后');
  section.append(title, pair);
  frames.append(section);
  return { baseline, current };
}
async function loadRenderer(
  engine: TextEngine,
  bundle: TemplateBundle,
  fonts: Array<SystemTemplateFont & { mediaType: string; bytes: Uint8Array }>,
  text: string
) {
  const renderer = engine.createRenderer();
  try {
    for (const font of fonts) renderer.registerAsset(font.id, font.mediaType, font.bytes);
    const info = await renderer.loadTemplate(structuredClone(bundle), {
      bindings: { content: text },
      allowRasterFallback: true
    });
    check(
      !info.requiresBrowserComposition,
      'These cases must retain the original native raster path'
    );
    return { renderer, info };
  } catch (error) {
    renderer.dispose();
    throw error;
  }
}
async function one(
  test: Case,
  baselineEngine: TextEngine,
  currentEngine: TextEngine,
  resources: Awaited<ReturnType<typeof systemFonts>>,
  originalFrames: Map<number, Uint8ClampedArray>
) {
  const recipe = resolveRecipe(test.template);
  check(!recipe.external, 'Native quality fixture cannot use an external GPU template');
  const composed = await composeRecipe(recipe, (part: string) =>
    loadTextTemplate(
      new URL(
        `/text-templates/templates/com.videocut.text.qt-type.${part}/manifest.json`,
        location.origin
      )
    )
  );
  const bundle = rewriteTemplateFonts(composed.bundle, resources.catalog) as TemplateBundle;
  const result: any = {
    id: test.id,
    template: test.template,
    recipe,
    text: test.text,
    sources: composed.sources,
    resolution: [W, H],
    requestedTimes: test.times,
    samples: [],
    reverseComparisons: []
  };
  if (test.nonzeroShadow) result.nonzeroShadow = setNonzeroShadow(bundle);
  report.results.push(result);
  report.phase = `${test.id}:loading`;
  await publish();
  const baseline = await loadRenderer(baselineEngine, bundle, resources.fonts, test.text);
  let current: Awaited<ReturnType<typeof loadRenderer>> | undefined;
  const repeats = new Set(test.times.filter((time, i, all) => all.indexOf(time) !== i));
  const retained = new Map<number, { baseline: Uint8ClampedArray; current: Uint8ClampedArray }>();
  try {
    current = await loadRenderer(currentEngine, bundle, resources.fonts, test.text);
    result.baselineLoad = baseline.info;
    result.currentLoad = current.info;
    check(baseline.info.durationUs === current.info.durationUs, 'Template duration differs');
    const views = canvases(test.id);
    for (let i = 0; i < test.times.length; i++) {
      await yieldFrame();
      const timeUs = test.times[i];
      check(timeUs < baseline.info.durationUs, `Fixture time ${timeUs} exceeds template duration`);
      report.phase = `${test.id}:${i + 1}/${test.times.length}@${timeUs}us`;
      const before = baseline.renderer.render({ timeUs, width: W, height: H });
      const after = current.renderer.render({ timeUs, width: W, height: H });
      const beforeFull = fullRgba(before),
        afterFull = fullRgba(after);
      const difference = compare(beforeFull, afterFull);
      const sample: any = {
        timeUs,
        cold: i === 0,
        direction: i && timeUs < test.times[i - 1] ? 'reverse' : 'forward',
        ...difference,
        baseline: await frameEvidence(before, beforeFull),
        current: await frameEvidence(after, afterFull),
        rasterGeometryEqual:
          before.width === after.width &&
          before.height === after.height &&
          before.originX === after.originX &&
          before.originY === after.originY
      };
      if (retained.has(timeUs)) {
        const previous = retained.get(timeUs)!;
        result.reverseComparisons.push({
          timeUs,
          baseline: compare(previous.baseline, beforeFull),
          current: compare(previous.current, afterFull)
        });
      } else if (repeats.has(timeUs))
        retained.set(timeUs, { baseline: beforeFull, current: afterFull });
      if (test.id === 'flower38-original' && [350000, 1500000].includes(timeUs))
        originalFrames.set(timeUs, beforeFull);
      if (test.nonzeroShadow) {
        const original = originalFrames.get(timeUs);
        check(original, `Missing original alpha-0 comparison at ${timeUs}`);
        sample.nonzeroControlVsOriginal = compare(original, beforeFull);
        check(
          sample.nonzeroControlVsOriginal.changedVisiblePixels > 100,
          `Nonzero shadow control must visibly alter the original output at ${timeUs}`
        );
      }
      result.samples.push(sample);
      views.baseline.getContext('2d')!.putImageData(new ImageData(beforeFull, W, H), 0, 0);
      views.current.getContext('2d')!.putImageData(new ImageData(afterFull, W, H), 0, 0);
      await publish();
    }
    result.maxByteError = Math.max(...result.samples.map((sample: any) => sample.maxByteError));
    result.changedBytes = result.samples.reduce(
      (sum: number, sample: any) => sum + sample.changedBytes,
      0
    );
    result.changedPixels = result.samples.reduce(
      (sum: number, sample: any) => sum + sample.changedPixels,
      0
    );
    result.nonblank = result.samples.some(
      (sample: any) =>
        sample.baseline.fullFrameAlpha.visiblePixels > 100 &&
        sample.current.fullFrameAlpha.visiblePixels > 100
    );
    check(
      result.nonblank,
      `${test.id}: all frames are blank; natural transparent animation ticks remain valid comparison frames`
    );
    result.passed =
      result.samples.every((sample: any) => sample.equal) &&
      result.reverseComparisons.every(
        (sample: any) => sample.baseline.equal && sample.current.equal
      );
  } finally {
    baseline.renderer.dispose();
    current?.renderer.dispose();
    await publish();
  }
}
async function run() {
  if (button.disabled) return;
  button.disabled = true;
  download.disabled = true;
  stopped = false;
  frames.replaceChildren();
  report.results = [];
  report.logs = [];
  report.complete = false;
  report.passed = false;
  report.phase = 'loading-engines-and-system-fonts';
  report.started = new Date().toISOString();
  report.userAgent = navigator.userAgent;
  report.hardwareConcurrency = navigator.hardwareConcurrency;
  delete report.error;
  let baselineEngine: TextEngine | undefined, currentEngine: TextEngine | undefined;
  try {
    await publish();
    const [baselineSdk, currentSdk, baselineBytes, currentBytes, resources] = await Promise.all([
      import(/* @vite-ignore */ baselineSdkUrl) as Promise<{
        createTextEngine: typeof createTextEngine;
      }>,
      import(/* @vite-ignore */ currentSdkUrl) as Promise<{
        createTextEngine: typeof createTextEngine;
      }>,
      bytes(baselineWasmUrl),
      bytes(new URL(currentWasmUrl, location.origin).href),
      systemFonts()
    ]);
    const [baselineSha256, currentSha256] = await Promise.all([
      sha256(baselineBytes),
      sha256(currentBytes)
    ]);
    check(
      baselineSha256 !== currentSha256,
      'Baseline and optimized WASM bytes are identical; quality A/B would not test an optimization'
    );
    report.wasm = {
      baseline: { url: baselineWasmUrl, bytes: baselineBytes.byteLength, sha256: baselineSha256 },
      current: {
        url: new URL(currentWasmUrl, location.origin).href,
        bytes: currentBytes.byteLength,
        sha256: currentSha256
      }
    };
    baselineEngine = await baselineSdk.createTextEngine({
      wasmBinary: baselineBytes,
      printErr: (message) => report.logs.push({ engine: 'baseline', message })
    });
    currentEngine = await currentSdk.createTextEngine({
      wasmBinary: currentBytes,
      printErr: (message) => report.logs.push({ engine: 'current', message })
    });
    report.capabilities = {
      baseline: baselineEngine.capabilities,
      current: currentEngine.capabilities
    };
    const originalFrames = new Map<number, Uint8ClampedArray>();
    for (const test of cases)
      await one(test, baselineEngine, currentEngine, resources, originalFrames);
    report.maxByteError = Math.max(...report.results.map((result: any) => result.maxByteError));
    report.changedBytes = report.results.reduce(
      (sum: number, result: any) => sum + result.changedBytes,
      0
    );
    report.changedPixels = report.results.reduce(
      (sum: number, result: any) => sum + result.changedPixels,
      0
    );
    report.comparedFrames = report.results.reduce(
      (sum: number, result: any) => sum + result.samples.length,
      0
    );
    report.passed = report.results.every((result: any) => result.passed);
    report.complete = true;
    report.phase = report.passed ? 'complete' : 'pixel-mismatch';
  } catch (error) {
    report.phase = stopped ? 'cancelled' : 'failed';
    report.error = error instanceof Error ? error.stack : String(error);
  } finally {
    baselineEngine?.dispose();
    currentEngine?.dispose();
    report.completed = new Date().toISOString();
    button.disabled = false;
    download.disabled = false;
    await publish();
  }
}
button.addEventListener('click', () => void run());
document.querySelector('#stop')!.addEventListener('click', () => {
  stopped = true;
});
download.addEventListener('click', () => {
  const url = URL.createObjectURL(
    new Blob([JSON.stringify(report, null, 2)], { type: 'application/json' })
  );
  const link = document.createElement('a');
  link.href = url;
  link.download = 'native-full-hd-pixel-quality-ab.json';
  link.click();
  URL.revokeObjectURL(url);
});
void publish();
