import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { createProject, addAsset, addHtmlClip, addText, ticks } from '../packages/core/project.mjs';

const load = async (entry, plugins = []) => {
  const bundled = await build({
    entryPoints: [entry],
    bundle: true,
    platform: 'node',
    format: 'esm',
    write: false,
    logLevel: 'silent',
    plugins
  });
  return import(
    'data:text/javascript;base64,' + Buffer.from(bundled.outputFiles[0].text).toString('base64')
  );
};
const { ResidentSourceCache, sourceFrameKey, sourceRasterBounds } = await load(
  'packages/render/source-cache.ts'
);
const { SceneRenderer } = await load('packages/render/renderer.ts', [
  {
    name: 'resident-source-fixtures',
    setup(api) {
      api.onResolve(
        { filter: /^\.\/text$|^\.\.\/media\/browser$|^\.\/gpu\.mjs$|^\.\/html$/ },
        ({ path }) => ({ path, namespace: 'fixture' })
      );
      api.onLoad({ filter: /.*/, namespace: 'fixture' }, ({ path }) => ({
        contents:
          path === './text'
            ? 'export const createTemplatePlayer=(...args)=>globalThis.__residentSources.player(...args); export const ensureCanvasFonts=async()=>"Fixture";'
            : path === './gpu.mjs'
              ? 'export const createGpuCompositor=async()=>globalThis.__residentSources.gpu;'
              : path === './html'
                ? 'export const htmlSourceTime=(h,t)=>Math.max(0,Math.min(Math.round(t),h.duration-1)); export class HtmlFrameClient { retain(){} frame(...args){return globalThis.__residentSources.html(...args)} dispose(){} }'
                : 'export const sharedMediaEngine={image:(...args)=>globalThis.__residentSources.image(...args), createVideoReader:(...args)=>globalThis.__residentSources.reader(...args)};'
      }));
    }
  }
]);

const metadata = (id) => ({ inputKey: id, uploadIdentity: id, width: 1920, height: 1080 });
test('Native raster bounds keep an exact texel halo and reject uncertain or full-frame crops', () => {
  assert.deepEqual(sourceRasterBounds(1920, 1080, { x: 321, y: 211, width: 630, height: 234 }), {
    x: 320,
    y: 210,
    width: 632,
    height: 236
  });
  assert.deepEqual(sourceRasterBounds(1920, 1080, { x: 0, y: 0, width: 10, height: 20 }), {
    x: 0,
    y: 0,
    width: 11,
    height: 21
  });
  assert.deepEqual(sourceRasterBounds(1920, 1080, { x: 1910, y: 1060, width: 10, height: 20 }), {
    x: 1909,
    y: 1059,
    width: 11,
    height: 21
  });
  for (const bounds of [
    undefined,
    { x: 0, y: 0, width: 1920, height: 1080 },
    { x: -1, y: 0, width: 10, height: 20 },
    { x: 0.5, y: 0, width: 10, height: 20 },
    { x: 0, y: 0, width: 0, height: 20 },
    { x: 1919, y: 0, width: 2, height: 20 }
  ])
    assert.equal(sourceRasterBounds(1920, 1080, bounds), undefined);
});
test('Metadata LRU is bounded, removes evicted textures, and cannot own bitmap lifetimes', () => {
  const cache = new ResidentSourceCache(2, 10);
  cache.set('first', metadata('first'));
  cache.set('next', metadata('next'));
  assert.equal(cache.get('first', () => true).inputKey, 'first');
  cache.set('third', metadata('third'));
  assert.equal(cache.peek('next'), undefined, 'LRU touch retains the revisited backward frame');
  assert.equal(cache.size, 2);
  assert.equal(
    cache.get('first', () => false),
    undefined,
    'CPU metadata never proves GPU residency'
  );
  assert.equal(cache.peek('first'), undefined);
  cache.set('an oversized key', metadata('large'));
  assert.equal(cache.size, 1, 'Source-key memory also has a hard bound');
  cache.clear();
  assert.equal(cache.size, 0);
});

test('Source keys preserve exact ticks, native extent and authored content while excluding placement/effects/audio', () => {
  const project = createProject(),
    item = addHtmlClip(project, {
      html: {
        html: '<div>Title</div>',
        width: 1920,
        height: 1080,
        duration: ticks(5),
        transparent: true
      }
    });
  const layer = { item, sourceTime: ticks(0.2) };
  const key = () => sourceFrameKey(layer, 960, 540, 1920, 1080, () => '');
  const first = key();
  item.clip.visual.positionX = 50;
  item.clip.effects = [{ id: 'fx', templateId: 'blur', parameters: { radius: 3 } }];
  item.clip.audio.gainLinear = 0.2;
  assert.equal(key(), first);
  layer.sourceTime++;
  assert.notEqual(key(), first, 'Even one 120000 Hz tick must invalidate');
  layer.sourceTime--;
  item.clip.html.variables = { title: 'Changed' };
  assert.notEqual(key(), first);
  delete item.clip.html.variables;
  item.clip.html.width = 1280;
  assert.notEqual(key(), first);
  item.clip.audio.gainLinear = 1;
  item.clip.effects = [];
  const text = addText(project, {
    content: 'Flower',
    length: ticks(5),
    template: { id: 'pattern-flower', version: 1 }
  });
  const native = { item: text, sourceTime: 1 };
  const nativeKey = sourceFrameKey(native, 960, 540, 1920, 1080, () => '');
  assert.notEqual(
    sourceFrameKey(native, 480, 270, 1920, 1080, () => ''),
    nativeKey
  );
  text.clip.text.template.recipe = { base: 'flower-style-03', backdrop: 'bubble-nine-slice' };
  assert.notEqual(
    sourceFrameKey(native, 960, 540, 1920, 1080, () => ''),
    nativeKey
  );
  const media = {
    item: { clip: {} },
    asset: { id: 'original', kind: 'video', width: 640, height: 360 },
    sourceTime: 1234
  };
  assert.equal(
    sourceFrameKey(media, 960, 540, 1920, 1080, () => '/original'),
    sourceFrameKey(media, 1920, 1080, 1920, 1080, () => '/original'),
    'Media source pixels are independent of preview/compositor dimensions'
  );
});

class Canvas {
  constructor(width, height) {
    this.width = width;
    this.height = height;
  }
  getContext() {
    return {};
  }
}
function fixture(t) {
  const calls = { html: 0, native: 0, video: 0, close: 0 },
    textures = new Map(),
    pins = new Set(),
    draws = [];
  const gpu = {
    uploads: 0,
    hits: 0,
    retainInputs(entries, preferNative = false) {
      this.preferNative = preferNative;
      if (!preferNative) for (const texture of textures.values()) texture.priority = 0;
      pins.clear();
      for (const entry of entries) {
        const texture = textures.get(entry.inputKey);
        if (
          texture?.identity === entry.uploadIdentity &&
          texture.width === (entry.rasterBounds?.width || entry.width) &&
          texture.height === (entry.rasterBounds?.height || entry.height) &&
          JSON.stringify(texture.rasterBounds) === JSON.stringify(entry.rasterBounds)
        )
          pins.add(entry.inputKey);
      }
      return new Set(pins);
    },
    begin() {
      draws.length = 0;
      return {};
    },
    upload(frame, key, identity, rasterBounds, priority = 0) {
      const texture = {
        key,
        identity,
        width: rasterBounds?.width || frame.width,
        height: rasterBounds?.height || frame.height,
        rasterBounds,
        priority,
        sample: frame.sample
      };
      textures.set(key, texture);
      pins.add(key);
      this.uploads++;
      return texture;
    },
    reuseInput(entry) {
      assert.ok(
        pins.has(entry.inputKey),
        'Every reused texture must be protected before any new layer is allocated'
      );
      assert.equal(textures.get(entry.inputKey)?.identity, entry.uploadIdentity);
      this.hits++;
      return textures.get(entry.inputKey);
    },
    layer(input, geometry, effects, key) {
      draws.push({ sample: input.sample, key, geometry });
      return input;
    },
    blend(base) {
      return base;
    },
    finish() {},
    abort() {
      pins.clear();
    },
    reuseFinal() {
      return false;
    },
    stats() {
      return {};
    },
    dispose() {}
  };
  const lease = (width, height, sample) => ({
    frame: { width, height, sample },
    width,
    height,
    timestamp: sample,
    close() {
      calls.close++;
    }
  });
  const harness = {
    gpu,
    html: async (id, html, time) => {
      calls.html++;
      return {
        ...lease(html.width, html.height, `html:${time}`),
        tickTime: time,
        transparent: html.transparent,
        identity: JSON.stringify([html, time])
      };
    },
    player: async (canvas) => ({
      durationUs: 5000000,
      setText() {},
      resize() {
        return false;
      },
      dispose() {},
      render(time) {
        calls.native++;
        canvas.sample = `native:${time}`;
        return {
          controlBounds: { x: 10, y: 20, width: 200, height: 50 },
          rasterBounds: { x: 10, y: 20, width: 200, height: 50 }
        };
      }
    }),
    image: async (url, options) => {
      assert.deepEqual(
        Object.keys(options),
        ['signal'],
        'Images retain their original source extent'
      );
      return lease(640, 360, 'image:original');
    },
    reader: async (url, options) => {
      assert.deepEqual(
        Object.keys(options),
        ['signal'],
        'Decoder dimensions cannot follow preview size'
      );
      return {
        frameAt: async (time) => {
          calls.video++;
          return lease(1920, 1080, `video:${time}`);
        },
        async close() {}
      };
    }
  };
  for (const [name, value] of Object.entries({
    OffscreenCanvas: Canvas,
    __residentSources: harness
  })) {
    const descriptor = Object.getOwnPropertyDescriptor(globalThis, name);
    Object.defineProperty(globalThis, name, { value, writable: true, configurable: true });
    t.after(() =>
      descriptor ? Object.defineProperty(globalThis, name, descriptor) : delete globalThis[name]
    );
  }
  const scene = new SceneRenderer(new Canvas(960, 540), (id) => `/${id}`);
  t.after(() => scene.dispose());
  const project = createProject();
  const video = addAsset(project, {
    id: 'video',
    name: 'Video',
    path: '/video.mp4',
    kind: 'video',
    duration: ticks(5),
    size: 1,
    width: 1920,
    height: 1080,
    hasAudio: false
  });
  const native = addText(project, {
    content: 'Flower',
    length: ticks(5),
    template: { id: 'pattern-flower', version: 1 }
  });
  const html = addHtmlClip(project, {
    html: {
      html: '<div>HTML</div>',
      width: 1920,
      height: 1080,
      duration: ticks(5),
      transparent: true
    }
  });
  return { scene, project, video, native, html, calls, gpu, textures, draws };
}

test('Backward mixed-source seeks reuse exact resident HTML/native/video pixels and keep report/bounds order', async (t) => {
  const f = fixture(t),
    time = ticks(0.2);
  const first = await f.scene.render(f.project, time, 960, 540);
  await f.scene.render(f.project, ticks(2), 960, 540);
  const backward = await f.scene.render(f.project, time, 960, 540);
  assert.deepEqual(f.calls, { html: 2, native: 2, video: 2, close: 4 });
  assert.equal(f.gpu.hits, 3);
  assert.ok(backward.media.every((entry) => entry.sourceCacheHit));
  assert.deepEqual(
    backward.media.map((entry) => entry.itemId),
    first.media.map((entry) => entry.itemId)
  );
  assert.deepEqual(backward.bounds, first.bounds);
  const source = [...f.textures.values()].find((entry) => entry.sample.startsWith('native:'));
  const nativeDraw = f.draws.find((entry) => entry.sample.startsWith('native:'));
  assert.deepEqual(
    [source.width, source.height],
    [202, 52],
    'Historical native texture stores only finished pixels and the halo'
  );
  assert.deepEqual([nativeDraw.geometry.sourceWidth, nativeDraw.geometry.sourceHeight], [960, 540]);
  assert.deepEqual(nativeDraw.geometry.rasterBounds, { x: 9, y: 19, width: 202, height: 52 });
  assert.equal(
    nativeDraw.geometry.matrix[0],
    960,
    'ROI residency preserves the full authored placement'
  );
  assert.equal(source.priority, 1);
  assert.equal(f.gpu.preferNative, true);
  assert.equal(backward.media.find((entry) => entry.itemId === f.html.id).htmlWidth, 1920);
  assert.equal(backward.width, 960);
  f.html.clip.visual.positionX += 10;
  f.html.clip.effects = [];
  f.native.clip.audio.gainLinear = 0.2;
  const moved = await f.scene.render(f.project, time, 960, 540);
  assert.deepEqual(f.calls, { html: 2, native: 2, video: 2, close: 4 });
  assert.ok(moved.media.every((entry) => entry.sourceCacheHit));
  assert.notDeepEqual(
    moved.bounds,
    first.bounds,
    'A source hit still reevaluates placement geometry'
  );
});

test('Removing the active native clip resets source priority without changing logical media geometry', async (t) => {
  const f = fixture(t);
  await f.scene.render(f.project, ticks(0.2), 960, 540);
  assert.equal(f.gpu.preferNative, true);
  assert.ok([...f.textures.values()].some((texture) => texture.priority === 1));
  f.native.enabled = false;
  const report = await f.scene.render(f.project, ticks(0.3), 960, 540);
  assert.equal(f.gpu.preferNative, false);
  assert.ok([...f.textures.values()].every((texture) => texture.priority === 0));
  assert.equal(
    report.media.some((entry) => entry.itemId === f.native.id),
    false
  );
  assert.equal(f.draws.find((entry) => entry.sample.startsWith('html:')).geometry.matrix[0], 960);
});

test('GPU eviction, stale signature, authored edits and extent changes refresh only affected sources', async (t) => {
  const f = fixture(t),
    time = ticks(0.2);
  await f.scene.render(f.project, time, 960, 540);
  const htmlTexture = [...f.textures.values()].find((entry) => entry.sample.startsWith('html:'));
  f.textures.delete(htmlTexture.key);
  let report = await f.scene.render(f.project, time, 960, 540);
  assert.equal(f.calls.html, 2);
  assert.equal(f.calls.native, 1);
  assert.equal(f.calls.video, 1);
  assert.equal(report.media.find((entry) => entry.itemId === f.html.id).sourceCacheHit, false);
  const videoTexture = [...f.textures.values()].find((entry) => entry.sample.startsWith('video:'));
  videoTexture.identity = 'stale';
  await f.scene.render(f.project, time, 960, 540);
  assert.equal(f.calls.video, 2, 'Metadata cannot reuse a texture with another source signature');
  f.html.clip.html.variables = { title: 'Edited' };
  f.native.clip.text.content = 'Edited';
  report = await f.scene.render(f.project, time, 960, 540);
  assert.equal(f.calls.html, 3);
  assert.equal(f.calls.native, 2);
  assert.equal(f.calls.video, 2);
  assert.equal(report.media.filter((entry) => entry.sourceCacheHit).length, 1);
  await f.scene.render(f.project, time, 1920, 1080);
  assert.equal(f.calls.html, 3, 'HTML remains its authored 1920 raster at every preview extent');
  assert.equal(f.calls.native, 3);
  assert.equal(
    f.calls.video,
    2,
    'Changing only composition extent reuses the original video texture'
  );
});

test('A small source image fills the preview from its original pixels and reuses them across output sizes', async (t) => {
  const f = fixture(t);
  addAsset(f.project, {
    id: 'image',
    name: 'Image',
    path: '/image.png',
    kind: 'image',
    duration: ticks(5),
    size: 1,
    width: 640,
    height: 360,
    hasAudio: false
  });
  await f.scene.render(f.project, ticks(0.2), 960, 540);
  const source = [...f.textures.values()].find((entry) => entry.sample === 'image:original');
  assert.deepEqual(
    [source.width, source.height],
    [640, 360],
    'The 640px source is never first reduced to 320px'
  );
  assert.equal(f.draws.find((entry) => entry.sample === 'image:original').geometry.matrix[0], 960);
  const report = await f.scene.render(f.project, ticks(0.2), 1920, 1080);
  assert.equal(report.media.find((entry) => entry.kind === 'image').sourceCacheHit, true);
  assert.equal(
    [...f.textures.values()].filter((entry) => entry.sample === 'image:original').length,
    1
  );
});
