import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { ByteCache, MediaScheduler, PeakAccumulator, sampleLinear, rasterSize, thumbnailGeometry, sourceCacheIdentity } from '../packages/media/runtime.mjs';
import { createProject, addAsset, ticks } from '../packages/core/project.mjs';

const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

test('portrait slot raster uses both dimensions and DPR before cropping without throwing away detail', () => {
  const portrait = thumbnailGeometry(360, 640, 80, 32, 2, 'cover');
  assert.equal(portrait.width, 160); assert.equal(portrait.height, 64);
  assert.equal(portrait.drawWidth, 160); assert.ok(portrait.drawHeight > 280);
  assert.equal(portrait.x, 0); assert.ok(portrait.y < -100);
  const wide = thumbnailGeometry(1280, 360, 80, 32, 2, 'cover');
  assert.equal(wide.height, 64); assert.ok(wide.drawWidth > 220);
  const tiny = thumbnailGeometry(20, 40, 80, 32, 2, 'cover');
  assert.equal(tiny.width, 20); assert.equal(tiny.drawWidth, 20);
  const contained = thumbnailGeometry(360, 640, 160, 90, 2, 'contain');
  assert.ok(contained.x > 0); assert.equal(contained.y, 0);
  assert.throws(() => thumbnailGeometry(360,640,-1,48,2));
});

test('byte cache updates recency, counts bytes, and closes exactly evicted resources', () => {
  const closed = [];
  const cache = new ByteCache(10);
  cache.set('a', { close: () => closed.push('a') }, 4);
  cache.set('b', { close: () => closed.push('b') }, 4);
  cache.get('a');
  cache.set('c', { close: () => closed.push('c') }, 4);
  assert.deepEqual(closed, ['b']);
  assert.equal(cache.bytes, 8);
  cache.clear();
  assert.equal(cache.bytes, 0);
  assert.deepEqual(closed, ['b', 'a', 'c']);
});

test('media scheduler deduplicates producers and cancellation does not cancel another consumer', async () => {
  const queue = new MediaScheduler(1);
  const controller = new AbortController();
  let runs = 0, finish;
  const produce = async (signal) => {
    runs++;
    return new Promise((resolve) => { finish = () => { assert.equal(signal.aborted, false); resolve(42); }; });
  };
  const first = queue.run('frame', produce, { signal: controller.signal });
  const second = queue.run('frame', produce);
  const rejection = assert.rejects(first, { name: 'AbortError' });
  await settle();
  controller.abort(); finish();
  await rejection;
  assert.equal(await second, 42);
  assert.equal(runs, 1);
  queue.dispose();
});

test('cancelled queued work never starts and shared resources dispatch before producer release', async () => {
  const queue = new MediaScheduler(1);
  let finish, cancelledRuns = 0;
  const active = queue.run('busy', () => new Promise((resolve) => { finish = resolve; }));
  await settle();
  const controller = new AbortController();
  const pending = queue.run('cancelled', () => { cancelledRuns++; }, { signal: controller.signal });
  const rejection = assert.rejects(pending, { name: 'AbortError' });
  controller.abort(); finish(); await active; await rejection;
  assert.equal(cancelledRuns, 0);
  const events = [];
  const a = queue.run('owned', () => 5, { consume: (value) => { events.push('a'); return value; }, afterDispatch: () => events.push('released') });
  const b = queue.run('owned', () => { throw Error('duplicate'); }, { consume: (value) => { events.push('b'); return value; } });
  assert.deepEqual(await Promise.all([a, b]), [5, 5]);
  assert.deepEqual(events, ['a', 'b', 'released']);
  queue.dispose();
});

test('all consumers cancelling aborts producer and releases late resources', async () => {
  const queue = new MediaScheduler(1), controller = new AbortController();
  let finish, signal, closed = 0;
  const result = queue.run('slow', (activeSignal) => {
    signal = activeSignal;
    return new Promise((resolve) => { finish = resolve; });
  }, { signal: controller.signal, afterDispatch: () => closed++ });
  const rejected = assert.rejects(result, { name: 'AbortError' });
  await settle(); controller.abort(); await rejected;
  assert.equal(signal.aborted, true);
  finish({}); await settle();
  assert.equal(closed, 1);
  assert.equal(queue.active, 0);
  queue.dispose();
});

test('waveform pyramid preserves signed transients independently per channel at every scale', () => {
  const accumulator = new PeakAccumulator(9, 2, 2);
  accumulator.add([new Float32Array([0, 1, -0.5, 0]), new Float32Array([0, -1, 0.5, 0])], 0);
  accumulator.add([new Float32Array([0.25, 0, 0, -0.75, 0]), new Float32Array([-0.25, 0, 0, 0.75, 0])], 4);
  const levels = accumulator.finish();
  assert.deepEqual(levels.map((level) => level.channels[0].min.length), [5, 3, 2, 1]);
  assert.equal(levels.at(-1).channels[0].min[0], -0.75);
  assert.equal(levels.at(-1).channels[0].max[0], 1);
  assert.equal(levels.at(-1).channels[1].min[0], -1);
  assert.equal(levels.at(-1).channels[1].max[0], 0.75);
  // Opposite phase channels remain visible; they were never downmixed before measuring.
  assert.equal(levels[0].channels[0].max[0], 1);
  assert.equal(levels[0].channels[1].min[0], -1);
});

test('waveform clips samples to chunk bounds and treats missing buckets as silence', () => {
  const accumulator = new PeakAccumulator(8, 1, 2);
  accumulator.add([new Float32Array([99, 0.5, -0.5, 0.25])], -1);
  const base = accumulator.finish()[0].channels[0];
  assert.deepEqual([...base.min], [-0.5, 0.25, 0, 0]);
  assert.deepEqual([...base.max], [0.5, 0.25, 0, 0]);
});

test('DPR raster targets preserve portrait orientation without upscaling tiny sources', () => {
  assert.deepEqual(rasterSize(1080, 1920, 48, 2), { width: 54, height: 96 });
  assert.deepEqual(rasterSize(24, 16, 48, 2), { width: 24, height: 16 });
  assert.equal(rasterSize(1920, 1080, 900, 4).height, 512);
  assert.equal(sampleLinear(new Float32Array([0, 1]), 0.5), 0.5);
  assert.equal(sampleLinear(new Float32Array([0, 1]), -1), 0);
});

let engineModule;
async function getEngine() {
  if (!engineModule) {
    const result = await build({ entryPoints: [new URL('../packages/media/browser.ts', import.meta.url).pathname], bundle: true, write: false, format: 'esm', platform: 'browser', target: 'es2022' });
    engineModule = import(`data:text/javascript;base64,${Buffer.from(result.outputFiles[0].text).toString('base64')}`);
  }
  return engineModule;
}

test('bounded mixer respects placement, source trims, gain and muted tracks', async () => {
  const { MediaEngine } = await getEngine();
  const engine = new MediaEngine();
  let requested = [];
  engine.audioWindow = async (_url, begin, end) => {
    requested.push([begin, end]);
    return [{ data: [new Float32Array(8000).fill(0.8)], timestamp: 0, sampleRate: 8000, numberOfFrames: 8000 }];
  };
  const project = createProject('Mixer test');
  const item = addAsset(project, { id: 'a', kind: 'audio', path: '/a.wav', name: 'a', duration: ticks(1), width: 0, height: 0, hasAudio: true, size: 1 }, { start: ticks(0.1) });
  item.clip.audio.gainLinear = 0.5;
  const blocks = [];
  for await (const block of engine.mixAudio(project, (id) => id, 0, ticks(0.2), { sampleRate: 8000, blockFrames: 256 })) blocks.push(block);
  const output = blocks.flatMap((block) => [...block.data[0]]);
  assert.equal(output.length, 1600);
  assert.equal(output.slice(0, 800).every((value) => value === 0), true);
  assert.ok(Math.abs(output[1000] - 0.4) < 1e-6);
  assert.equal(blocks[0].timestamp, 0);
  assert.ok(requested.every(([begin, end]) => end - begin < 0.1));
  project.timeline.tracks[0].muted = true; requested = [];
  for await (const block of engine.mixAudio(project, (id) => id, 0, ticks(0.2), { sampleRate: 8000, blockFrames: 256 })) assert.equal(block.data.every((channel) => channel.every((value) => value === 0)), true);
  assert.equal(requested.length, 0);
  engine.dispose();
});

test('audio transport stays at its requested position while the first decode is pending', async () => {
  const { PreviewAudioPlayer } = await getEngine();
  const original = globalThis.AudioContext;
  class TestAudioContext {
    currentTime = 500;
    async resume() {}
    async close() {}
  }
  globalThis.AudioContext = TestAudioContext;
  let finish;
  const engine = { async *mixAudio() { await new Promise((resolve) => { finish = resolve; }); } };
  const player = new PreviewAudioPlayer(engine, (id) => id);
  try {
    const playing = player.play(createProject('clock'), ticks(4));
    const rejected = assert.rejects(playing, { name: 'AbortError' });
    await settle();
    assert.equal(player.currentTimeTicks(), ticks(4));
    player.stop(); finish();
    await rejected;
    assert.equal(player.currentTimeTicks(), ticks(4));
  } finally { await player.dispose(); globalThis.AudioContext = original; }
});


test('cache identity reuses verified source versions across sessions but rejects size-only aliases', () => {
  const a='http://localhost:4318/api/sessions/one/media?asset=a&token=one&source=1:2:300:400.5';
  const b='http://localhost:4318/api/sessions/two/media?asset=b&token=two&source=1:2:300:400.5';
  assert.equal(sourceCacheIdentity(a),sourceCacheIdentity(b));
  assert.notEqual(sourceCacheIdentity(a),sourceCacheIdentity(b.replace('400.5','401.5')));
  assert.notEqual(sourceCacheIdentity(a.replace('1:2:300:400.5','300')),sourceCacheIdentity(b.replace('1:2:300:400.5','300')));
  assert.equal(sourceCacheIdentity('blob:test'),'blob:test');
});
