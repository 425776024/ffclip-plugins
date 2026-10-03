import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, readFile, readdir, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer } from 'node:http';
import { setTimeout as delay } from 'node:timers/promises';
import { PinnedModelStore } from '../packages/server/model-store.mjs';
import { MODEL_SOURCES, selectModelDownload } from '../packages/server/model-download.mjs';

const [upstream, mirror] = MODEL_SOURCES;
const repository = 'fixture/model',
  revision = '0123456789abcdef';
const content = {
  'config.json': Buffer.from('{"model":"fixture"}'),
  'onnx/model.onnx': Buffer.from('verified model bytes'),
  'tokenizer.json': Buffer.from('{"tokens":[]}')
};
const files = Object.fromEntries(
  Object.entries(content).map(([path, bytes]) => [
    path,
    {
      size: bytes.length,
      sha256: createHash('sha256').update(bytes).digest('hex')
    }
  ])
);

async function fixture(t, fetchImpl, selectedFiles = files) {
  const directory = await mkdtemp(join(tmpdir(), 'videocut-model-sources-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const store = new PinnedModelStore({
    directory,
    fetchImpl,
    files: selectedFiles,
    modelId: 'fixture',
    repository,
    revision
  });
  const progress = Object.entries(selectedFiles).map(([path, file]) => ({
    path,
    size: file.size,
    downloaded: 0
  }));
  const controller = new AbortController();
  store.install = { state: 'downloading', progress: 0, files: progress };
  return { store, controller, run: () => store.downloadAll(controller, progress) };
}
function requested(url) {
  const parsed = new URL(url);
  assert.ok(MODEL_SOURCES.includes(parsed.origin));
  assert.ok(parsed.pathname.startsWith(`/${repository}/resolve/${revision}/`));
  return { source: parsed.origin, path: parsed.pathname.split(`${revision}/`)[1] };
}
async function assertClean(store) {
  assert.ok(
    !(await readdir(await store.root(), { recursive: true })).some((path) =>
      path.includes('.partial-')
    )
  );
}
function selection(fetchImpl, options = {}) {
  return selectModelDownload({
    sources: MODEL_SOURCES,
    path: `${repository}/resolve/${revision}/config.json`,
    size: files['config.json'].size,
    fetchImpl,
    signal: new AbortController().signal,
    hedgeDelayMs: 10,
    timeoutMs: 1000,
    ...options
  });
}

test('unreachable upstream switches to mirror, reuses the successful source and keeps verified cache offline', async (t) => {
  const calls = [];
  const { store, run } = await fixture(t, async (url) => {
    const request = requested(url);
    calls.push(request);
    if (request.source === upstream) throw new TypeError('fetch failed: unreachable');
    return new Response(content[request.path]);
  });
  await run();
  assert.equal(store.status().state, 'ready');
  assert.equal(store.status().progress, 1);
  assert.deepEqual(
    calls.map(({ source }) => source),
    [upstream, mirror, mirror, mirror]
  );
  for (const path of Object.keys(files))
    assert.deepEqual(await readFile(await store.verifiedFile(path)), content[path]);
  store.fetchImpl = () => assert.fail('verified cache must not contact either endpoint');
  await run();
  await assertClean(store);
});

test('preferred mirror can fail later; individual missing resources switch back to upstream', async (t) => {
  const calls = [];
  const { store, run } = await fixture(t, async (url) => {
    const request = requested(url);
    calls.push(request);
    if (
      (request.path === 'config.json' && request.source === upstream) ||
      (request.path === 'onnx/model.onnx' && request.source === mirror)
    )
      return new Response('missing', { status: 404 });
    return new Response(content[request.path]);
  });
  await run();
  assert.deepEqual(
    calls.map(({ source }) => source),
    [upstream, mirror, mirror, upstream, upstream]
  );
  assert.equal(store.status().state, 'ready');
});

test('slow connection is hedged; mirror starts without waiting for upstream timeout and aborts the loser', async () => {
  let loserSignal;
  const download = await selection(async (url, { signal }) => {
    if (requested(url).source === upstream) {
      loserSignal = signal;
      return new Promise((_, reject) =>
        signal.addEventListener('abort', () => reject(signal.reason), { once: true })
      );
    }
    return new Response(content['config.json']);
  });
  assert.equal(download.source, mirror);
  assert.equal(loserSignal.aborted, true);
  assert.deepEqual(Buffer.from(download.first.value), content['config.json']);
  download.close();
});

test('reachable headers with a stalled body cannot win source selection; losing body is cancelled', async () => {
  let cancelled = false;
  const download = await selection(async (url) =>
    requested(url).source === upstream
      ? new Response(
          new ReadableStream({
            cancel() {
              cancelled = true;
            }
          }),
          { headers: { 'content-length': files['config.json'].size } }
        )
      : new Response(content['config.json'])
  );
  assert.equal(download.source, mirror);
  assert.equal(cancelled, true);
  download.close();
});

test('wrong digest, size, truncation and broken streams retry from zero on the other source', async (t) => {
  const bytes = content['config.json'];
  const responses = {
    hash: () => new Response(Buffer.alloc(bytes.length)),
    length: () => new Response(bytes, { headers: { 'content-length': bytes.length + 1 } }),
    oversized: () => new Response(Buffer.concat([bytes, bytes])),
    truncated: () => new Response(bytes.subarray(0, 5)),
    empty: () => new Response(new Uint8Array()),
    interrupted: () => {
      let first = true;
      return new Response(
        new ReadableStream(
          {
            pull(controller) {
              if (first) {
                first = false;
                controller.enqueue(bytes.subarray(0, 5));
              } else controller.error(new Error('connection reset'));
            }
          },
          { highWaterMark: 0 }
        )
      );
    }
  };
  for (const [name, badResponse] of Object.entries(responses))
    await t.test(name, async (t) => {
      const calls = [];
      const { store, run } = await fixture(
        t,
        async (url) => {
          const { source } = requested(url);
          calls.push(source);
          return source === upstream ? badResponse() : new Response(bytes);
        },
        { 'config.json': files['config.json'] }
      );
      await run();
      assert.deepEqual(calls, [upstream, mirror]);
      assert.equal(store.status().files[0].downloaded, bytes.length);
      assert.equal(store.status().progress, 1);
      assert.deepEqual(await readFile(await store.verifiedFile('config.json')), bytes);
      await assertClean(store);
    });
});

test('both corrupt sources fail with source details and never publish a partial model', async (t) => {
  const { store, run } = await fixture(
    t,
    async () => new Response(Buffer.alloc(files['config.json'].size)),
    { 'config.json': files['config.json'] }
  );
  await assert.rejects(run(), (error) => {
    assert.equal(error.code, 'MODEL_DOWNLOAD_FAILED');
    assert.ok(error.message.includes(upstream) && error.message.includes(mirror));
    assert.match(error.message, /校验/);
    return true;
  });
  assert.equal(await store.verifiedFile('config.json'), null);
  assert.equal(store.status().files[0].downloaded, 0);
  await assertClean(store);
});

test('connection and idle timeouts are bounded even when a transport ignores cancellation', async () => {
  await assert.rejects(
    selection(() => new Promise(() => {}), { timeoutMs: 25 }),
    /超时/
  );
  let cancelled = false;
  const download = await selection(
    async () =>
      new Response(
        new ReadableStream({
          start(controller) {
            controller.enqueue(content['config.json'].subarray(0, 5));
          },
          cancel() {
            cancelled = true;
          }
        })
      ),
    { timeoutMs: 25 }
  );
  try {
    await assert.rejects(download.read(), /超时/);
  } finally {
    download.close();
  }
  assert.equal(cancelled, true);
});

test('steady data resets the idle timeout so large files have no total-duration cutoff', async () => {
  const download = await selection(
    async () =>
      new Response(
        new ReadableStream(
          {
            start(controller) {
              controller.enqueue(new Uint8Array([1]));
            },
            async pull(controller) {
              await delay(10);
              controller.enqueue(new Uint8Array([1]));
            }
          },
          { highWaterMark: 0 }
        )
      ),
    { timeoutMs: 100 }
  );
  try {
    for (let i = 0; i < 15; i++) assert.equal((await download.read()).value.length, 1);
  } finally {
    download.close();
  }
});

test('cancel during selection aborts active requests and does not start a delayed alternative', async () => {
  const controller = new AbortController();
  let entered,
    calls = 0,
    activeSignal;
  const started = new Promise((resolve) => {
    entered = resolve;
  });
  const pending = selection(
    async (_url, { signal }) => {
      calls++;
      activeSignal = signal;
      entered();
      return new Promise(() => {});
    },
    { signal: controller.signal }
  );
  await started;
  controller.abort();
  await assert.rejects(pending, { name: 'AbortError' });
  assert.equal(activeSignal.aborted, true);
  await delay(20);
  assert.equal(calls, 1);
});

test('cancel after receiving bytes cleans the partial and never retries another source', async (t) => {
  let calls = 0,
    entered;
  const reading = new Promise((resolve) => {
    entered = resolve;
  });
  const { store, controller, run } = await fixture(
    t,
    async () => {
      calls++;
      return new Response(
        new ReadableStream(
          {
            start(controller) {
              controller.enqueue(content['config.json'].subarray(0, 5));
            },
            pull() {
              entered();
            }
          },
          { highWaterMark: 0 }
        )
      );
    },
    { 'config.json': files['config.json'] }
  );
  const pending = run();
  await reading;
  controller.abort();
  await assert.rejects(pending, { name: 'AbortError' });
  assert.equal(calls, 1);
  assert.equal(await store.verifiedFile('config.json'), null);
  await assertClean(store);
});

test('real HTTP redirects and chunked bodies preserve pinned paths and verified output across source failure', async (t) => {
  const requests = [];
  const server = createServer((req, res) => {
    requests.push(req.url);
    if (req.url.startsWith('/huggingface.co/')) {
      res.writeHead(503).end();
      return;
    }
    if (req.url.startsWith('/hf-mirror.com/')) {
      res.writeHead(302, { location: '/blob' }).end();
      return;
    }
    assert.equal(req.url, '/blob');
    res.write(content['config.json'].subarray(0, 5));
    res.end(content['config.json'].subarray(5));
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(
    () =>
      new Promise((resolve) => {
        server.closeAllConnections();
        server.close(resolve);
      })
  );
  const base = `http://127.0.0.1:${server.address().port}`;
  const { store, run } = await fixture(
    t,
    (url, options) => {
      const parsed = new URL(url);
      return fetch(`${base}/${parsed.hostname}${parsed.pathname}`, options);
    },
    { 'config.json': files['config.json'] }
  );
  await run();
  assert.deepEqual(requests, [
    `/huggingface.co/${repository}/resolve/${revision}/config.json`,
    `/hf-mirror.com/${repository}/resolve/${revision}/config.json`,
    '/blob'
  ]);
  assert.deepEqual(await readFile(await store.verifiedFile('config.json')), content['config.json']);
  await assertClean(store);
});
