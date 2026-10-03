import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { createProject, addAsset, addText, ticks } from '../packages/core/project.mjs';

const bundled = await build({
  entryPoints: ['packages/render/renderer.ts'],
  bundle: true,
  platform: 'node',
  format: 'esm',
  write: false,
  logLevel: 'silent',
  plugins: [
    {
      name: 'source-lifecycle-fixtures',
      setup(api) {
        api.onResolve(
          { filter: /^\.\/text$|^\.\.\/media\/browser$|^\.\/gpu\.mjs$|^\.\/text-prefetch$/ },
          ({ path }) => ({ path, namespace: 'fixture' })
        );
        api.onLoad({ filter: /.*/, namespace: 'fixture' }, ({ path }) => ({
          contents:
            path === './text'
              ? `export const createTemplatePlayer=(...args)=>globalThis.__renderSources.player(...args); export const ensureCanvasFonts=async()=>"Fixture";`
              : path === './text-prefetch'
                ? `export class TextWorkerUnavailableError extends Error {}
                   export class TextPrefetchPool {
                     constructor(){this.impl=globalThis.__renderSources.poolFactory(TextWorkerUnavailableError)}
                     request(...args){return this.impl.request(...args)}
                     prefetch(...args){return this.impl.prefetch(...args)}
                     invalidate(){return this.impl.invalidate()}
                     dispose(){return this.impl.dispose()}
                     stats(){return this.impl.stats()}
                   }`
                : path === './gpu.mjs'
                  ? `export const createGpuCompositor=async()=>null;`
                  : `export const sharedMediaEngine={image:(...args)=>globalThis.__renderSources.media.image(...args)};`
        }));
      }
    }
  ]
});
const { SceneRenderer } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundled.outputFiles[0].text).toString('base64')
);
class FixtureCanvas {
  constructor(width, height) {
    this.width = width;
    this.height = height;
    this.draws = [];
    this.context = {
      setTransform() {},
      clearRect() {},
      fillRect() {},
      save() {},
      restore() {},
      putImageData: (image, x, y) => {
        this.image = { image, x, y };
      },
      drawImage: (source) => this.draws.push(source)
    };
  }
  getContext() {
    return this.context;
  }
}
function setup(t, overrides = {}, options = {}) {
  const players = [],
    harness = {
      media: {},
      player: async (canvas, _template, text) => {
        const entry = { canvas, text, renders: 0, resizes: 0, closes: 0, edits: 0, samples: [] };
        players.push(entry);
        return {
          durationUs: 1000000,
          setText() {
            entry.edits++;
          },
          resize(w, h) {
            entry.resizes++;
            if (overrides.fixedSize) return false;
            canvas.width = w;
            canvas.height = h;
            return true;
          },
          async render(timeUs) {
            entry.renders++;
            entry.samples.push(timeUs);
            return {
              controlBounds: {
                x: canvas.width / 4,
                y: canvas.height / 4,
                width: canvas.width / 2,
                height: canvas.height / 2
              }
            };
          },
          dispose() {
            entry.closes++;
          }
        };
      },
      ...overrides
    };
  for (const [name, value] of Object.entries({
    OffscreenCanvas: FixtureCanvas,
    ImageData: class {
      constructor(data, width, height) {
        this.data = data;
        this.width = width;
        this.height = height;
      }
    },
    __renderSources: harness
  })) {
    const descriptor = Object.getOwnPropertyDescriptor(globalThis, name);
    Object.defineProperty(globalThis, name, { value, writable: true, configurable: true });
    t.after(() =>
      descriptor ? Object.defineProperty(globalThis, name, descriptor) : delete globalThis[name]
    );
  }
  const canvas = new FixtureCanvas(640, 360),
    scene = new SceneRenderer(canvas, (id) => id, options);
  t.after(() => scene.dispose());
  return { scene, canvas, harness, players };
}
function image(project, id) {
  return addAsset(project, {
    id,
    name: id,
    path: `/${id}.png`,
    kind: 'image',
    duration: ticks(1),
    size: 1,
    width: 320,
    height: 180,
    hasAudio: false
  });
}
function lease(id) {
  const frame = {
    frame: { id },
    width: 320,
    height: 180,
    timestamp: 0,
    closed: 0,
    close() {
      this.closed++;
    }
  };
  return frame;
}

test('Adaptive text extent changes reuse native document/fonts and refresh output bounds', async (t) => {
  const { scene, players } = setup(t),
    project = createProject();
  const item = addText(project, {
    content: 'Native flower',
    length: ticks(1),
    template: { id: 'pattern-flower', version: 1 }
  });
  const first = await scene.render(project, 0, 640, 360);
  const small = await scene.render(project, ticks(0.1), 320, 180);
  const restored = await scene.render(project, ticks(0.2), 640, 360);
  assert.equal(
    players.length,
    1,
    'Changing only raster size does not register fonts or reload the native document'
  );
  assert.equal(players[0].resizes, 2);
  assert.equal(players[0].renders, 3);
  assert.equal(first.textLayoutCount, 1);
  assert.equal(small.textLayoutCount, 0);
  assert.equal(restored.textLayoutCount, 0);
  assert.deepEqual(small.bounds, first.bounds, 'Authoring-space bounds survive preview scaling');
  item.clip.text.content = 'Updated';
  await scene.render(project, ticks(0.3), 640, 360);
  assert.equal(players.length, 1);
  assert.equal(players[0].edits, 1);
});

test('Fixed-size external compositions recreate on an extent change', async (t) => {
  const { scene, players } = setup(t, { fixedSize: true }),
    project = createProject();
  addText(project, {
    content: 'Complex',
    length: ticks(1),
    template: { id: 'pattern-flower', version: 1 }
  });
  await scene.render(project, 0, 640, 360);
  await scene.render(project, ticks(0.1), 320, 180);
  assert.equal(players.length, 2);
  assert.equal(players[0].closes, 1);
  assert.equal(players[1].canvas.width, 320);
});

test('Parallel source failure awaits late frames and closes every acquired bitmap', async (t) => {
  const { scene, harness } = setup(t),
    project = createProject(),
    pending = new Map(),
    started = [];
  image(project, 'first');
  image(project, 'second');
  harness.media.image = (id) => {
    started.push(id);
    return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
  };
  let completed = false;
  const rendering = scene.render(project, 0, 640, 360).finally(() => {
    completed = true;
  });
  const rejection = assert.rejects(rendering, /source failed/);
  while (pending.size < 2) await new Promise((resolve) => setImmediate(resolve));
  assert.equal(started.length, 2, 'Both source requests start before either finishes');
  pending.get('first').reject(new Error('source failed'));
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(
    completed,
    false,
    'Failure does not release frame ownership while another source is still pending'
  );
  const late = lease('second');
  pending.get('second').resolve(late);
  await rejection;
  assert.equal(late.closed, 1);
});

test('Parallel completion retains scene layer, bounds and report order', async (t) => {
  const { scene, canvas, harness } = setup(t),
    project = createProject(),
    pending = new Map();
  const first = image(project, 'first'),
    second = image(project, 'second');
  harness.media.image = (id) => new Promise((resolve) => pending.set(id, resolve));
  const rendering = scene.render(project, 0, 640, 360);
  while (pending.size < 2) await new Promise((resolve) => setImmediate(resolve));
  const late = lease('second'),
    early = lease('first');
  pending.get('first')(early);
  await new Promise((resolve) => setImmediate(resolve));
  pending.get('second')(late);
  const report = await rendering;
  assert.deepEqual(
    report.media.map((entry) => entry.itemId),
    [second.id, first.id]
  );
  assert.deepEqual(
    report.bounds.map((entry) => entry.id),
    [second.id, first.id]
  );
  assert.deepEqual(
    canvas.draws.map((frame) => frame.id),
    ['second', 'first']
  );
  assert.equal(late.closed, 1);
  assert.equal(early.closed, 1);
});

const poolStats = () => ({
  readyHits: 0,
  awaitedHits: 0,
  rendered: 0,
  bytes: 0,
  active: 0,
  queued: 0,
  staleDrops: 0,
  workers: 0
});
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
};
const flower = (project, content) =>
  addText(project, {
    content,
    length: ticks(1),
    template: { id: 'pattern-flower', version: 1 }
  });
const nativeFrame = (input) => ({
  ok: true,
  width: input.width,
  height: input.height,
  rowBytes: input.width * 4,
  originX: 0,
  originY: 0,
  timeUs: input.timeUs,
  data: new Uint8ClampedArray(input.width * input.height * 4),
  controlBounds: {
    x: input.width / 4,
    y: input.height / 4,
    width: input.width / 2,
    height: input.height / 2
  }
});

test(
  'A two-flower paused frame survives worker transport failure and both reference players keep advancing',
  { timeout: 2000 },
  async (t) => {
    const requests = [];
    let disposals = 0;
    const { scene, canvas, players } = setup(
      t,
      {
        poolFactory: (UnavailableError) => ({
          request(input) {
            const pending = deferred();
            requests.push({ input, ...pending });
            if (requests.length === 2)
              queueMicrotask(() =>
                requests[0].reject(new UnavailableError('Nested workers unavailable'))
              );
            return pending.promise;
          },
          dispose() {
            disposals++;
            for (const pending of requests)
              pending.reject(new DOMException('Pool was closed', 'AbortError'));
          },
          invalidate() {},
          prefetch() {},
          stats: poolStats
        })
      },
      { textWorkers: true }
    );
    const project = createProject();
    const first = flower(project, '第一朵花'),
      second = flower(project, '第二朵花');
    const initial = await scene.render(project, 0, 640, 360);
    assert.equal(requests.length, 2, 'Both native sources wait on the same failing pool');
    assert.equal(disposals, 1);
    assert.deepEqual(new Set(initial.media.map((m) => m.itemId)), new Set([first.id, second.id]));
    assert.equal(canvas.draws.length, 2, 'The paused frame completes without another UI request');
    assert.equal(players.length, 2, 'Each pooled clip receives an independent reference player');
    await scene.render(project, ticks(0.1), 640, 360);
    await scene.render(project, ticks(0.2), 640, 360);
    assert.equal(requests.length, 2, 'Transport fallback stops submitting native worker requests');
    assert.equal(players.length, 2, 'Fallback reuses both installed documents');
    for (const player of players)
      assert.deepEqual(
        player.samples,
        [0, 100000, 200000],
        `${player.text} must advance after fallback`
      );
  }
);

test(
  'Disposing during asynchronous fallback installation closes its late player and never draws',
  { timeout: 2000 },
  async (t) => {
    const installation = deferred(),
      started = deferred();
    const resource = {
      renders: 0,
      closes: 0,
      durationUs: 1000000,
      setText() {},
      resize() {
        return true;
      },
      async render() {
        this.renders++;
        throw new Error('A disposed player must not render');
      },
      dispose() {
        this.closes++;
      }
    };
    const { scene, canvas } = setup(
      t,
      {
        player: () => {
          started.resolve();
          return installation.promise;
        },
        poolFactory: (UnavailableError) => ({
          request: () => Promise.reject(new UnavailableError('Worker startup failed')),
          dispose() {},
          invalidate() {},
          prefetch() {},
          stats: poolStats
        })
      },
      { textWorkers: true }
    );
    const project = createProject();
    flower(project, '晚到的花字');
    const rendering = scene.render(project, 0, 640, 360);
    const rejection = assert.rejects(rendering, /渲染器已关闭/);
    await started.promise;
    scene.dispose();
    installation.resolve(resource);
    await rejection;
    assert.equal(resource.closes, 1, 'Late installation retains ownership until it can close');
    assert.equal(resource.renders, 0);
    assert.equal(
      canvas.draws.length,
      0,
      'Disposal prevents the completed installation replacing the canvas'
    );
  }
);

test(
  'Changing raster extent invalidates outstanding predictions before submitting the foreground frame',
  { timeout: 2000 },
  async (t) => {
    const events = [],
      predictions = [];
    let generation = 0;
    const { scene } = setup(
      t,
      {
        poolFactory: () => ({
          async request(input) {
            events.push({ kind: 'request', input, generation });
            return nativeFrame(input);
          },
          prefetch(inputs) {
            predictions.push(...inputs);
            events.push({ kind: 'prefetch', inputs, generation });
          },
          invalidate() {
            generation++;
            predictions.length = 0;
            events.push({ kind: 'invalidate', generation });
          },
          dispose() {},
          stats: poolStats
        })
      },
      { textWorkers: true }
    );
    const project = createProject();
    flower(project, '原尺寸花字');
    await scene.render(project, 0, 640, 360);
    scene.prepareAhead(project, 0, 640, 360);
    assert.equal(predictions.length, 6);
    assert.ok(predictions.every((input) => input.width === 640 && input.height === 360));
    const initialRequest = events.find((event) => event.kind === 'request');
    await scene.render(project, ticks(0.1), 320, 180);
    assert.equal(
      predictions.length,
      0,
      'Obsolete extent jobs are invalidated before foreground work'
    );
    const resizeRequest = events.find(
      (event) => event.kind === 'request' && event.input.width === 320
    );
    assert.equal(resizeRequest.generation, initialRequest.generation + 1);
    assert.equal(events[events.indexOf(resizeRequest) - 1].kind, 'invalidate');
    assert.notEqual(resizeRequest.input.key, initialRequest.input.key);
    scene.prepareAhead(project, ticks(0.1), 320, 180);
    assert.equal(predictions.length, 6);
    assert.ok(predictions.every((input) => input.width === 320 && input.height === 180));
    assert.equal(events.at(-1).generation, resizeRequest.generation);
    predictions.length = 0;
    scene.prepareAhead(project, ticks(0.95), 320, 180);
    assert.ok(predictions.length > 0 && predictions.length < 6);
    assert.ok(
      predictions.every((input) => input.timeUs < 1000000),
      'Export predictions stop before the authored end'
    );
    predictions.length = 0;
    scene.prepareAhead(project, ticks(1), 320, 180);
    assert.equal(predictions.length, 0, 'The final frame never starts work beyond project end');
  }
);

test('An invalid future template does not reject a valid prefetched frame and still fails at its playhead', async (t) => {
  const requests = [],
    batches = [];
  let predictions = [];
  const { scene, canvas } = setup(
    t,
    {
      poolFactory: () => ({
        async request(input) {
          requests.push(input);
          return nativeFrame(input);
        },
        prefetch(inputs) {
          predictions = [...inputs];
          batches.push(predictions);
        },
        invalidate() {
          predictions = [];
        },
        dispose() {},
        stats: poolStats
      })
    },
    { textWorkers: true }
  );
  const project = createProject();
  const current = addText(project, {
    content: '当前有效花字',
    length: ticks(0.1),
    template: { id: 'pattern-flower', version: 1 }
  });
  const future = addText(project, {
    content: '未来无效模板',
    start: ticks(0.1),
    length: ticks(1),
    template: { id: 'pattern-flower', version: 1 }
  });
  future.clip.text.template.id = 'missing-future-template';
  const first = await scene.render(project, 0, 640, 360, undefined, { prefetch: true });
  assert.deepEqual(
    first.media.map((entry) => entry.itemId),
    [current.id]
  );
  assert.ok(predictions.length > 0, 'The initial window contains valid successor frames');
  assert.ok(predictions.every((input) => input.text === current.clip.text.content));
  const lastValid = await scene.render(project, ticks(2 / 30), 640, 360, undefined, {
    prefetch: true
  });
  assert.deepEqual(
    lastValid.media.map((entry) => entry.itemId),
    [current.id]
  );
  assert.deepEqual(batches.at(-1), [], 'An entirely ineligible future window clears predictions');
  assert.deepEqual(predictions, []);
  assert.equal(canvas.draws.length, 2, 'Both valid foreground frames complete');
  assert.equal(requests.length, 2);
  await assert.rejects(
    scene.render(project, ticks(0.1), 640, 360, undefined, { prefetch: true }),
    /未找到文字模板/
  );
  assert.equal(requests.length, 2, 'Invalid foreground input cannot become a worker cache hit');
  assert.equal(canvas.draws.length, 2, 'A previous valid frame cannot mask the invalid template');
});
