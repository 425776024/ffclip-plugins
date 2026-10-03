import test from 'node:test';
import assert from 'node:assert/strict';
import { Chromium, ChromiumTransportError } from '../packages/server/chromium.mjs';
import { HtmlFrameRenderer } from '../packages/server/html-renderer.mjs';

const content = {
  html: '<div>Frame</div>',
  width: 160,
  height: 120,
  duration: 120000,
  transparent: true
};

test('one CDP timeout fails every pending command and refuses more commands on that connection', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const browser = new Chromium(),
    commands = [];
  browser.socket = { send: (message) => commands.push(JSON.parse(message)), close() {} };
  const context = browser.send('Target.createBrowserContext'),
    peer = browser.send('Page.captureScreenshot');
  const rejected = Promise.all(
    [context, peer].map((promise) =>
      assert.rejects(promise, (error) => {
        assert.equal(error.code, 'CHROMIUM_TIMEOUT');
        assert.equal(error.browser, browser);
        assert.equal(error.method, 'Target.createBrowserContext');
        return true;
      })
    )
  );
  t.mock.timers.tick(15000);
  await rejected;
  assert.equal(browser.pending.size, 0);
  await assert.rejects(browser.send('Target.disposeBrowserContext'), { code: 'CHROMIUM_TIMEOUT' });
  assert.equal(commands.length, 2, 'Cleanup never incurs another 15-second timeout');
  await browser.close();
});

test('a closed transport fails immediately and closing a signal-exited child never waits for another exit', async () => {
  const browser = new Chromium();
  browser.socket = {
    send() {
      throw new Error('socket closed');
    },
    close() {}
  };
  await assert.rejects(browser.send('Target.createBrowserContext'), {
    code: 'CHROMIUM_DISCONNECTED'
  });
  browser.process = {
    exitCode: null,
    signalCode: 'SIGTRAP',
    once() {
      assert.fail('The child has already emitted its exit');
    },
    kill() {
      assert.fail('Do not signal an exited process');
    }
  };
  const first = browser.close(),
    second = browser.close();
  assert.equal(first, second, 'Concurrent cleanup shares the same shutdown');
  await first;
});

class BrowserFixture {
  listeners = new Set();
  closed = 0;
  contexts = new Set();
  commands = [];
  sequence = 0;
  constructor(failContext = false) {
    this.failContext = failContext;
  }
  async send(method, params) {
    this.commands.push(method);
    if (this.failure) throw this.failure;
    if (method === 'Target.createBrowserContext') {
      if (this.failContext) {
        this.failure = new ChromiumTransportError(
          'HTML 渲染超时：Target.createBrowserContext',
          'CHROMIUM_TIMEOUT',
          this,
          method
        );
        throw this.failure;
      }
      const browserContextId = `context-${++this.sequence}`;
      this.contexts.add(browserContextId);
      return { browserContextId };
    }
    if (method === 'Target.createTarget') return { targetId: `target-${++this.sequence}` };
    if (method === 'Target.attachToTarget') return { sessionId: `session-${++this.sequence}` };
    if (method === 'Page.getFrameTree') return { frameTree: { frame: { id: 'frame' } } };
    if (method === 'Runtime.evaluate') return { result: { value: null } };
    if (method === 'Page.captureScreenshot')
      return { data: Buffer.from('owned-png').toString('base64') };
    if (method === 'Target.disposeBrowserContext') this.contexts.delete(params.browserContextId);
    return {};
  }
  async close() {
    this.closed++;
  }
}

test('two failed capture lanes rebuild one browser and both retry their exact frame once', async (t) => {
  const browsers = [];
  const renderer = new HtmlFrameRenderer({
    browserFactory: async () => {
      const browser = new BrowserFixture(browsers.length === 0);
      browsers.push(browser);
      return browser;
    }
  });
  t.after(() => renderer.close());
  const frames = await Promise.all([
    renderer.capture(content, 3000),
    renderer.capture(content, 6000)
  ]);
  assert.deepEqual(
    frames.map((frame) => frame.time),
    [3000, 6000]
  );
  assert.equal(browsers.length, 2);
  assert.equal(browsers[0].closed, 1);
  assert.ok([...renderer.pages.values()].every((page) => page.browser === browsers[1]));
  await renderer.recover(browsers[0]);
  assert.equal(
    renderer.browser,
    browsers[1],
    'A late failure cannot close the replacement browser'
  );
  assert.equal(browsers[1].closed, 0);
});

test('persistent transport failures have one retry; authored errors and cancelled frames have none', async () => {
  const browsers = [];
  const renderer = new HtmlFrameRenderer({
    browserFactory: async () => {
      const browser = new BrowserFixture(true);
      browsers.push(browser);
      return browser;
    }
  });
  await assert.rejects(renderer.capture(content, 0), { code: 'CHROMIUM_TIMEOUT' });
  assert.equal(browsers.length, 2);
  await renderer.close();
  assert.ok(browsers.every((browser) => browser.closed === 1));

  const authored = new HtmlFrameRenderer();
  let calls = 0;
  authored.frame = async () => {
    calls++;
    throw new Error('authored tick failed');
  };
  await assert.rejects(authored.capture(content, 0), /authored tick failed/);
  assert.equal(calls, 1);
  await authored.close();

  const cancel = new AbortController(),
    browser = new BrowserFixture(true);
  const cancelled = new HtmlFrameRenderer({
    browserFactory: async () => {
      cancel.abort();
      return browser;
    }
  });
  await assert.rejects(cancelled.capture(content, 0, cancel.signal), { name: 'AbortError' });
  assert.equal(browser.closed, 0, 'Cancellation cannot start an automatic retry');
  await cancelled.close();
});

test('cancelled page-lock waiters allocate nothing and closing during startup retires its late browser', async () => {
  const renderer = new HtmlFrameRenderer(),
    calls = [];
  let release;
  renderer.openPage = async (_content, key) => {
    calls.push(key);
    await new Promise((resolve) => {
      release = resolve;
    });
  };
  const first = renderer.page(content, 'first'),
    cancel = new AbortController();
  const second = assert.rejects(renderer.page(content, 'cancelled', cancel.signal), {
    name: 'AbortError'
  });
  await new Promise((resolve) => setImmediate(resolve));
  cancel.abort();
  release();
  await first;
  await second;
  assert.deepEqual(calls, ['first']);
  await renderer.close();

  const browser = new BrowserFixture();
  let finishStart;
  const starting = new HtmlFrameRenderer({
    browserFactory: () =>
      new Promise((resolve) => {
        finishStart = resolve;
      })
  });
  const capture = assert.rejects(starting.capture(content, 0), /已关闭/);
  await new Promise((resolve) => setImmediate(resolve));
  const shutdown = starting.close();
  finishStart(browser);
  await capture;
  await shutdown;
  assert.equal(browser.closed, 1);
  assert.equal(browser.commands.length, 0);
});

test('source churn reuses eight isolated contexts instead of repeatedly creating storage partitions', async (t) => {
  const browser = new BrowserFixture(),
    renderer = new HtmlFrameRenderer({ browserFactory: async () => browser });
  t.after(() => renderer.close());
  for (let index = 0; index < 24; index++)
    await renderer.capture({ ...content, variables: { index } }, 0);
  assert.equal(
    browser.commands.filter((method) => method === 'Target.createBrowserContext').length,
    8
  );
  assert.equal(browser.commands.filter((method) => method === 'Target.closeTarget').length, 16);
  assert.equal(browser.contexts.size, 8);
  assert.equal(renderer.pages.size, 8);
  assert.equal(browser.listeners.size, 8);
});
