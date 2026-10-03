import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, realpath, rm, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer } from 'vite';
import vue from '@vitejs/plugin-vue';
import { textTemplateAssets } from '../scripts/text-assets.mjs';
import { Chromium } from '../packages/server/chromium.mjs';
import { startServer } from '../packages/server/index.mjs';
import { revealCommand } from '../packages/server/reveal-output.mjs';
import { VideoCutClient, createProject, addText, ticks } from '../packages/client/index.mjs';

test('file manager commands preserve spaces and punctuation as literal arguments', () => {
  const path = '/作品目录/视频 "one" & two.mp4';
  assert.deepEqual(revealCommand(path, 'darwin'), ['open', ['-R', path]]);
  assert.deepEqual(revealCommand(path, 'linux'), ['xdg-open', ['/作品目录']]);
  assert.deepEqual(revealCommand('C:\\作品目录\\one & two.mp4', 'win32'), [
    'explorer.exe',
    ['/select,C:\\作品目录\\one & two.mp4']
  ]);
});

test(
  'real editor exports show one playable completion dialog and project saves show folder actions',
  { timeout: 60000 },
  async (t) => {
    const root = await realpath(await mkdtemp(join(tmpdir(), 'videocut-complete-')));
    let api, web, browser;
    t.after(async () => {
      await browser?.close();
      await api?.close();
      await web?.close();
      await rm(root, { recursive: true, force: true });
    });
    api = await startServer({ port: 0, roots: [root], visionModelDir: join(root, 'vision') });
    const client = new VideoCutClient(api.url);
    await client.request('/vision/setup', { method: 'POST', body: '{"enabled":false}' });
    const project = createProject('完成窗口 & 中文');
    project.canvas = { width: 320, height: 180 };
    project.frameRate = { numerator: 10, denominator: 1 };
    addText(project, { content: 'Export preview', fontSize: 24, length: ticks(0.5) });
    const session = await client.createSession(project);
    web = await createServer({
      configFile: false,
      cacheDir: join(root, '.vite'),
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
              proxy.on('proxyReq', (req) => req.removeHeader('origin'));
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
    const evaluate = async (expression, userGesture = false) => {
      const result = await send('Runtime.evaluate', {
        expression,
        userGesture,
        awaitPromise: true,
        returnByValue: true
      });
      if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description);
      return result.result.value;
    };
    const wait = async (expression, message, timeout = 15000) => {
      const deadline = Date.now() + timeout;
      while (!(await evaluate(expression))) {
        assert.ok(
          Date.now() < deadline,
          `${message}: ${await evaluate('document.body.innerText.slice(-1500)')}`
        );
        await new Promise((resolve) => setTimeout(resolve, 50));
      }
    };
    await send('Emulation.setDeviceMetricsOverride', {
      width: 1280,
      height: 900,
      deviceScaleFactor: 1,
      mobile: false
    });
    await send('Page.navigate', {
      url: `http://127.0.0.1:${web.httpServer.address().port}/?session=${session.id}`
    });
    await wait('!!document.querySelector(".connection.online")', 'Editor connection');
    await evaluate(`window.completionOpens = 0;
      new MutationObserver((records) => { for (const record of records)
        if (record.attributeName === 'open' && record.oldValue === null) window.completionOpens++;
      }).observe(document.querySelector('.export-dialog'), { attributes: true, attributeOldValue: true });
      document.querySelector('.export-button').click();`);
    await wait(
      '!!document.querySelector(".export-dialog[open] .export-progress progress")',
      'Export progress is in a dialog'
    );
    assert.equal(await evaluate('!!document.querySelector(".message.notice")'), false);
    await send('Input.dispatchKeyEvent', {
      type: 'keyDown',
      key: 'Escape',
      code: 'Escape',
      windowsVirtualKeyCode: 27
    });
    await send('Input.dispatchKeyEvent', {
      type: 'keyUp',
      key: 'Escape',
      code: 'Escape',
      windowsVirtualKeyCode: 27
    });
    assert.equal(
      await evaluate('!!document.querySelector(".export-dialog[open]")'),
      true,
      await evaluate(
        'JSON.stringify({body:document.body.innerText.slice(-1200), dialog:document.querySelector(".export-dialog").outerHTML})'
      )
    );
    await wait(
      '!!document.querySelector(".export-complete-dialog[open]")',
      'Automatic completion dialog',
      30000
    );
    await wait(
      'document.querySelector(".export-result video")?.readyState >= 2',
      'Exported video decodes'
    );
    assert.equal(await evaluate('window.completionOpens'), 1);
    assert.equal(await evaluate('document.querySelector(".export-result video").videoWidth'), 320);
    assert.equal(await evaluate('document.querySelector(".export-result video").videoHeight'), 180);
    const path = await evaluate('document.querySelector(".export-result-path").textContent');
    assert.ok(path.endsWith('.mp4'));
    assert.equal(
      await evaluate('document.querySelector(".export-result-actions .primary").disabled'),
      false
    );
    await evaluate('document.querySelector(".export-result video").play()', true);
    await wait(
      'document.querySelector(".export-result video").currentTime > 0.1',
      'Exported video plays'
    );
    const url = client.exportOutputUrl(session.id, path);
    assert.equal((await fetch(url.replace(/&token=.*/, ''))).status, 401);
    assert.equal((await fetch(url, { headers: { Origin: 'https://other.example' } })).status, 403);
    const range = await fetch(url, { headers: { Range: 'bytes=0-31' } });
    assert.equal(range.status, 206);
    assert.deepEqual(
      Buffer.from(await range.arrayBuffer()),
      (await readFile(path)).subarray(0, 32)
    );
    assert.equal((await fetch(url, { method: 'HEAD' })).status, 200);
    const other = await client.createSession();
    assert.equal((await fetch(client.exportOutputUrl(other.id, path))).status, 404);
    const unrelated = join(root, 'unrelated.mp4');
    await writeFile(unrelated, 'private local file');
    assert.equal((await fetch(client.exportOutputUrl(session.id, unrelated))).status, 404);
    await assert.rejects(
      client.revealExport(session.id, unrelated),
      (error) => error.status === 404
    );
    await assert.rejects(client.revealExport(other.id, path), (error) => error.status === 404);

    // Finishing HTTP and SSE notifications must not reopen a dismissed dialog.
    await wait('!document.querySelector(".export-button").disabled', 'Render request settles');
    await evaluate('document.querySelector(".export-result-actions button:last-child").click()');
    await new Promise((resolve) => setTimeout(resolve, 300));
    assert.equal(
      await evaluate('!!document.querySelector(".export-complete-dialog[open]")'),
      false
    );
    assert.equal(await evaluate('window.completionOpens'), 1);
    await evaluate('document.querySelector(".export-settings-button").click()');
    await wait('!!document.querySelector(".export-menu")', 'Export settings opens');
    await evaluate('document.querySelectorAll(".export-menu > button")[1].click()');
    await wait(
      '!!document.querySelector(".export-complete-dialog[open]")',
      'Saved project completion dialog'
    );
    assert.equal(await evaluate('window.completionOpens'), 2);
    assert.equal(await evaluate('!!document.querySelector(".export-result video")'), false);
    const saved = await evaluate('document.querySelector(".export-result-path").textContent');
    assert.ok(saved.endsWith('.vcutweb'));
    assert.equal((await fetch(client.exportOutputUrl(session.id, saved))).status, 400);
    assert.equal(
      await evaluate('document.querySelector(".export-result-actions .primary").disabled'),
      false
    );
    await send('Input.dispatchKeyEvent', {
      type: 'keyDown',
      key: 'Escape',
      code: 'Escape',
      windowsVirtualKeyCode: 27
    });
    await send('Input.dispatchKeyEvent', {
      type: 'keyUp',
      key: 'Escape',
      code: 'Escape',
      windowsVirtualKeyCode: 27
    });
    await wait(
      '!document.querySelector(".export-complete-dialog[open]")',
      'Escape dismisses dialog'
    );
    assert.equal((await client.getSession(session.id)).project.timeline.tracks[0].items.length, 1);
    await evaluate('document.querySelector(".export-button").click()');
    await wait(
      '!!document.querySelector(".export-dialog[open] .export-progress button")',
      'Cancellation is available'
    );
    await evaluate('document.querySelector(".export-progress button").click()');
    await wait(
      '!document.querySelector(".export-dialog[open]") && !document.querySelector(".export-button").disabled',
      'Cancelled export settles'
    );
    assert.equal((await client.renderStatus(session.id)).error, '用户取消导出');
    assert.equal(
      await evaluate(
        'document.querySelector(".message.notice").textContent.includes("已取消导出")'
      ),
      true
    );
    await evaluate('document.querySelector(".export-button").click()');
    await wait(
      '!!document.querySelector(".export-complete-dialog[open]")',
      'Export can restart after cancellation'
    );
    assert.equal(await evaluate('window.completionOpens'), 4);
  }
);
