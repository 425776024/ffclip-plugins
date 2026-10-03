import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';

const output = await build({
  entryPoints: ['packages/media/browser.ts'], bundle: true, platform: 'node',
  format: 'esm', write: false, logLevel: 'silent', plugins: [{
    name: 'decoded-video-fixture',
    setup(api) {
      api.onResolve({ filter: /^mediabunny$/ }, () => ({ path: 'fixture', namespace: 'fixture' }));
      api.onLoad({ filter: /.*/, namespace: 'fixture' }, () => ({ contents: `
        export class Input {} export class UrlSource {} export const ALL_FORMATS = [];
        export class VideoSampleSink { constructor(track) { this.track=track; } samples(target) { return this.track.samples(target); } }
        export class AudioSampleSink {} export class EncodedPacketSink {}
        export class MatroskaInputFormat {} export class OggInputFormat {}
      ` }));
    }
  }]
});
const { MediaEngine } = await import('data:text/javascript;base64,' + Buffer.from(output.outputFiles[0].text).toString('base64'));

function setup(t, { pts = [0, .04, .1, .25, .6], durations = [], duration = 1, origin = 10, width = 1920, height = 1080, gate, failAt } = {}) {
  const state = { starts: [], samples: [], bitmaps: [], releases: 0 };
  const track = {
    async *samples(target) {
      state.starts.push(target);
      let index = 0;
      while (index + 1 < pts.length && origin + pts[index + 1] <= target) index++;
      if (gate) await gate();
      for (; index < pts.length; index++) {
        if (index === failAt) throw new Error('Fixture decoder failed');
        const sample = {
          timestamp: origin + pts[index], duration: durations[index] ?? .04,
          displayWidth: width, displayHeight: height, id: pts[index], closed: 0,
          draw(context) { context.canvas.id = this.id; },
          close() { assert.equal(++this.closed, 1, 'decoded sample closes exactly once'); }
        };
        state.samples.push(sample);
        yield sample;
      }
    }
  };
  class Canvas {
    constructor(w, h) { this.width = w; this.height = h; }
    getContext() { return { canvas: this }; }
    transferToImageBitmap() {
      const bitmap = { width: this.width, height: this.height, id: this.id, closed: 0,
        close() { assert.equal(++this.closed, 1, 'bitmap closes exactly once'); } };
      state.bitmaps.push(bitmap);
      return bitmap;
    }
  }
  const descriptor = Object.getOwnPropertyDescriptor(globalThis, 'OffscreenCanvas');
  Object.defineProperty(globalThis, 'OffscreenCanvas', { value: Canvas, configurable: true });
  const engine = new MediaEngine(), readers = [];
  engine.acquire = async () => ({ source: { video: track,
    metadata: { sourceOrigin: origin, duration, videoDecodable: true } },
    release() { state.releases++; } });
  const reader = async (url = '/video.mp4', options = {}) => {
    const value = await engine.createVideoReader(url, options);
    readers.push(value);
    return value;
  };
  t.after(async () => {
    for (const value of readers) await value.close();
    engine.dispose();
    assert.ok(state.samples.every((sample) => sample.closed === 1));
    assert.ok(state.bitmaps.every((bitmap) => bitmap.closed === 1));
    assert.equal(state.releases, readers.length);
    if (descriptor) Object.defineProperty(globalThis, 'OffscreenCanvas', descriptor);
    else delete globalThis.OffscreenCanvas;
  });
  return { engine, state, reader };
}
async function frame(reader, time) {
  const result = await reader.frameAt(time);
  const value = { id: result.frame.id, timestamp: result.timestamp, duration: result.duration, width: result.width, height: result.height };
  result.close();
  return value;
}

test('reader history uses real VFR PTS boundaries and preserves the physical forward cursor on cache hits', async (t) => {
  const { reader, state } = setup(t, { durations: [.2, .02, .04, .04, .04] });
  const r = await reader('/video.mp4', { width: 960, height: 540 });
  assert.equal((await frame(r, .08)).id, .04);
  assert.equal((await frame(r, .06)).id, .04);
  assert.equal(state.starts.length, 1, 'leftward movement in one decoded frame must not flush');
  assert.equal((await frame(r, .1)).id, .1, 'next PTS is an exclusive boundary');
  assert.equal((await frame(r, .3)).id, .25);
  assert.equal((await frame(r, .11)).id, .1);
  assert.equal((await frame(r, .21)).id, .1, 'a real VFR display gap holds the preceding frame');
  assert.equal((await frame(r, .65)).id, .6, 'history hits must not retarget the physical cursor behind its real position');
  assert.equal(state.starts.length, 1);
  assert.equal(state.bitmaps.length, 4);
  assert.ok(state.bitmaps.every((bitmap) => bitmap.width === 960 && bitmap.height === 540));
});

test('readers share immutable bitmap history within the existing engine budget and isolate output extents', async (t) => {
  const { reader, engine, state } = setup(t);
  const a = await reader('/video.mp4', { width: 960, height: 540 });
  const first = await a.frameAt(.08);
  const b = await reader('/video.mp4', { width: 960, height: 540 });
  const second = await b.frameAt(.06);
  assert.equal(first.frame, second.frame);
  assert.equal(state.starts.length, 1);
  assert.equal(engine.stats().frameBytes, 960 * 540 * 4);
  first.close(); second.close();
  const c = await reader('/video.mp4', { width: 320, height: 180 });
  const different = await c.frameAt(.06);
  assert.equal(different.width, 320);
  assert.equal(different.height, 180);
  different.close();
  assert.equal(state.starts.length, 2);
});

test('budget eviction releases history independently of held leases and a missing interval decodes again', async (t) => {
  const { reader, engine, state } = setup(t, { pts: [0, .1, .2, .3] });
  engine.frames.maximumBytes = 2 * 320 * 180 * 4;
  const r = await reader('/video.mp4', { width: 320, height: 180 });
  const held = await r.frameAt(0);
  await frame(r, .1); await frame(r, .2);
  assert.ok(engine.stats().frameBytes <= engine.frames.maximumBytes);
  assert.equal(held.frame.closed, 0, 'an evicted bitmap remains valid for its consumer');
  await frame(r, 0);
  assert.equal(state.starts.length, 2);
  assert.equal(state.bitmaps.length, 4);
  held.close();
  assert.equal(held.frame.closed, 1);
});

test('a confirmed last video frame keeps the existing hold behavior through the media end', async (t) => {
  const { reader, state } = setup(t, { pts: [0, .2, .45], duration: 1 });
  const r = await reader();
  assert.equal((await frame(r, .9)).id, .45);
  assert.equal((await frame(r, .5)).id, .45);
  assert.equal((await frame(r, 99)).id, .45);
  assert.equal(state.starts.length, 1);
  assert.equal((await frame(r, .449)).id, .2);
  assert.equal((await frame(r, .45)).id, .45);
  assert.equal(state.starts.length, 2);
});

test('abort and close release late decoder samples without admitting an unpublished bitmap', async (t) => {
  let finish;
  const deferred = new Promise((resolve) => { finish = resolve; });
  const { reader, engine, state } = setup(t, { gate: () => deferred });
  const controller = new AbortController();
  const r = await reader('/video.mp4', { signal: controller.signal });
  const pending = r.frameAt(.2);
  const rejected = assert.rejects(pending, { name: 'AbortError' });
  await new Promise((resolve) => setTimeout(resolve, 0));
  controller.abort(); finish();
  await rejected;
  assert.equal(state.bitmaps.length, 0);
  assert.equal(engine.stats().frameBytes, 0);
  await assert.rejects(r.frameAt(.1), { name: 'AbortError' });
  await r.close();
});

test('source invalidation rejects an old reader and admits new pixels under a new generation', async (t) => {
  const { reader, engine, state } = setup(t);
  const old = await reader();
  await frame(old, .08);
  engine.invalidate('/video.mp4');
  assert.equal(engine.stats().frameBytes, 0);
  await assert.rejects(old.frameAt(.06), { name: 'AbortError' });
  const current = await reader();
  await frame(current, .06);
  assert.equal(state.starts.length, 2);
  assert.equal(state.bitmaps.length, 2);
  engine.dispose();
  await assert.rejects(current.frameAt(.06), { name: 'AbortError' });
});

test('a decoder failure while advancing closes the adopted next sample once', async (t) => {
  const { reader, state } = setup(t, { pts: [0, .1, .2, .3], failAt: 2 });
  const r = await reader();
  await frame(r, 0);
  await assert.rejects(r.frameAt(.15), /Fixture decoder failed/);
  assert.equal(state.samples.length, 2);
  assert.ok(state.samples.every((sample) => sample.closed === 1));
});

test('tiny-source history also has a bounded entry count', async (t) => {
  const pts = Array.from({ length: 132 }, (_, i) => i / 100);
  const { reader, engine } = setup(t, { pts, origin: 0, duration: 2, width: 1, height: 1 });
  const r = await reader();
  for (const time of pts) await frame(r, time);
  assert.equal(engine.frames.entries.size, 128);
  assert.equal(engine.stats().frameBytes, 128 * 4);
});
