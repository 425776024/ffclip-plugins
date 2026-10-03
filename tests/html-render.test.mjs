import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { createProject, addText, ticks } from '../packages/core/project.mjs';

async function load(path) {
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
}
const { HtmlFrameClient, htmlSourceTime } = await load('packages/render/html.ts');
const { SceneGraph, visualFrameIdentity } = await load('packages/render/graph.ts');
const content = () => ({
  html: '<div id="title"></div><script>window.tick=t=>title.textContent=t</script>',
  width: 320,
  height: 180,
  duration: ticks(2),
  transparent: true,
  variables: { title: 'Overlay', color: '#ff8800' }
});
function mocks(t, fetch, bitmap) {
  for (const [key, value] of Object.entries({ fetch, createImageBitmap: bitmap })) {
    const previous = Object.getOwnPropertyDescriptor(globalThis, key);
    Object.defineProperty(globalThis, key, { value, configurable: true, writable: true });
    t.after(() =>
      previous ? Object.defineProperty(globalThis, key, previous) : delete globalThis[key]
    );
  }
}
const bootstrap = () => Response.json({ token: 'local-token' });
const png = () => new Response(new Blob(['png'], { type: 'image/png' }));
const waitFor = async (predicate) => {
  for (let index = 0; index < 1000 && !predicate(); index++)
    await new Promise((resolve) => setTimeout(resolve, 1));
  assert.ok(predicate(), 'Expected asynchronous capture/decode completion');
};

test('HTML export lookahead overlaps capture without cancelling current or active successor frames', async (t) => {
  const calls = [],
    bitmaps = [];
  mocks(
    t,
    async (url, init) => {
      if (String(url).endsWith('/bootstrap')) return bootstrap();
      return new Promise((resolve) =>
        calls.push({ ...JSON.parse(init.body), signal: init.signal, resolve })
      );
    },
    async () => {
      const bitmap = {
        width: 320,
        height: 180,
        closed: 0,
        close() {
          this.closed++;
        }
      };
      bitmaps.push(bitmap);
      return bitmap;
    }
  );
  const client = new HtmlFrameClient('http://127.0.0.1:4318/api/html-frames');
  t.after(() => client.dispose());
  const html = content(),
    current = client.frame('clip', html, 0);
  client.prefetch([1, 2, 3].map((time) => ({ itemId: 'clip', html, sourceTime: time })));
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(
    calls.map((call) => call.time),
    [0, 1, 2],
    'Only two successors dispatch'
  );
  client.retain(new Set(['clip']));
  assert.ok(
    calls.every((call) => !call.signal.aborted),
    'Retaining the clip preserves its predictive requests'
  );
  calls[0].resolve(png());
  (await current).close();
  const successor = client.frame('clip', html, 1);
  calls[1].resolve(png());
  const frame = await successor;
  assert.equal(frame.tickTime, 1);
  assert.equal(calls.length, 3, 'Consuming an in-flight successor never re-fetches it');
  frame.close();
  frame.close();
  calls[2].resolve(png());
  await waitFor(() => bitmaps.length === 3);
  client.prefetch([]);
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(
    bitmaps.map((bitmap) => bitmap.closed),
    [1, 1, 1],
    'Both consumed and unused snapshots close once'
  );
});

test('HTML export lookahead reserves full RGBA bytes, invalidates source edits and releases late cancelled bitmaps', async (t) => {
  const calls = [],
    bitmaps = [];
  const completeBitmaps = [];
  mocks(
    t,
    async (url, init) => {
      if (String(url).endsWith('/bootstrap')) return bootstrap();
      calls.push({ ...JSON.parse(init.body), signal: init.signal });
      return png();
    },
    () =>
      new Promise((resolve) => {
        completeBitmaps.push(() => {
          const bitmap = {
            width: 4096,
            height: 2160,
            closed: 0,
            close() {
              this.closed++;
            }
          };
          bitmaps.push(bitmap);
          resolve(bitmap);
        });
      })
  );
  const client = new HtmlFrameClient('http://127.0.0.1:4318/api/html-frames');
  t.after(() => client.dispose());
  const html = { ...content(), width: 4096, height: 2160 };
  client.prefetch([1, 2].map((sourceTime) => ({ itemId: 'clip', html, sourceTime })));
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(calls.length, 1, 'Two full 4096 x 2160 RGBA allocations exceed the 64 MiB limit');
  await waitFor(() => completeBitmaps.length === 1);
  client.prefetch([
    { itemId: 'clip', html: { ...html, variables: { title: 'Edited' } }, sourceTime: 1 }
  ]);
  assert.equal(
    calls[0].signal.aborted,
    true,
    'A changed source cannot reuse the old predicted pixels'
  );
  completeBitmaps[0]();
  await waitFor(() => completeBitmaps.length === 2 && bitmaps[0]?.closed === 1);
  assert.equal(bitmaps[0].closed, 1, 'The cancelled old source releases its late bitmap');
  client.dispose();
  completeBitmaps[1]();
  await waitFor(() => bitmaps.length === 2 && bitmaps[1].closed === 1);
  assert.ok(bitmaps.every((bitmap) => bitmap.closed === 1));
});

test('HTML predictive errors are delayed until their exact frame and cancelled jobs never fetch successors', async (t) => {
  const calls = [];
  mocks(
    t,
    async (url, init) => {
      if (String(url).endsWith('/bootstrap')) return bootstrap();
      calls.push(JSON.parse(init.body));
      return Response.json({ error: 'future tick failed' }, { status: 500 });
    },
    async () => {
      throw new Error('No failed capture should decode');
    }
  );
  const client = new HtmlFrameClient('http://127.0.0.1:4318/api/html-frames');
  t.after(() => client.dispose());
  const html = content();
  client.prefetch([{ itemId: 'clip', html, sourceTime: 1 }]);
  await new Promise((resolve) => setImmediate(resolve));
  await assert.rejects(client.frame('clip', html, 1), /future tick failed/);
  assert.equal(calls.length, 1, 'The failed predictive request is consumed rather than retried');
  const cancel = new AbortController();
  cancel.abort();
  client.prefetch([{ itemId: 'clip', html, sourceTime: 2 }], cancel.signal);
  assert.equal(calls.length, 1);
});

test('HTML scene identity samples source time and invalidates HTML, variables and alpha edits', () => {
  const project = createProject(),
    item = addText(project, { content: 'HTML', length: ticks(2) });
  item.clip.type = 'html-clip';
  item.clip.html = content();
  delete item.clip.text;
  const graph = new SceneGraph();
  const identity = (time = 0) =>
    visualFrameIdentity(graph.evaluate(project, time, 640, 360), () => '');
  const first = identity();
  assert.notEqual(identity(ticks(0.5)), first);
  item.clip.audio.gainLinear = 0.2;
  assert.equal(identity(), first, 'Audio parameters are independent of the DOM frame');
  item.clip.html.variables.title = 'Edited';
  const variables = identity();
  assert.notEqual(variables, first);
  item.clip.html.transparent = false;
  const opaque = identity();
  assert.notEqual(opaque, variables);
  item.clip.html.html += '<div>Added</div>';
  assert.notEqual(identity(), opaque);
  assert.equal(graph.builds, 1, 'HTML edits preserve compiled scene topology');
  assert.equal(htmlSourceTime(item.clip.html, -10), 0);
  assert.equal(htmlSourceTime(item.clip.html, ticks(3)), ticks(2) - 1);
});

test('HTML frames authenticate, clamp timeline samples and release bitmaps once', async (t) => {
  const calls = [],
    bitmaps = [];
  mocks(
    t,
    async (url, init) => {
      calls.push({ url: String(url), init });
      return String(url).endsWith('/bootstrap') ? bootstrap() : png();
    },
    async () => {
      const bitmap = {
        width: 320,
        height: 180,
        closed: 0,
        close() {
          this.closed++;
        }
      };
      bitmaps.push(bitmap);
      return bitmap;
    }
  );
  const client = new HtmlFrameClient('http://127.0.0.1:4318/api/html-frames');
  t.after(() => client.dispose());
  const html = content(),
    first = await client.frame('clip', html, -10);
  assert.equal(first.tickTime, 0);
  assert.equal(first.transparent, true);
  first.close();
  first.close();
  assert.equal(bitmaps[0].closed, 1);
  const second = await client.frame('clip', html, ticks(3));
  assert.equal(second.tickTime, ticks(2) - 1);
  assert.notEqual(first.identity, second.identity);
  second.close();
  const edited = await client.frame('clip', { ...html, variables: { title: 'Changed' } }, 0);
  assert.notEqual(edited.identity, first.identity);
  edited.close();
  assert.equal(calls.filter((c) => c.url.endsWith('/bootstrap')).length, 1);
  const requests = calls.filter((c) => !c.url.endsWith('/bootstrap'));
  assert.equal(requests[0].init.headers.Authorization, 'Bearer local-token');
  assert.deepEqual(JSON.parse(requests[0].init.body), { html, time: 0 });
  assert.equal(JSON.parse(requests[1].init.body).time, ticks(2) - 1);
});

test('HTML frames reject mismatched or downsampled source pixels', async (t) => {
  let closed = 0;
  mocks(
    t,
    async (url) => (String(url).endsWith('/bootstrap') ? bootstrap() : png()),
    async () => ({
      width: 160,
      height: 90,
      close() {
        closed++;
      }
    })
  );
  const client = new HtmlFrameClient('http://127.0.0.1:4318/api/html-frames');
  t.after(() => client.dispose());
  await assert.rejects(client.frame('clip', content(), ticks(0.5)), /尺寸与模板不一致/);
  assert.equal(closed, 1);
});

test('Superseded or inactive HTML seeks cancel capture requests', async (t) => {
  const captures = [];
  mocks(
    t,
    async (url, init) => {
      if (String(url).endsWith('/bootstrap')) return bootstrap();
      return new Promise((resolve, reject) => {
        captures.push({ resolve, signal: init.signal });
        init.signal.addEventListener(
          'abort',
          () => reject(new DOMException('cancelled', 'AbortError')),
          { once: true }
        );
      });
    },
    async () => ({ width: 320, height: 180, close() {} })
  );
  const client = new HtmlFrameClient('http://127.0.0.1:4318/api/html-frames');
  t.after(() => client.dispose());
  const first = client.frame('clip', content(), 0);
  const rejectedFirst = assert.rejects(first, { name: 'AbortError' });
  while (captures.length < 1) await new Promise((resolve) => setImmediate(resolve));
  const second = client.frame('clip', content(), 10);
  const rejectedSecond = assert.rejects(second, { name: 'AbortError' });
  while (captures.length < 2) await new Promise((resolve) => setImmediate(resolve));
  assert.equal(captures[0].signal.aborted, true);
  client.retain(new Set());
  assert.equal(captures[1].signal.aborted, true);
  await Promise.all([rejectedFirst, rejectedSecond]);
});

test('HTML bitmap completion after cancellation or invalid dimensions closes its allocation', async (t) => {
  let complete,
    bitmap = {
      width: 320,
      height: 180,
      closed: 0,
      close() {
        this.closed++;
      }
    };
  mocks(
    t,
    async (url) => (String(url).endsWith('/bootstrap') ? bootstrap() : png()),
    () =>
      new Promise((resolve) => {
        complete = resolve;
      })
  );
  const client = new HtmlFrameClient('http://127.0.0.1:4318/api/html-frames'),
    controller = new AbortController();
  t.after(() => client.dispose());
  const frame = client.frame('clip', content(), 0, controller.signal);
  const rejected = assert.rejects(frame, { name: 'AbortError' });
  while (!complete) await new Promise((resolve) => setImmediate(resolve));
  controller.abort();
  complete(bitmap);
  await rejected;
  assert.equal(bitmap.closed, 1);
  bitmap = {
    width: 10,
    height: 10,
    closed: 0,
    close() {
      this.closed++;
    }
  };
  complete = undefined;
  const invalid = client.frame('clip', content(), 0);
  const dimensions = assert.rejects(invalid, /尺寸与模板不一致/);
  while (!complete) await new Promise((resolve) => setImmediate(resolve));
  complete(bitmap);
  await dimensions;
  assert.equal(bitmap.closed, 1);
  client.dispose();
  await assert.rejects(client.frame('clip', content(), 0), /已关闭/);
});

test('HTML client refreshes expired credentials and reports server capture errors', async (t) => {
  let bootstraps = 0,
    captures = 0;
  mocks(
    t,
    async (url) => {
      if (String(url).endsWith('/bootstrap')) {
        bootstraps++;
        return bootstrap();
      }
      return ++captures === 1
        ? Response.json({ error: 'expired' }, { status: 401 })
        : Response.json({ error: 'tick failed' }, { status: 400 });
    },
    async () => {
      throw new Error('Unexpected bitmap decode');
    }
  );
  const client = new HtmlFrameClient('http://127.0.0.1:4318/api/html-frames');
  t.after(() => client.dispose());
  await assert.rejects(client.frame('clip', content(), 0), /tick failed/);
  assert.equal(bootstraps, 2);
  assert.equal(captures, 2);
});
