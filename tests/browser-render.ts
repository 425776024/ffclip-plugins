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
import { renderTemplateExport } from '../src/editor/template-export';
import { recipes } from '../packages/text-wasm/src/recipes.mjs';
import { sharedGpu } from '../packages/render/gpu.mjs';
import type { EncodingMode } from '../packages/render/export';
import { AdaptivePreviewQuality } from '../packages/render/quality';
import { VISUAL_PACKAGES } from '../packages/render/catalog';

const options = new URLSearchParams(location.search),
  fixtures = options.get('fixtures') || '/tmp/videocut-browser-qa';
const destination = options.get('output') || `${fixtures}/render-results`;
const reportURL = options.get('report') || 'http://127.0.0.1:4332';
const status = document.querySelector('#status')!,
  preview = document.querySelector('#preview') as HTMLCanvasElement,
  decoded = document.querySelector('#decoded') as HTMLCanvasElement;
const client = new VideoCutClient(location.origin),
  logs: string[] = [],
  results: any[] = [],
  sessions: string[] = [];
let controller = new AbortController();
const report: any = {
  started: new Date().toISOString(),
  userAgent: navigator.userAgent,
  fixture: fixtures,
  phase: 'idle',
  results,
  logs
};
async function publish() {
  status.textContent = JSON.stringify(report, null, 2);
  await fetch(`${reportURL}/report`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-QA-Report': 'render' },
    body: JSON.stringify(report)
  }).catch(() => {});
}
async function note(value: string) {
  logs.push(value);
  await publish();
}
window.addEventListener('error', (event) => {
  void note(`UNCAUGHT ${event.error?.stack || event.message}`);
});
window.addEventListener('unhandledrejection', (event) => {
  void note(`REJECTION ${event.reason?.stack || event.reason}`);
});
async function image(name: string, canvas: HTMLCanvasElement) {
  const blob = await new Promise<Blob>((resolve, reject) =>
    canvas.toBlob((b) => (b ? resolve(b) : reject(new Error('PNG失败'))))
  );
  await fetch(`${reportURL}/image?name=${encodeURIComponent(name)}`, {
    method: 'POST',
    headers: { 'X-QA-Report': 'render' },
    body: blob
  });
}
function difference(a: Uint8Array, b: Uint8ClampedArray) {
  let sum = 0,
    squared = 0,
    nonBlack = 0;
  const histogram = new Uint32Array(256);
  for (let i = 0; i < a.length; i += 4) {
    if (a[i] + a[i + 1] + a[i + 2] > 12) nonBlack++;
    for (let c = 0; c < 3; c++) {
      const d = Math.abs(a[i + c] - b[i + c]);
      sum += d;
      squared += d * d;
      histogram[d]++;
    }
  }
  const n = (a.length / 4) * 3;
  let count = 0,
    p99 = 0;
  for (let i = 0; i < 256; i++) {
    count += histogram[i];
    if (count >= n * 0.99) {
      p99 = i;
      break;
    }
  }
  return {
    mae: sum / n,
    rmse: Math.sqrt(squared / n),
    psnr: 10 * Math.log10((255 * 255) / (squared / n)),
    p99,
    nonBlack
  };
}
function frameClip(
  p: Project,
  asset: any,
  start = 0,
  length = 0.6,
  source = 0.3,
  trackId?: string
) {
  const item = addAsset(p, asset, { start: ticks(start), trackId, validate: false });
  item.placement.end = ticks(start + length);
  item.clip.source = { begin: ticks(source), end: ticks(source + length) };
  return item;
}
function project(name: string) {
  const p = createProject(`QA ${name}`);
  p.canvas = { width: 640, height: 360 };
  return p;
}
/** An isolated realm gives every run a genuinely cold font cache, including repeated button clicks. */
async function fontRetryCases() {
  const scene = new SceneRenderer(new OffscreenCanvas(640, 360), () => {
    throw new Error('Font retry fixture has no media');
  });
  const p = project('font-retry');
  addText(p, { content: '字体重试 FONT RETRY', color: '#ffffff', fontSize: 64, length: ticks(1) });
  const originalFetch = globalThis.fetch;
  let injected = 0,
    initialError = '';
  globalThis.fetch = (input, init) => {
    const url = new URL(input instanceof Request ? input.url : String(input), location.href);
    if (url.pathname === '/api/fonts' && injected === 0) {
      injected++;
      return Promise.resolve(new Response('Injected font catalog outage', { status: 503 }));
    }
    return originalFetch.call(globalThis, input, init);
  };
  try {
    try {
      await scene.render(p, 0, 640, 360);
    } catch (error) {
      initialError = String(error);
    } finally {
      globalThis.fetch = originalFetch;
    }
    if (injected !== 1 || !initialError) throw new Error('Cold font failure was not exercised');
    const recovered = await scene.render(p, 0, 640, 360);
    const pixels = await scene.readPixels();
    let brightPixels = 0;
    for (let i = 0; i < pixels.length; i += 4)
      if (pixels[i] + pixels[i + 1] + pixels[i + 2] > 300) brightPixels++;
    const retry = {
      name: 'font-load-retry',
      injected,
      initialError,
      backend: recovered.backend,
      bounds: recovered.bounds,
      brightPixels,
      passed: recovered.bounds.some((b) => b.width > 1 && b.height > 1) && brightPixels > 100
    };
    if (!retry.passed) throw new Error('Same SceneRenderer stayed blank after font fetch recovery');
    const abandoned = new SceneRenderer(new OffscreenCanvas(32, 32), () => 'unused');
    abandoned.dispose();
    let readyError = '',
      renderError = '';
    try {
      await abandoned.ready;
    } catch (error) {
      readyError = String(error);
    }
    try {
      await abandoned.render(project('disposed'), 0, 32, 32);
    } catch (error) {
      renderError = String(error);
    } finally {
      abandoned.dispose();
    }
    const disposal = {
      name: 'dispose-before-ready',
      readyError,
      renderError,
      passed: !!readyError && !!renderError
    };
    if (!disposal.passed) throw new Error('Disposed SceneRenderer returned a ready/render result');
    return [retry, disposal];
  } finally {
    globalThis.fetch = originalFetch;
    scene.dispose();
  }
}
async function isolatedFontRetry() {
  const iframe = document.createElement('iframe');
  iframe.hidden = true;
  const url = new URL(location.href);
  url.searchParams.set('case', 'font-retry');
  iframe.src = url.href;
  try {
    return await new Promise<any[]>((resolve, reject) => {
      const timeout = setTimeout(() => {
        cleanup();
        reject(new Error('Font retry fixture timed out'));
      }, 45000);
      const receive = (event: MessageEvent) => {
        if (
          event.origin !== location.origin ||
          event.source !== iframe.contentWindow ||
          event.data?.type !== 'font-retry-result'
        )
          return;
        cleanup();
        event.data.error ? reject(new Error(event.data.error)) : resolve(event.data.results);
      };
      const cleanup = () => {
        clearTimeout(timeout);
        window.removeEventListener('message', receive);
      };
      window.addEventListener('message', receive);
      document.body.append(iframe);
    });
  } finally {
    iframe.remove();
  }
}
async function cacheCases(still: any, audio: any) {
  report.phase = 'cache-graph';
  await publish();
  const p = project('cache-graph'),
    item = frameClip(p, still, 0, 1, 0),
    sound = frameClip(p, audio, 0, 1, 0);
  item.clip.effects = [
    { id: 'blur-cache', templateId: 'blur', enabled: true, parameters: { radius: 8 } },
    {
      id: 'glow-cache',
      templateId: 'glow',
      enabled: true,
      parameters: { radius: 10, strength: 0.5 }
    },
    {
      id: 'lut-cache',
      templateId: 'lut',
      enabled: true,
      parameters: { preset: 'warm', amount: 0.5 }
    }
  ];
  const text = addText(p, { content: '缓存 Cache', length: ticks(1), fontSize: 40 });
  const session = await client.createSession(p);
  sessions.push(session.id);
  const url = (id: string) => {
    const value = new URL(client.mediaUrl(session.id, id), location.href),
      a = p.assets.find((a) => a.id === id);
    if (a?.sourceIdentity) value.searchParams.set('source', a.sourceIdentity);
    return value.href;
  };
  const scene = new SceneRenderer(new OffscreenCanvas(640, 360), url);
  const record = async (name: string, values: any, passed: boolean) => {
    results.push({ name, ...values, passed });
    await publish();
    if (!passed) throw new Error(`${name} failed`);
  };
  try {
    const cold = await scene.render(p, 0, 640, 360),
      repeat = await scene.render(p, 0, 640, 360);
    await record(
      'cache-final-reuse',
      { cold, repeat },
      repeat.gpuPasses === 0 && repeat.uploadCount === 0 && repeat.compositeHits > 0
    );
    sound.clip.audio.gainLinear = 0.25;
    const audioOnly = await scene.render(p, 0, 640, 360);
    await record(
      'cache-audio-edit',
      { audioOnly },
      audioOnly.gpuPasses === 0 && audioOnly.graphBuilds === cold.graphBuilds
    );
    item.clip.visual.positionX = 23;
    item.clip.visual.positionY = -7;
    const moved = await scene.render(p, 0, 640, 360);
    const movedPixels = await scene.readPixels();
    const fresh = new SceneRenderer(new OffscreenCanvas(640, 360), url);
    let coldPixels: Uint8Array;
    try {
      await fresh.render(p, 0, 640, 360);
      coldPixels = await fresh.readPixels();
    } finally {
      fresh.dispose();
    }
    let differingBytes = 0;
    for (let i = 0; i < movedPixels.length; i++)
      if (movedPixels[i] !== coldPixels![i]) differingBytes++;
    await record(
      'cache-move-effects',
      { moved, differingBytes },
      moved.effectHits >= 5 &&
        moved.textLayoutCount === 0 &&
        moved.graphBuilds === cold.graphBuilds &&
        differingBytes === 0
    );
    item.clip.effects[1].parameters.strength = 0.8;
    const parameter = await scene.render(p, 0, 640, 360);
    await record(
      'cache-effect-parameter',
      { parameter },
      parameter.effectHits >= 3 &&
        parameter.gpuPasses > 0 &&
        parameter.graphBuilds === cold.graphBuilds
    );
    text.clip.visual.positionX = 50;
    const layout = await scene.render(p, 0, 640, 360);
    await record(
      'cache-text-layout',
      { layout },
      layout.textLayoutCount === 0 && layout.effectHits >= 5
    );
    p.assets.find((a) => a.id === still.id)!.sourceIdentity = 'qa-replaced-source';
    const replaced = await scene.render(p, 0, 640, 360);
    await record(
      'cache-source-identity',
      { replaced },
      replaced.uploadCount >= 1 && replaced.gpuPasses > 0
    );
    const before = await scene.readPixels(),
      cancel = new AbortController();
    cancel.abort();
    let cancellation = '';
    try {
      await scene.render(p, ticks(0.2), 640, 360, cancel.signal);
    } catch (error) {
      cancellation = String(error);
    }
    const after = await scene.readPixels();
    await record(
      'cache-cancel-preserves-final',
      { cancellation },
      cancellation.includes('AbortError') && before.every((value, i) => value === after[i])
    );
  } finally {
    scene.dispose();
  }
  const limited = new SceneRenderer(new OffscreenCanvas(640, 360), url, {
    textureBudgetBytes: 8 * 1048576
  });
  try {
    let evictions = 0,
      last: any;
    for (let i = 0; i < 8; i++) {
      const q = project('cache-lru-' + i);
      frameClip(q, still, 0, 1, 0);
      last = await limited.render(q, 0, 640, 360);
      evictions += last.evictions;
    }
    await record(
      'cache-budget-lru',
      { evictions, last },
      evictions > 0 && last.resourceBytes <= last.textureBudget
    );
    await limited.setTextureBudget(512 * 1024);
    const q = project('cache-pressure');
    frameClip(q, still, 0, 1, 0);
    const adaptive = new AdaptivePreviewQuality(),
      attempts: any[] = [];
    let recovered: any;
    for (let attempt = 0; attempt < 5; attempt++) {
      const extent = adaptive.extent({ width: 640, height: 360 });
      try {
        recovered = await limited.render(q, 0, extent.width, extent.height);
        attempts.push({ extent, passed: true });
        break;
      } catch (error) {
        attempts.push({ extent, error: String(error), code: (error as any).code });
        if ((error as any).code !== 'RENDER_BUDGET' || !adaptive.budgetPressure()) throw error;
      }
    }
    await record(
      'cache-budget-quality',
      { attempts, recovered },
      attempts.length > 1 && !!recovered && recovered.resourceBytes <= recovered.textureBudget
    );
    adaptive.paused();
    await limited.setTextureBudget(8 * 1048576);
    const detail = await limited.render(q, 0, 640, 360);
    await record(
      'cache-paused-detail',
      { scale: adaptive.scale, detail },
      adaptive.scale === 1 && detail.width === 640 && detail.height === 360
    );
    const committed = await limited.readPixels();
    await limited.setTextureBudget(512 * 1024);
    let failedResize = '';
    try {
      await limited.render(q, 0, 320, 180);
    } catch (error) {
      failedResize = (error as any).code;
    }
    const displayedSize = [limited.canvas.width, limited.canvas.height];
    await limited.setTextureBudget(8 * 1048576);
    const restored = await limited.render(q, 0, 640, 360),
      restoredPixels = await limited.readPixels();
    await record(
      'cache-failed-resize-preserves-display',
      { failedResize, displayedSize, restored },
      failedResize === 'RENDER_BUDGET' &&
        displayedSize[0] === 640 &&
        displayedSize[1] === 360 &&
        restored.gpuPasses > 0 &&
        committed.every((value, i) => value === restoredPixels[i])
    );
  } finally {
    limited.dispose();
  }
}
async function one(
  name: string,
  p: Project,
  time: number,
  format: 'mp4' | 'webm' = 'mp4',
  preferredEncoding: EncodingMode | 'auto' = 'auto'
) {
  controller.signal.throwIfAborted();
  const started = performance.now();
  report.phase = name;
  await publish();
  const snapshot = await client.createSession(p);
  sessions.push(snapshot.id);
  const events = new EventSource(client.eventsUrl(snapshot.id));
  await new Promise<void>((resolve, reject) => {
    events.onopen = () => resolve();
    events.onerror = () => reject(new Error('测试会话SSE无法连接'));
  });
  const scene = new SceneRenderer(preview, (id) => client.mediaUrl(snapshot.id, id));
  try {
    const render = await scene.render(p, ticks(time), 640, 360, controller.signal),
      reference = await scene.readPixels();
    if (render.backend !== 'webgpu') throw new Error('此矩阵必须在真实WebGPU运行');
    const pending = client.renderVideo(snapshot.id, snapshot.version, destination, format);
    pending.catch(() => {});
    let job: any;
    for (let n = 0; n < 100; n++) {
      controller.signal.throwIfAborted();
      job = await client.renderStatus(snapshot.id);
      if (job?.id || job?.jobId) break;
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    await renderTemplateExport(
      client,
      snapshot.id,
      job.id || job.jobId,
      controller.signal,
      () => {},
      { preferredEncoding }
    );
    const receipt = await pending;
    const imported = await client.importMedia(receipt.path),
      output = imported.asset || imported;
    const decodeProject = createProject('QA decoded');
    addAsset(decodeProject, output);
    const playback = await client.createSession(decodeProject);
    sessions.push(playback.id);
    const frame = await sharedMediaEngine.videoFrame(client.mediaUrl(playback.id, output.id), time);
    const frameTimestamp = frame.timestamp;
    const ctx = decoded.getContext('2d')!;
    ctx.clearRect(0, 0, 640, 360);
    ctx.drawImage(frame.frame, 0, 0, 640, 360);
    frame.close();
    const diff = difference(reference, ctx.getImageData(0, 0, 640, 360).data);
    let audioCheck: any;
    if (preferredEncoding !== 'auto') {
      const mediaUrl = client.mediaUrl(playback.id, output.id),
        metadata = await sharedMediaEngine.probe(mediaUrl),
        blocks = await sharedMediaEngine.audioWindow(mediaUrl, 0, 0.6);
      const l = blocks[0].data[0],
        r = blocks[0].data[1];
      let ls = 0,
        rs = 0,
        dot = 0;
      const n = Math.min(28800, l.length, r.length);
      for (let i = 0; i < n; i++) {
        ls += l[i] * l[i];
        rs += r[i] * r[i];
        dot += l[i] * r[i];
      }
      audioCheck = {
        channels: metadata.channels,
        sampleRate: metadata.sampleRate,
        duration: metadata.duration,
        samples: n,
        leftRms: Math.sqrt(ls / n),
        rightRms: Math.sqrt(rs / n),
        correlation: dot / Math.sqrt(ls * rs)
      };
      audioCheck.passed =
        audioCheck.channels === 2 &&
        n === 28800 &&
        Math.abs(metadata.duration - 0.6) < 0.05 &&
        audioCheck.leftRms > 0.01 &&
        audioCheck.correlation < -0.98;
    }
    const result = {
      name,
      format,
      path: receipt.path,
      encoding: receipt.encoding,
      frames: receipt.frames,
      time,
      decodedTimestamp: frameTimestamp,
      audio: audioCheck,
      ...diff,
      previewMs: render.frameMs,
      totalMs: performance.now() - started,
      passed:
        diff.mae < 8 &&
        diff.p99 < 100 &&
        diff.nonBlack > 50 &&
        (!audioCheck || audioCheck.passed) &&
        Math.abs(frameTimestamp - time) < 0.034
    };
    results.push(result);
    await image(`${name}-preview`, preview);
    await image(`${name}-export`, decoded);
    await publish();
    if (!result.passed)
      throw new Error(`${name} 像素对比未过：MAE=${diff.mae.toFixed(2)} P99=${diff.p99}`);
  } finally {
    events.close();
    scene.dispose();
  }
}
async function run() {
  const button = document.querySelector('#run') as HTMLButtonElement;
  button.disabled = true;
  controller = new AbortController();
  report.started = new Date().toISOString();
  report.phase = 'importing';
  results.length = 0;
  logs.length = 0;
  await publish();
  try {
    report.phase = 'font-load-retry';
    await publish();
    results.push(...(await isolatedFontRetry()));
    await publish();
    await client.connect();
    const imports = await Promise.all(
      ['timecode.mp4', 'stereo.wav', 'still.png'].map((n) => client.importMedia(`${fixtures}/${n}`))
    );
    const [video, audio, still] = imports.map((v) => v.asset || v);
    await cacheCases(still, audio);
    if (options.get('case') === 'cache') {
      report.phase = 'complete';
      report.completed = new Date().toISOString();
      await publish();
      return;
    }
    for (const [id, parameters] of [
      ['blur', { radius: 8 }],
      ['glow', { radius: 12, strength: 0.6 }],
      ['lut', { preset: 'warm', amount: 0.8 }]
    ] as const) {
      const p = project(id);
      const item = frameClip(p, video);
      item.clip.effects = [{ id: `effect-${id}`, templateId: id, enabled: true, parameters }];
      await one(id, p, 0.2);
    }
    for (const id of ['dissolve', 'fade', 'wipe', 'slide'] as const) {
      const p = project(id),
        from = frameClip(p, video, 0, 0.4, 0.4),
        to = frameClip(p, video, 0.4, 0.4, 2, p.timeline.tracks[0].id);
      p.timeline.transitions = [
        {
          id: `transition-${id}`,
          fromItemId: from.id,
          toItemId: to.id,
          templateId: id,
          duration: ticks(0.4),
          parameters:
            id === 'wipe' || id === 'slide'
              ? { direction: 'right' }
              : id === 'fade'
                ? { color: '#000000' }
                : {}
        }
      ];
      await one(id, p, id === 'fade' ? 0.3 : 0.4);
    }
    const coveredPackages = VISUAL_PACKAGES.map((entry) => ({
      id: entry.id,
      kind: entry.kind,
      passed: results.some((value) => value.name === entry.id && value.passed)
    }));
    report.visualPackageCoverage = coveredPackages;
    if (coveredPackages.some((entry) => !entry.passed))
      throw new Error('The admitted visual catalogue contains an untested execution path');
    for (const recipe of recipes) {
      const p = project(recipe.id),
        item = addText(p, {
          template: { id: recipe.id, version: 1 },
          content: recipe.text,
          length: ticks(0.6)
        });
      item.clip.source = {
        begin: Math.max(0, ticks(recipe.timeUs / 1e6 - 0.3)),
        end: Math.max(0, ticks(recipe.timeUs / 1e6 - 0.3)) + ticks(0.6)
      };
      await one(recipe.id, p, 0.3);
    }
    for (const mode of ['normal', 'multiply', 'screen', 'overlay', 'darken', 'lighten'] as const) {
      const p = project(`blend-${mode}`),
        top = frameClip(p, still, 0, 0.6, 0),
        bottom = frameClip(p, video);
      top.clip.visual.blendMode = mode;
      top.clip.visual.opacity = 0.65;
      top.clip.visual.scaleX = 0.7;
      top.clip.visual.scaleY = 0.7;
      bottom.clip.audio.muted = true;
      await one(`blend-${mode}`, p, 0.2);
    }
    const sound = project('stereo-webm');
    frameClip(sound, still, 0, 0.6, 0);
    frameClip(sound, audio, 0, 0.6, 0);
    await one('stereo-webm', sound, 0.2, 'webm');
    for (const encoding of ['video-with-pcm', 'frames-with-pcm'] as const) {
      const p = project(encoding),
        item = frameClip(p, video);
      item.clip.audio.muted = true;
      item.clip.effects = [
        {
          id: 'fx-lut',
          templateId: 'lut',
          enabled: true,
          parameters: { preset: 'cool', amount: 0.4 }
        }
      ];
      frameClip(p, audio, 0, 0.6, 0);
      await one(encoding, p, 0.2, 'mp4', encoding);
    }
    report.phase = 'upload-cache';
    await publish();
    const cacheProject = project('cache'),
      cacheItem = frameClip(cacheProject, video),
      cacheSession = await client.createSession(cacheProject),
      cacheScene = new SceneRenderer(new OffscreenCanvas(640, 360), (id) =>
        client.mediaUrl(cacheSession.id, id)
      );
    const cold = await cacheScene.render(cacheProject, ticks(0.2), 640, 360);
    cacheItem.clip.visual.positionX = 30;
    const moved = await cacheScene.render(cacheProject, ticks(0.2), 640, 360),
      advanced = await cacheScene.render(cacheProject, ticks(0.3), 640, 360);
    cacheScene.dispose();
    const cacheResult = {
      name: 'upload-cache',
      cold: { uploads: cold.uploadCount, hits: cold.uploadCacheHits },
      moved: { uploads: moved.uploadCount, hits: moved.uploadCacheHits },
      advanced: { uploads: advanced.uploadCount, hits: advanced.uploadCacheHits },
      textureBytes: advanced.textureBytes,
      passed:
        cold.uploadCount === 1 &&
        moved.uploadCount === 0 &&
        moved.uploadCacheHits === 1 &&
        advanced.uploadCount === 1
    };
    results.push(cacheResult);
    if (!cacheResult.passed) throw new Error('GPU upload cache did not invalidate by decoded PTS');
    report.phase = 'superseded-frame';
    await publish();
    const staleWorker = new Worker(new URL('../packages/render/worker.ts', import.meta.url), {
        type: 'module'
      }),
      staleCanvas = new OffscreenCanvas(640, 360),
      observed: any[] = [];
    const staleUrls = Object.fromEntries(
      cacheProject.assets.map((a) => [a.id, client.mediaUrl(cacheSession.id, a.id)])
    );
    await new Promise<void>((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('Stale frame worker timed out')), 10000);
      staleWorker.onerror = (event) => {
        clearTimeout(timeout);
        reject(new Error(event.message));
      };
      staleWorker.onmessage = ({ data }) => {
        observed.push({ type: data.type, generation: data.generation, time: data.report?.time });
        if (data.type === 'ready') {
          staleWorker.postMessage({
            type: 'render',
            id: 1,
            generation: 1,
            project: cacheProject,
            time: ticks(0.2),
            width: 640,
            height: 360,
            urls: staleUrls
          });
          staleWorker.postMessage({ type: 'invalidate', generation: 2 });
        } else if (data.type === 'cancelled') {
          staleWorker.postMessage({
            type: 'render',
            id: 2,
            generation: 2,
            time: ticks(0.3),
            width: 640,
            height: 360,
            urls: staleUrls
          });
        } else if (data.type === 'frame') {
          clearTimeout(timeout);
          data.generation === 2 ? resolve() : reject(new Error('Superseded frame was presented'));
        } else if (data.type === 'error') {
          clearTimeout(timeout);
          reject(new Error(data.error));
        }
      };
      staleWorker.postMessage({ type: 'init', canvas: staleCanvas, urls: staleUrls }, [
        staleCanvas
      ]);
    }).finally(() => staleWorker.terminate());
    results.push({
      name: 'superseded-frame',
      observed,
      passed:
        observed.some((x) => x.type === 'cancelled' && x.generation === 1) &&
        observed.some((x) => x.type === 'frame' && x.generation === 2)
    });
    report.phase = 'cancel-export';
    await publish();
    const cancelProject = project('cancel-export');
    addText(cancelProject, {
      template: { id: 'cube', version: 1 },
      content: '取消测试',
      length: ticks(60)
    });
    const cancelSession = await client.createSession(cancelProject),
      cancelEvents = new EventSource(client.eventsUrl(cancelSession.id));
    await new Promise<void>((resolve, reject) => {
      cancelEvents.onopen = () => resolve();
      cancelEvents.onerror = () => reject(new Error('Cancel SSE failed'));
    });
    let progressBeforeCancel = 0;
    cancelEvents.addEventListener('render-progress', (event) => {
      progressBeforeCancel = Math.max(
        progressBeforeCancel,
        JSON.parse((event as MessageEvent).data).completed || 0
      );
      if (progressBeforeCancel >= 1) cancellation.abort();
    });
    const cancellation = new AbortController(),
      cancelReceipt = client.renderVideo(cancelSession.id, cancelSession.version, destination);
    cancelReceipt.catch(() => {});
    let cancelJob: any;
    for (let n = 0; n < 100; n++) {
      cancelJob = await client.renderStatus(cancelSession.id);
      if (cancelJob.id) break;
      await new Promise((resolve) => setTimeout(resolve, 20));
    }
    let cancelError = '',
      receiptError = '';
    try {
      await renderTemplateExport(
        client,
        cancelSession.id,
        cancelJob.id,
        cancellation.signal,
        () => {
          setTimeout(() => cancellation.abort(), 5000);
        },
        { preferredEncoding: 'frames-with-pcm' }
      );
    } catch (error) {
      cancelError = String(error);
    }
    try {
      await cancelReceipt;
    } catch (error) {
      receiptError = String(error);
    } finally {
      cancelEvents.close();
    }
    const cancelStatus = await client.renderStatus(cancelSession.id);
    results.push({
      name: 'cancel-export',
      progressBeforeCancel,
      error: cancelError,
      serverError: receiptError,
      status: cancelStatus.phase,
      passed:
        !!cancelError &&
        !!receiptError &&
        cancelStatus.phase === 'error' &&
        progressBeforeCancel >= 1
    });
    if (!cancelError || !receiptError || cancelStatus.phase !== 'error' || progressBeforeCancel < 1)
      throw new Error('Cancellation was not acknowledged');
    report.phase = 'device-loss';
    await publish();
    const gpu = await sharedGpu(),
      lossScene = new SceneRenderer(new OffscreenCanvas(640, 360), () => {
        throw new Error('unused');
      });
    await lossScene.ready;
    gpu.device.destroy();
    await gpu.device.lost;
    let lossError = '';
    try {
      await lossScene.render(project('lost'), 0, 640, 360);
    } catch (error) {
      lossError = String(error);
    } finally {
      lossScene.dispose();
    }
    results.push({ name: 'device-loss', error: lossError, passed: !!lossError });
    if (!lossError) throw new Error('Device loss未拒绝渲染');
    const recovered = new SceneRenderer(new OffscreenCanvas(640, 360), () => {
      throw new Error('unused');
    });
    const recoveredFrame = await recovered.render(project('recovered'), 0, 640, 360);
    recovered.dispose();
    results.push({
      name: 'device-recovery',
      backend: recoveredFrame.backend,
      passed: recoveredFrame.backend === 'webgpu'
    });
    report.phase = 'complete';
    report.completed = new Date().toISOString();
    await publish();
  } catch (error) {
    report.phase = controller.signal.aborted ? 'cancelled' : 'failed';
    report.error = error instanceof Error ? error.stack : String(error);
    await publish();
  } finally {
    button.disabled = false;
  }
}
if (options.get('case') === 'font-retry') {
  void fontRetryCases().then(
    (results) => parent.postMessage({ type: 'font-retry-result', results }, location.origin),
    (error) =>
      parent.postMessage({ type: 'font-retry-result', error: String(error) }, location.origin)
  );
} else {
  document.querySelector('#run')!.addEventListener('click', () => void run());
  document.querySelector('#stop')!.addEventListener('click', () => controller.abort());
  void publish();
}
