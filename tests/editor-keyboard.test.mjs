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
import { createProject, addText } from '../packages/core/project.mjs';

test(
  'editor deletion shortcuts follow timeline selection and preserve text editing',
  { timeout: 60000 },
  async (t) => {
    const directory = await realpath(await mkdtemp(join(tmpdir(), 'videocut-keyboard-')));
    let api, web, browser;
    t.after(async () => {
      await browser?.close();
      await api?.close();
      await web?.close();
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
    browser = await new Chromium().start();
    const { targetId } = await browser.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await browser.send('Target.attachToTarget', { targetId, flatten: true });
    const send = (method, params) => browser.send(method, params, sessionId);
    const errors = [];
    browser.listeners.add((event) => {
      if (event.sessionId === sessionId && event.method === 'Runtime.exceptionThrown')
        errors.push(
          event.params.exceptionDetails.exception?.description || event.params.exceptionDetails.text
        );
    });
    await send('Runtime.enable');
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
      assert.equal(result.exceptionDetails, undefined, result.exceptionDetails?.text);
      return result.result.value;
    };
    const wait = async (predicate, message) => {
      const deadline = Date.now() + 5000;
      while (!(await predicate())) {
        assert.ok(Date.now() < deadline, message);
        await new Promise((resolve) => setTimeout(resolve, 30));
      }
    };
    const click = async (selector, modifiers = 0) => {
      const point = await evaluate(`(() => {
      const node = document.querySelector(${JSON.stringify(selector)});
      if (!node) throw Error('Missing control: ' + ${JSON.stringify(selector)});
      const rect = node.getBoundingClientRect();
      return { x: rect.left + Math.min(rect.width / 2, 80), y: rect.top + rect.height / 2 };
    })()`);
      await send('Input.dispatchMouseEvent', {
        type: 'mousePressed',
        ...point,
        button: 'left',
        clickCount: 1,
        modifiers
      });
      await send('Input.dispatchMouseEvent', {
        type: 'mouseReleased',
        ...point,
        button: 'left',
        clickCount: 1,
        modifiers
      });
    };
    const key = async (key, modifiers = 0) => {
      const code = { Delete: 46, Backspace: 8, z: 90, ' ': 32 }[key];
      await send('Input.dispatchKeyEvent', {
        type: 'keyDown',
        key,
        code: key === ' ' ? 'Space' : key === 'z' ? 'KeyZ' : key,
        windowsVirtualKeyCode: code,
        modifiers,
        ...(key === ' ' ? { text: ' ' } : {})
      });
      await send('Input.dispatchKeyEvent', {
        type: 'keyUp',
        key,
        windowsVirtualKeyCode: code,
        modifiers
      });
    };
    let session;
    const load = async () => {
      const project = createProject('Keyboard regression');
      addText(project, { content: 'First clip' });
      addText(project, { content: 'Second clip' });
      session = await client.createSession(project);
      await send('Page.navigate', {
        url: `http://127.0.0.1:${web.httpServer.address().port}/?session=${session.id}`
      });
      try {
        await wait(
          () =>
            evaluate(
              'document.querySelectorAll(".timeline-clip").length === 2 && !!document.querySelector(".connection.online")'
            ),
          'Editor did not connect'
        );
      } catch (error) {
        throw new Error(
          `${error.message}: ${await evaluate('document.body.innerText.slice(0, 1500)')} ${errors.join('; ')}`
        );
      }
    };
    const count = async () =>
      (await client.getSession(session.id)).project.timeline.tracks.flatMap((track) => track.items)
        .length;
    const expectCount = (value) =>
      wait(async () => (await count()) === value, `Expected ${value} saved clips`);
    const select = async (index = 1, modifiers = 0) => {
      await click(`.track-row:nth-child(${index}) .timeline-clip`, modifiers);
      await wait(
        () => evaluate('!!document.querySelector(".timeline-clip.selected")'),
        'Clip was not selected'
      );
    };

    await t.test(
      'Delete works after a toolbar button and clip click; undo and redo restore the document',
      async () => {
        await load();
        await click('.timeline-right button');
        assert.equal(await evaluate('document.activeElement.tagName'), 'BUTTON');
        await select();
        await key('Delete');
        await expectCount(1);
        assert.equal(
          await evaluate('document.activeElement.classList.contains("timeline-scroll")'),
          true
        );
        await key('z', 2);
        await expectCount(2);
        await key('z', 2 | 8);
        await expectCount(1);
      }
    );
    await t.test(
      'Backspace works after a focused input, including additive selection',
      async () => {
        await load();
        await click('.project-name');
        await select();
        assert.equal(
          await evaluate('document.activeElement.classList.contains("timeline-scroll")'),
          true
        );
        await select(2, 8);
        await wait(
          () => evaluate('document.querySelectorAll(".timeline-clip.selected").length === 2'),
          'Additive selection failed'
        );
        await key('Backspace');
        await expectCount(0);
      }
    );
    await t.test('Delete also works while a timeline toolbar button retains focus', async () => {
      await load();
      await select();
      await click('.timeline-right button');
      await key('Delete');
      await expectCount(1);
    });
    await t.test(
      'Backspace edits an input without deleting the selected clip; reselecting restores the shortcut',
      async () => {
        await load();
        await select();
        await click('.project-name');
        await evaluate('document.activeElement.setSelectionRange(1, 1)');
        await key('Backspace');
        assert.equal(await evaluate('document.activeElement.value'), 'eyboard regression');
        assert.equal(await count(), 2);
        await select();
        await key('Backspace');
        await expectCount(1);
      }
    );
    await t.test('Space keeps native toolbar activation', async () => {
      await load();
      await click('.timeline-right button');
      const pressed = await evaluate('document.activeElement.getAttribute("aria-pressed")');
      await key(' ');
      await wait(
        () =>
          evaluate(
            `document.activeElement.getAttribute('aria-pressed') !== ${JSON.stringify(pressed)}`
          ),
        'Space did not activate the button'
      );
      assert.equal(await count(), 2);
    });
  }
);
