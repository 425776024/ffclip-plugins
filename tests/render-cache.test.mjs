import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import {
  createProject,
  addText,
  ticks,
  createProjectDraft,
  documentPatches
} from '../packages/core/project.mjs';
const load = async (path) => {
  const result = await build({
    entryPoints: [path],
    bundle: true,
    platform: 'node',
    format: 'esm',
    write: false,
    logLevel: 'silent'
  });
  return import(
    'data:text/javascript;base64,' + Buffer.from(result.outputFiles[0].text).toString('base64')
  );
};
const { RenderResourceCache, RenderMemoryPool, RenderBudgetError } = await load(
  'packages/render/resource-cache.ts'
);
const { AdaptivePreviewQuality } = await load('packages/render/quality.ts');
const { SceneGraph, visualFrameIdentity, localEffectGeometry } = await load(
  'packages/render/graph.ts'
);
const { PreviewDocument, presentationProject } = await load('packages/render/projection.ts');
test('GPU LRU keeps current dependencies, evicts inactive oldest resources, and accounts text allocations', () => {
  const freed = [],
    cache = new RenderResourceCache(100, (value, key) => freed.push(key)),
    pool = new RenderMemoryPool();
  pool.register(cache, 100);
  cache.set('a', {}, 30);
  cache.set('b', {}, 30);
  cache.releasePins();
  cache.get('a');
  pool.reserve(50);
  cache.set('c', {}, 50);
  assert.deepEqual(freed, ['b']);
  assert.equal(pool.bytes, 80);
  assert.throws(() => pool.retainExternal({}, 30), RenderBudgetError);
  cache.releasePins();
  const font = {};
  pool.retainExternal(font, 40);
  assert.deepEqual(freed, ['b', 'a']);
  assert.equal(pool.externalBytes, 40);
  assert.equal(pool.bytes, 90);
  assert.equal(pool.peakBytes, 90);
  pool.releaseExternal(font);
  cache.clear();
  assert.equal(pool.bytes, 0);
});
test('rebuild priority preserves costly inputs across owners without bypassing pins or budget', () => {
  const freed = [],
    a = new RenderResourceCache(100, (_, key) => freed.push(key)),
    b = new RenderResourceCache(100, (_, key) => freed.push(key)),
    pool = new RenderMemoryPool();
  pool.register(a, 100);
  pool.register(b, 100);
  a.set('native-old', {}, 30);
  a.setPriority('native-old', 1);
  a.set('ordinary-old', {}, 20);
  a.set('ordinary-new', {}, 20);
  a.releasePins();
  b.set('active', {}, 20);
  pool.reserve(30);
  assert.deepEqual(freed, ['ordinary-old']);
  b.set('new-input', {}, 30);
  assert.equal(pool.bytes, 100);
  assert.ok(a.has('native-old'));
  pool.reserve(30);
  assert.deepEqual(freed, ['ordinary-old', 'ordinary-new', 'native-old']);
  assert.throws(() => pool.reserve(60), RenderBudgetError);
  assert.ok(b.has('active'), 'Current dependencies remain pinned');
  assert.ok(b.has('new-input'));
});
test('native retention preference can reset and local reservations follow the same policy', () => {
  const freed = [],
    cache = new RenderResourceCache(100, (_, key) => freed.push(key));
  cache.set('native-old', {}, 30);
  cache.setPriority('native-old', 1);
  cache.set('ordinary-old', {}, 30);
  cache.releasePins();
  cache.reserve(50);
  assert.deepEqual(freed, ['ordinary-old']);
  cache.set('ordinary-new', {}, 30);
  cache.releasePins();
  cache.resetPriorities();
  cache.reserve(50);
  assert.deepEqual(freed, ['ordinary-old', 'native-old']);
  assert.equal(cache.bytes, 30);
});
test('visual graph ignores audio edits, samples property edits without rebuilding topology, and rebuilds insertion', () => {
  const p = createProject(),
    item = addText(p, { content: 'Cache', length: ticks(1) }),
    graph = new SceneGraph();
  const first = graph.evaluate(p, 0, 640, 360),
    key = visualFrameIdentity(first, () => '');
  item.clip.audio.gainLinear = 0.1;
  p.timeline.tracks[0].muted = true;
  assert.equal(
    visualFrameIdentity(graph.evaluate(p, 0, 640, 360), () => ''),
    key
  );
  assert.equal(graph.builds, 1);
  item.clip.visual.positionX = 40;
  assert.notEqual(
    visualFrameIdentity(graph.evaluate(p, 0, 640, 360), () => ''),
    key
  );
  assert.equal(graph.builds, 1);
  item.clip.audio.gainLinear = 1;
  p.timeline.tracks[0].muted = false;
  addText(p, { content: 'New', length: ticks(1) });
  graph.evaluate(p, 0, 640, 360);
  assert.equal(graph.builds, 2);
});
test('effect-local geometry is invariant under translation and pads spatial kernels', () => {
  const effects = [
    { templateId: 'blur', parameters: { radius: 8 } },
    { templateId: 'glow', parameters: { radius: 12 } }
  ];
  const base = {
    matrix: [200, 30, -10, 100, 50, 70],
    renderScale: 0.5,
    crop: [0, 0, 1, 1],
    opacity: 1
  };
  const a = localEffectGeometry(base, effects),
    b = localEffectGeometry({ ...base, matrix: [200, 30, -10, 100, 85, 20] }, effects);
  assert.deepEqual(a.geometry.inverse, b.geometry.inverse);
  assert.equal(b.x - a.x, 35);
  assert.equal(b.y - a.y, -50);
  assert.equal(a.width, 254);
  assert.equal(a.height, 174);
});
test('preview quality downgrades sustained slow frames, ignores cache hits and restores paused detail', () => {
  const quality = new AdaptivePreviewQuality();
  for (let i = 0; i < 4; i++)
    quality.sample(
      { frameMs: 70, gpuPasses: 5, textureBytes: 10, textureBudget: 100 },
      30,
      1000 + i * 40
    );
  assert.equal(quality.scale, 0.75);
  for (let i = 0; i < 100; i++)
    quality.sample({ frameMs: 0.01, gpuPasses: 0, compositeHits: 1 }, 30, 3000 + i * 40);
  assert.equal(quality.scale, 0.75);
  quality.paused();
  assert.equal(quality.scale, 1);
  for (let i = 0; i < 10; i++)
    quality.sample(
      { frameMs: 1, gpuPasses: 2, resourceBytes: 99, workingSetBytes: 20, textureBudget: 100 },
      30,
      10000 + i * 40
    );
  assert.equal(quality.scale, 1, 'Reclaimable cache residency must not lower preview quality');
  assert.equal(quality.budgetPressure(10000), true);
  assert.equal(quality.extent({ width: 640, height: 360 }).width, 480);
  for (let i = 0; i < 4; i++) quality.budgetPressure(10000);
  assert.equal(quality.scale, 0.25);
  assert.equal(quality.budgetPressure(10000), false);
  const cpu = new AdaptivePreviewQuality();
  for (let i = 0; i < 4; i++)
    cpu.sample({ frameMs: 70, gpuPasses: 0, compositeHits: 0 }, 30, 1000 + i * 40);
  assert.equal(
    cpu.scale,
    0.75,
    'Canvas2D real rendering still participates in the frame-time controller'
  );
});
test('preview revision deltas reject stale bases, and pointer overrides copy only the edited branch', () => {
  const p = createProject(),
    item = addText(p, { content: 'immutable' });
  item.clip.automation = {
    'visual.positionX': {
      timeDomain: 'itemLocal',
      keyframes: [{ id: 'key', time: 0, value: 10, interpolation: 'linear' }]
    }
  };
  const draft = createProjectDraft(p);
  draft.draft.name = 'renamed';
  const next = draft.finish();
  const state = new PreviewDocument();
  assert.equal(state.receive({ project: p, documentRevision: 1 }), true);
  const patches = documentPatches(p, next);
  assert.equal(state.receive({ patches, baseRevision: 0, documentRevision: 2 }), false);
  assert.equal(state.project, p);
  assert.equal(state.receive({ patches, baseRevision: 1, documentRevision: 2 }), true);
  assert.equal(state.project.name, 'renamed');
  assert.equal(state.receive({ documentRevision: 1 }), false);
  const shown = presentationProject(state.project, { id: item.id, x: 70, y: -30 });
  const edited = shown.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === item.id);
  assert.equal(edited.clip.visual.positionX, 70);
  assert.equal(edited.clip.visual.positionY, -30);
  assert.equal(edited.clip.automation['visual.positionX'], undefined);
  assert.equal(item.clip.automation['visual.positionX'].keyframes[0].value, 10);
  assert.equal(shown.assets, p.assets);
  assert.equal(edited.clip.audio, item.clip.audio);
  assert.equal(presentationProject(state.project), state.project);
});
