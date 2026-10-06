import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { Chromium } from '../packages/server/chromium.mjs';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';
import { createProject, addText, ticks, editTimeline } from '../packages/core/project.mjs';

test(
  'Qt-style property keyframe buttons navigate, edit and undo without listing unrelated frames',
  { timeout: 45000 },
  async () => {
    const root = resolve('.local/editor-keyframes-test');
    await mkdir(root, { recursive: true });
    let project = createProject('关键帧按钮检查');
    project.canvas = { width: 640, height: 360 };
    const item = addText(project, { content: '由你掌控', start: ticks(2), length: ticks(6) });
    project = editTimeline(project, [
      ...[
        [0, 0],
        [0.32, 0],
        [0.95, 1],
        [5.5, 1],
        [5.94, 0]
      ].map(([timeSeconds, value]) => ({
        action: 'set_keyframe',
        itemId: item.id,
        property: 'visual.opacity',
        timeSeconds,
        value
      })),
      ...[
        [0, 120],
        [0.95, 98]
      ].map(([timeSeconds, value]) => ({
        action: 'set_keyframe',
        itemId: item.id,
        property: 'visual.positionY',
        timeSeconds,
        value
      })),
      { action: 'add_effect', itemId: item.id, templateId: 'lut' }
    ]).project;
    const server = await startServer({
      roots: [root],
      port: 0,
      initialProject: project,
      visionModelDir: root + '/unused-models'
    });
    const client = new VideoCutClient(server.url);
    await client.request('/vision/setup', {
      method: 'POST',
      body: JSON.stringify({ enabled: false })
    });
    const browser = await new Chromium().start({
      args: ['--enable-unsafe-webgpu', '--lang=zh-CN']
    });
    const errors = [],
      cases = [];
    try {
      const { targetId } = await browser.send('Target.createTarget', { url: 'about:blank' });
      const { sessionId } = await browser.send('Target.attachToTarget', {
        targetId,
        flatten: true
      });
      const send = (method, params = {}) => browser.send(method, params, sessionId);
      await send('Runtime.enable');
      browser.listeners.add((event) => {
        if (event.sessionId === sessionId && event.method === 'Runtime.exceptionThrown')
          errors.push(
            event.params.exceptionDetails.exception?.description ||
              event.params.exceptionDetails.text
          );
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
      const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
      const wait = async (expression) => {
        const deadline = Date.now() + 15000;
        while (!(await evaluate(expression))) {
          assert.ok(Date.now() < deadline, expression);
          await pause(50);
        }
      };
      const click = async (expression) => {
        await evaluate(`(${expression}).scrollIntoView({ block: 'nearest', inline: 'nearest' })`);
        const point = await evaluate(
          `(() => { const r = (${expression}).getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`
        );
        await send('Input.dispatchMouseEvent', {
          type: 'mousePressed',
          ...point,
          button: 'left',
          clickCount: 1
        });
        await send('Input.dispatchMouseEvent', {
          type: 'mouseReleased',
          ...point,
          button: 'left',
          clickCount: 1
        });
        await evaluate(
          'new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))'
        );
      };
      const tab = (label) =>
        `Array.from(document.querySelectorAll('.inspector-tabs button')).find(b => b.textContent.trim() === '${label}')`;
      const rail =
        "document.getElementById('visual.opacity').closest('.property-row').querySelector('.keyframe-controls')";
      const button = (action) => `(${rail}).querySelector('.keyframe-${action}')`;
      const seek = async (time) => {
        await evaluate(
          `(() => { const input = document.querySelector('.preview-scrub input'); input.value = ${ticks(time)}; input.dispatchEvent(new Event('input', { bubbles: true })); })()`
        );
        await pause(80);
      };
      const snapshot = () => client.getSession(server.initialSession.id);
      const opacityFrames = async () =>
        (await snapshot()).project.timeline.tracks[0].items[0].clip.automation['visual.opacity']
          .keyframes;
      await send('Emulation.setDeviceMetricsOverride', {
        width: 1440,
        height: 900,
        deviceScaleFactor: 1,
        mobile: false
      });
      await send('Emulation.setLocaleOverride', { locale: 'zh-CN' });
      await send('Page.navigate', { url: server.initialSession.previewUrl });
      await wait("!!document.querySelector('.timeline-clip')");
      await click("document.querySelector('.timeline-clip')");
      await wait("!!document.querySelector('.text-properties')");
      for (const width of [280, 320, 440]) {
        await evaluate(
          `document.querySelector('.workspace').style.gridTemplateColumns = 'minmax(210px, 23fr) minmax(300px, 52fr) ${width}px'`
        );
        await pause(80);
        assert.equal(
          await evaluate(
            "document.querySelectorAll('.inspector .keyframe-list, .inspector .keyframe-group').length"
          ),
          0
        );
        const textLayout = await evaluate(
          "({ textBottom: document.querySelector('.property-text-row').getBoundingClientRect().bottom, presetTop: document.querySelector('.text-properties').getBoundingClientRect().top })"
        );
        assert.ok(
          textLayout.presetTop >= textLayout.textBottom &&
            textLayout.presetTop - textLayout.textBottom < 12
        );
        await click(tab('画面'));
        const geometry = await evaluate(`(() => {
      const r = (${rail}).getBoundingClientRect();
      return { width: r.width, height: r.height, buttons: Array.from((${rail}).querySelectorAll('button'), node => ({ width: node.getBoundingClientRect().width, height: node.getBoundingClientRect().height })), overflow: document.querySelector('.inspector-body').scrollWidth > document.querySelector('.inspector-body').clientWidth };
    })()`);
        assert.equal(geometry.width, 48);
        assert.equal(geometry.height, 28);
        assert.deepEqual(geometry.buttons, Array(3).fill({ width: 16, height: 28 }));
        assert.equal(geometry.overflow, false);
        cases.push({ width, textLayout, geometry });
        await click(tab('文字'));
      }
      await click(tab('画面'));
      await seek(0);
      assert.equal(await evaluate(`(${button('toggle')}).disabled`), true);
      await click(button('next'));
      await wait(`(${button('toggle')}).getAttribute('aria-pressed') === 'true'`);
      assert.equal(
        await evaluate("Number(document.querySelector('.preview-scrub input')._value)"),
        ticks(2)
      );
      assert.equal(await evaluate(`(${button('previous')}).disabled`), true);
      await click(button('next'));
      assert.equal(
        await evaluate("Number(document.querySelector('.preview-scrub input')._value)"),
        ticks(2.32)
      );
      assert.equal(
        await evaluate(`(${button('toggle')}).getAttribute('aria-label')`),
        '不透明度 · 删除关键帧'
      );
      await click(button('toggle'));
      await wait(`(${button('toggle')}).getAttribute('aria-pressed') === 'false'`);
      assert.equal((await opacityFrames()).length, 4);
      assert.equal(await evaluate(`(${rail}).classList.contains('bound')`), true);
      const deleted = await snapshot();
      await client.editSession(deleted.id, [{ action: 'undo' }], deleted.version);
      await wait(`(${button('toggle')}).getAttribute('aria-pressed') === 'true'`);
      assert.equal((await opacityFrames()).length, 5);
      await click(button('next'));
      assert.equal(
        await evaluate("Number(document.querySelector('.preview-scrub input')._value)"),
        ticks(2.95)
      );
      assert.equal(await evaluate("Number(document.getElementById('visual.opacity').value)"), 1);
      await click(button('previous'));
      assert.equal(
        await evaluate("Number(document.querySelector('.preview-scrub input')._value)"),
        ticks(2.32)
      );
      await seek(3.4);
      assert.equal(await evaluate(`(${button('toggle')}).getAttribute('aria-pressed')`), 'false');
      await click(button('toggle'));
      await wait(`(${button('toggle')}).getAttribute('aria-pressed') === 'true'`);
      assert.equal((await opacityFrames()).length, 6);
      await evaluate(
        "(() => { const input = document.getElementById('visual.opacity'); input.value = '0.4'; input.dispatchEvent(new Event('change', { bubbles: true })); })()"
      );
      await pause(200);
      assert.equal((await opacityFrames()).find((frame) => frame.time === ticks(1.4)).value, 0.4);
      await click(button('toggle'));
      await wait(`(${button('toggle')}).getAttribute('aria-pressed') === 'false'`);
      assert.equal((await opacityFrames()).length, 5);
      await seek(7.9);
      await click(button('next'));
      assert.equal(
        await evaluate("Number(document.querySelector('.preview-scrub input')._value)"),
        ticks(7.94)
      );
      assert.equal(await evaluate(`(${button('next')}).disabled`), true);
      await click(tab('特效'));
      await wait("!!document.querySelector('.effect-parameter-row .keyframe-controls')");
      const effectToggle = "document.querySelector('.effect-parameter-row .keyframe-toggle')";
      await click(effectToggle);
      await wait(`(${effectToggle}).getAttribute('aria-pressed') === 'true'`);
      await click(effectToggle);
      await wait(`(${effectToggle}).getAttribute('aria-pressed') === 'false'`);
      const effectGeometry = await evaluate(
        "(() => { const row = document.querySelector('.effect-parameter-row:has(.keyframe-controls)'); return { buttons: row.querySelectorAll('.keyframe-controls button').length, overflow: row.scrollWidth > row.clientWidth }; })()"
      );
      assert.equal(effectGeometry.buttons, 3);
      assert.equal(effectGeometry.overflow, false);
      await click(tab('文字'));
      await evaluate("document.querySelector('.inspector-body').scrollTop = 0");
      await pause(200);
      const region = await evaluate(
        "(() => { const r = document.querySelector('.inspector').getBoundingClientRect(); return { x: r.x, y: r.y, width: r.width, height: r.height, scale: 1 }; })()"
      );
      let screenshot = await send('Page.captureScreenshot', { format: 'png', clip: region });
      await writeFile(
        root + '/keyframe-buttons-text-panel.png',
        Buffer.from(screenshot.data, 'base64')
      );
      await click(tab('画面'));
      await seek(2.2);
      await click(button('next'));
      await evaluate("document.querySelector('.inspector-body').scrollTop = 0");
      screenshot = await send('Page.captureScreenshot', { format: 'png', clip: region });
      await writeFile(
        root + '/keyframe-buttons-visual-panel.png',
        Buffer.from(screenshot.data, 'base64')
      );
      assert.deepEqual(errors, []);
      const result = {
        passed: true,
        cases,
        clipOffset: 2,
        previousNext: true,
        addDeleteEditUndo: true,
        boundaries: true,
        effectButtons: true,
        errors
      };
      await writeFile(root + '/keyframe-buttons-results.json', JSON.stringify(result, null, 2));
      console.log(JSON.stringify(result));
    } finally {
      await browser.close();
      await server.close();
    }
  }
);
