import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';

const bundled = await build({
  entryPoints: ['packages/render/text-prefetch.ts'],
  bundle: true,
  platform: 'node',
  format: 'esm',
  write: false,
  logLevel: 'silent'
});
const { TextPrefetchPool, TextWorkerUnavailableError } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundled.outputFiles[0].text).toString('base64')
);
const request = (key, dimensions = {}) => ({
  key,
  template: { id: 'layered-flower', version: 1 },
  text: '灵感花开',
  width: 2,
  height: 2,
  timeUs: 1500000,
  ...dimensions
});
function frame(input, value = 17) {
  const width = input.width,
    height = input.height;
  const data = new Uint8ClampedArray(width * height * 4);
  for (let i = 0; i < data.length; i += 4) data.set([value, 29, 47, 128], i);
  const bounds = { x: 0, y: 0, width, height };
  return {
    ok: true,
    profile: 'wasm-raster-v1',
    width,
    height,
    originX: 0,
    originY: 0,
    rowBytes: width * 4,
    byteLength: data.byteLength,
    data,
    timeUs: input.timeUs,
    logicalBounds: bounds,
    inkBounds: { ...bounds },
    controlBounds: { ...bounds },
    visualExtent: { ...bounds },
    cacheBytes: 0,
    warnings: []
  };
}
function setup(t, options = {}) {
  const workers = [];
  class FixtureWorker {
    onmessage = null;
    onerror = null;
    onmessageerror = null;
    requests = [];
    terminated = false;
    postMessage(data) {
      this.requests.push(data);
    }
    terminate() {
      this.terminated = true;
    }
    complete(value = 17) {
      const current = this.requests.at(-1),
        owned = frame(current.input, value),
        buffer = owned.data.buffer;
      const reply = structuredClone({ id: current.id, frame: owned }, { transfer: [buffer] });
      assert.equal(
        buffer.byteLength,
        0,
        'Worker transfers its owned snapshot instead of borrowing native memory'
      );
      this.onmessage?.({ data: reply });
      return reply.frame;
    }
    fail(message) {
      this.onmessage?.({
        data: { id: this.requests.at(-1).id, error: { name: 'Error', message } }
      });
    }
  }
  const pool = new TextPrefetchPool({
    ...options,
    workerFactory: () => {
      const worker = new FixtureWorker();
      workers.push(worker);
      return worker;
    }
  });
  t.after(() => pool.dispose());
  return { pool, workers };
}

test('lazy two-worker pool bounds background work, promotes foreground singleflight and returns owned immutable snapshots', async (t) => {
  const { pool, workers } = setup(t);
  assert.equal(pool.stats().workers, 0);
  pool.prefetch('abcdefghi'.split('').map((key) => request(key)));
  assert.deepEqual(
    { active: pool.stats().active, queued: pool.stats().queued, workers: pool.stats().workers },
    { active: 2, queued: 6, workers: 2 }
  );
  const g1 = pool.request(request('g')),
    g2 = pool.request(request('g')),
    foreground = pool.request(request('foreground'));
  const a = workers[0].complete();
  assert.equal(
    workers[0].requests.at(-1).input.key,
    'g',
    'Promoted prefetch runs before earlier queued background jobs'
  );
  const ready = await pool.request(request('a'));
  assert.equal(ready, a);
  assert.equal(pool.stats().readyHits, 1);
  assert.throws(() => {
    ready.controlBounds.x = 999;
  }, TypeError);
  assert.deepEqual(Array.from(ready.data.subarray(0, 4)), [17, 29, 47, 128]);
  workers[0].complete(71);
  const [one, two] = await Promise.all([g1, g2]);
  assert.equal(one, two);
  assert.equal(pool.stats().awaitedHits, 2);
  assert.equal(workers[0].requests.at(-1).input.key, 'foreground');
  workers[0].complete(89);
  await foreground;
  pool.dispose();
  assert.ok(workers.every((worker) => worker.terminated));
  assert.deepEqual(
    Array.from(ready.data.subarray(0, 4)),
    [17, 29, 47, 128],
    'Cache disposal does not invalidate caller-owned pixel references'
  );
});

test('one waiter abort does not cancel peers; invalidate settles queued/active foreground and drops stale publication', async (t) => {
  const { pool, workers } = setup(t, { workers: 1 });
  const cancel = new AbortController(),
    first = pool.request(request('a'), cancel.signal),
    peer = pool.request(request('a'));
  const aborted = assert.rejects(first, { name: 'AbortError' });
  cancel.abort();
  await aborted;
  workers[0].complete();
  const cached = await peer;
  const active = pool.request(request('b')),
    queued = pool.request(request('c'));
  const rejectedActive = assert.rejects(active, { name: 'AbortError' }),
    rejectedQueued = assert.rejects(queued, { name: 'AbortError' });
  pool.prefetch([request('d')]);
  pool.invalidate();
  await Promise.all([rejectedActive, rejectedQueued]);
  assert.equal(pool.stats().queued, 0);
  assert.equal(pool.stats().active, 1);
  assert.equal(
    await pool.request(request('a')),
    cached,
    'Exact immutable cache survives epoch invalidation'
  );
  const fresh = pool.request(request('b'));
  workers[0].complete(33);
  assert.equal(pool.stats().staleDrops, 1);
  assert.equal(pool.stats().bytes, 16);
  assert.equal(workers[0].requests.at(-1).input.key, 'b');
  workers[0].complete(57);
  assert.equal((await fresh).data[0], 57);
  const waiting = pool.request(request('dispose-active')),
    pending = pool.request(request('dispose-queued'));
  const r1 = assert.rejects(waiting, { name: 'AbortError' }),
    r2 = assert.rejects(pending, { name: 'AbortError' });
  pool.dispose();
  await Promise.all([r1, r2]);
  assert.deepEqual(
    {
      bytes: pool.stats().bytes,
      active: pool.stats().active,
      queued: pool.stats().queued,
      workers: pool.stats().workers
    },
    { bytes: 0, active: 0, queued: 0, workers: 0 }
  );
});

test('new forecast windows remove obsolete queued background jobs while preserving active and promoted foreground work', async (t) => {
  const { pool, workers } = setup(t, { workers: 1 });
  pool.prefetch(['a', 'b', 'c', 'd'].map((key) => request(key)));
  const promoted = pool.request(request('c'));
  pool.prefetch(['e', 'f'].map((key) => request(key)));
  assert.equal(pool.stats().active, 1);
  assert.equal(pool.stats().queued, 3);
  assert.equal(
    pool.stats().staleDrops,
    2,
    'Old pending b/d forecasts are dropped without native work'
  );
  workers[0].complete();
  assert.equal(workers[0].requests.at(-1).input.key, 'c');
  workers[0].complete();
  await promoted;
  assert.equal(workers[0].requests.at(-1).input.key, 'e');
  pool.prefetch([]);
  assert.equal(pool.stats().queued, 0);
  assert.equal(pool.stats().active, 1, 'Already active e finishes naturally');
  workers[0].complete();
  const removed = pool.request(request('b'));
  assert.equal(workers[0].requests.at(-1).input.key, 'b', 'Removed forecast was never installed');
  workers[0].complete();
  await removed;
});

test('entry/byte LRU stays bounded and oversized foreground frames are delivered without caching', async (t) => {
  const { pool, workers } = setup(t, { workers: 1, maxBytes: 48, maxEntries: 2 });
  async function complete(key, size = {}) {
    const promise = pool.request(request(key, size));
    workers[0].complete();
    return promise;
  }
  await complete('a');
  await complete('b');
  await pool.request(request('a'));
  await complete('c');
  assert.equal(pool.stats().bytes, 32);
  const evicted = pool.request(request('b'));
  assert.equal(workers[0].requests.at(-1).input.key, 'b');
  workers[0].complete();
  await evicted;
  const large = await complete('large', { width: 4, height: 4 });
  assert.equal(large.data.byteLength, 64);
  assert.equal(pool.stats().bytes, 32);
  const uncached = pool.request(request('large', { width: 4, height: 4 }));
  assert.equal(workers[0].requests.at(-1).input.key, 'large');
  workers[0].complete();
  await uncached;
});

test('real template errors surface; worker failures/malformed frames settle and permit fresh-worker recovery', async (t) => {
  const { pool, workers } = setup(t, { workers: 1 });
  pool.prefetch([request('background-invalid')]);
  workers[0].fail('Template resource missing');
  const broken = pool.request(request('foreground-invalid')),
    rejection = assert.rejects(
      broken,
      (error) =>
        error.message === 'Template resource missing' &&
        !(error instanceof TextWorkerUnavailableError)
    );
  workers[0].fail('Template resource missing');
  await rejection;
  const crash = pool.request(request('crash')),
    crashed = assert.rejects(crash, { code: 'TEXT_WORKER_UNAVAILABLE' });
  workers[0].onerror({ message: 'Worker bootstrap failed', preventDefault() {} });
  await crashed;
  assert.equal(workers[0].terminated, true);
  const recover = pool.request(request('recover'));
  assert.equal(workers.length, 2);
  workers[1].complete();
  await recover;
  const malformed = pool.request(request('malformed')),
    malformedFailure = assert.rejects(malformed, { code: 'TEXT_WORKER_UNAVAILABLE' });
  const entry = workers[1].requests.at(-1),
    invalid = frame(entry.input);
  invalid.width = -2;
  workers[1].onmessage({ data: { id: entry.id, frame: invalid } });
  await malformedFailure;
  const undecodable = pool.request(request('message-error')),
    decodingFailure = assert.rejects(undecodable, { code: 'TEXT_WORKER_UNAVAILABLE' });
  workers[2].onmessageerror({});
  await decodingFailure;
  await assert.rejects(
    pool.request({ ...request('unsupported'), template: { id: 'studio-glow', version: 1 } }),
    /only flower/
  );
  assert.equal(pool.stats().active, 0);
  assert.equal(pool.stats().queued, 0);
});
