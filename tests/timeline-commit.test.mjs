import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, realpath, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer } from 'vite';
import vue from '@vitejs/plugin-vue';
import { textTemplateAssets } from '../scripts/text-assets.mjs';
import { Chromium } from '../packages/server/chromium.mjs';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';
import { createProject, addText, ticks, findItem } from '../packages/core/project.mjs';

test(
  'timeline release retains clip and handle geometry until the command settles',
  { timeout: 60000 },
  async (t) => {
    const directory = await realpath(await mkdtemp(join(tmpdir(), 'videocut-timeline-commit-')));
    let api, web, browser;
    t.after(async () => {
      await browser?.close();
      await web?.close();
      await api?.close();
      await rm(directory, { recursive: true, force: true });
    });
    api = await startServer({
      port: 0,
      roots: [directory],
      visionModelDir: join(directory, 'vision')
    });
    const client = new VideoCutClient(api.url);
    await client.request('/vision/setup', {
      method: 'POST',
      body: JSON.stringify({ enabled: false })
    });
    web = await createServer({
      configFile: false,
      plugins: [vue(), textTemplateAssets()],
      worker: { format: 'es' },
      server: {
        host: '127.0.0.1',
        port: 0,
        proxy: {
          '/api': {
            target: api.url,
            changeOrigin: true,
            configure(proxy) {
              proxy.on('proxyReq', (request) => request.removeHeader('origin'));
            }
          }
        }
      }
    });
    await web.listen();
    browser = await new Chromium().start({ frameRateLimit: true });
    const { targetId } = await browser.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await browser.send('Target.attachToTarget', { targetId, flatten: true });
    const send = (method, params) => browser.send(method, params, sessionId);
    await send('Emulation.setDeviceMetricsOverride', {
      width: 1440,
      height: 1000,
      deviceScaleFactor: 1,
      mobile: false
    });
    const evaluate = async (expression) => {
      const result = await send('Runtime.evaluate', {
        expression,
        returnByValue: true,
        awaitPromise: true
      });
      assert.equal(
        result.exceptionDetails,
        undefined,
        result.exceptionDetails?.exception?.description
      );
      return result.result.value;
    };
    const wait = async (predicate, message) => {
      const deadline = Date.now() + 20000;
      while (!(await predicate())) {
        assert.ok(Date.now() < deadline, message);
        await new Promise((resolve) => setTimeout(resolve, 20));
      }
    };
    let session, item;
    const geometry = () =>
      evaluate(`(() => {
    const node = document.querySelector('.timeline-clip');
    const clip = node.getBoundingClientRect();
    const left = node.querySelector('.trim-handle.left').getBoundingClientRect();
    const right = node.querySelector('.trim-handle.right').getBoundingClientRect();
    return { left: parseFloat(node.style.left), width: parseFloat(node.style.width),
      leftHandle: left.x - clip.x, rightHandle: right.x - clip.x };
  })()`);
    const load = async (leftTrimmed = false) => {
      const project = createProject('Timeline commit regression');
      item = addText(project, { content: 'Trim fixture', start: ticks(2), length: ticks(6) });
      if (leftTrimmed) {
        item.placement.begin = ticks(3);
        item.clip.source.begin = ticks(1);
      }
      session = await client.createSession(project);
      await send('Page.navigate', {
        url: `http://127.0.0.1:${web.httpServer.address().port}/?session=${session.id}`
      });
      await wait(
        () =>
          evaluate(
            '!!document.querySelector(".timeline-clip") && !!document.querySelector(".connection.online")'
          ),
        'Editor did not connect'
      );
      await evaluate(`(() => {
      document.querySelector('.timeline-right button[aria-pressed]').click();
      const state = window.timelineCommitTest = { requests: 0, release: null, reject: false, recording: false, frames: [] };
      const original = window.fetch;
      window.fetch = async (input, init) => {
        if (String(input).includes('/sessions/${session.id}/commands')) {
          state.requests++;
          await new Promise(resolve => state.release = resolve);
          if (state.reject) return new Response(JSON.stringify({ error: 'Rejected timeline commit' }), { status: 500 });
        }
        return original(input, init);
      };
      const sample = () => {
        if (state.recording) {
          const node = document.querySelector('.timeline-clip');
          const clip = node.getBoundingClientRect();
          state.frames.push({ left: parseFloat(node.style.left), width: parseFloat(node.style.width),
            leftHandle: node.querySelector('.trim-handle.left').getBoundingClientRect().x - clip.x,
            rightHandle: node.querySelector('.trim-handle.right').getBoundingClientRect().x - clip.x });
        }
        requestAnimationFrame(sample);
      };
      requestAnimationFrame(sample);
    })()`);
    };
    const drag = async (mode, delta) => {
      const selector = mode === 'move' ? '.timeline-clip' : `.trim-handle.${mode}`;
      const point = await evaluate(`(() => {
      const rect = document.querySelector(${JSON.stringify(selector)}).getBoundingClientRect();
      return { x: rect.left + Math.min(rect.width / 2, 80), y: rect.top + rect.height / 2 };
    })()`);
      await send('Input.dispatchMouseEvent', {
        type: 'mousePressed',
        ...point,
        button: 'left',
        clickCount: 1
      });
      await send('Input.dispatchMouseEvent', {
        type: 'mouseMoved',
        x: point.x + delta / 2,
        y: point.y,
        button: 'left',
        buttons: 1
      });
      await wait(
        async () => JSON.stringify(await geometry()) !== JSON.stringify(baselineGeometry),
        'Drag did not present intermediate geometry'
      );
      // Release at a newer position than the last move to cover final pointer sampling.
      await send('Input.dispatchMouseEvent', {
        type: 'mouseReleased',
        x: point.x + delta,
        y: point.y,
        button: 'left',
        clickCount: 1
      });
      await evaluate('window.timelineCommitTest.recording = true');
      await wait(
        () =>
          evaluate(
            'window.timelineCommitTest.requests === 1 && window.timelineCommitTest.frames.length >= 5'
          ),
        'Command or animation frames did not arrive'
      );
    };
    const assertGeometry = (actual, expected, message) => {
      for (const key of Object.keys(expected))
        assert.ok(
          Math.abs(actual[key] - expected[key]) < 0.1,
          `${message}: ${key} ${actual[key]} != ${expected[key]}`
        );
    };
    let baselineGeometry;
    for (const mode of ['left', 'right', 'move']) {
      await t.test(
        `${mode} release keeps the final position during a delayed save and remains undoable`,
        async () => {
          await load();
          baselineGeometry = await geometry();
          const delta = mode === 'right' ? -65 : 65;
          const expected = {
            left: baselineGeometry.left + (mode === 'right' ? 0 : delta),
            width:
              baselineGeometry.width + (mode === 'right' ? delta : mode === 'left' ? -delta : 0),
            leftHandle: baselineGeometry.leftHandle,
            rightHandle:
              baselineGeometry.rightHandle +
              (mode === 'right' ? delta : mode === 'left' ? -delta : 0)
          };
          await drag(mode, delta);
          const held = await evaluate('window.timelineCommitTest.frames');
          held.forEach((frame) =>
            assertGeometry(frame, expected, 'Handle reverted before save completed')
          );
          assert.equal((await client.getSession(session.id)).version, 0);
          await evaluate('window.timelineCommitTest.release()');
          await wait(
            async () => (await client.getSession(session.id)).version === 1,
            'Edit was not saved'
          );
          await wait(
            () => evaluate('document.querySelector(".timeline-tools button").disabled === false'),
            'Commit did not settle'
          );
          await evaluate(
            'new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))'
          );
          const frames = await evaluate(
            'window.timelineCommitTest.recording = false; window.timelineCommitTest.frames'
          );
          frames.forEach((frame) =>
            assertGeometry(frame, expected, 'Handle jumped during snapshot handoff')
          );
          const saved = await client.getSession(session.id);
          const range = findItem(saved.project, item.id).item.placement;
          assert.equal(range.begin, ticks(mode === 'right' ? 2 : 3));
          assert.equal(range.end, ticks(mode === 'left' ? 8 : mode === 'right' ? 7 : 9));
          assert.equal(saved.history.undoCount, 1);
          await client.editSession(session.id, [{ action: 'undo' }], saved.version);
          await wait(
            async () =>
              Math.abs((await geometry()).left - baselineGeometry.left) < 0.1 &&
              Math.abs((await geometry()).width - baselineGeometry.width) < 0.1,
            'Undo did not restore handle geometry'
          );
          t.diagnostic(
            `${frames.length} animation frame samples retained the final ${mode} geometry`
          );
        }
      );
    }
    await t.test(
      'text left edge extends past its source origin and stops at timeline zero',
      async () => {
        await load(true);
        baselineGeometry = await geometry();
        await drag('left', -260);
        const expected = {
          left: 0,
          width: baselineGeometry.width + baselineGeometry.left,
          leftHandle: baselineGeometry.leftHandle,
          rightHandle: baselineGeometry.rightHandle + baselineGeometry.left
        };
        const frames = await evaluate('window.timelineCommitTest.frames');
        frames.forEach((frame) =>
          assertGeometry(frame, expected, 'Text stopped at its source origin')
        );
        await evaluate('window.timelineCommitTest.release()');
        await wait(
          async () => (await client.getSession(session.id)).version === 1,
          'Text extension was not saved'
        );
        const saved = await client.getSession(session.id);
        const extended = findItem(saved.project, item.id).item;
        assert.deepEqual(extended.placement, { begin: 0, end: ticks(8) });
        assert.deepEqual(extended.clip.source, { begin: 0, end: ticks(8) });
        await client.editSession(session.id, [{ action: 'undo' }], saved.version);
        await wait(
          async () =>
            Math.abs((await geometry()).left - baselineGeometry.left) < 0.1 &&
            Math.abs((await geometry()).width - baselineGeometry.width) < 0.1,
          'Undo did not restore the text range'
        );
      }
    );
    await t.test('rejected trim rolls back and the next gesture can save', async () => {
      await load();
      baselineGeometry = await geometry();
      await evaluate('window.timelineCommitTest.reject = true');
      await drag('right', -65);
      assertGeometry(
        await geometry(),
        {
          ...baselineGeometry,
          width: baselineGeometry.width - 65,
          rightHandle: baselineGeometry.rightHandle - 65
        },
        'Rejected trim reverted before its response'
      );
      await evaluate('window.timelineCommitTest.release()');
      await wait(
        () => evaluate('!!document.querySelector(".message.error")'),
        'Rejection was not reported'
      );
      await wait(
        async () => Math.abs((await geometry()).width - baselineGeometry.width) < 0.1,
        'Rejected trim was not discarded'
      );
      assert.equal((await client.getSession(session.id)).version, 0);
      await evaluate(
        'window.timelineCommitTest.reject = false; window.timelineCommitTest.requests = 0; window.timelineCommitTest.frames = []; window.timelineCommitTest.recording = false'
      );
      await drag('left', 65);
      await evaluate('window.timelineCommitTest.release()');
      await wait(
        async () => (await client.getSession(session.id)).version === 1,
        'Next gesture stayed blocked'
      );
    });
  }
);
