import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { createServer } from 'vite';
import vue from '@vitejs/plugin-vue';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';
import { Chromium } from '../packages/server/chromium.mjs';
import { pagxRuntimeAssets } from '../scripts/pagx-assets.mjs';
import { textTemplateAssets } from '../scripts/text-assets.mjs';
test(
  'PAGX inspector edits pixels, preserves IME and selection drafts, undo/redo, and portable saves',
  { timeout: 60000 },
  async () => {
    const directory = resolve('.local/pagx/properties-evidence');
    await mkdir(directory, { recursive: true });
    let api, web, browser;
    try {
      api = await startServer({
        port: 0,
        roots: [directory],
        visionModelDir: join(directory, 'vision')
      });
      web = await createServer({
        configFile: false,
        plugins: [vue(), pagxRuntimeAssets(), textTemplateAssets()],
        worker: { format: 'es' },
        server: {
          host: '127.0.0.1',
          port: 0,
          watch: null,
          hmr: false,
          proxy: {
            '/api': {
              target: api.url,
              changeOrigin: true,
              configure: (p) => p.on('proxyReq', (r) => r.removeHeader('origin'))
            }
          }
        }
      });
      await web.listen();
      browser = await new Chromium().start();
      const { targetId } = await browser.send('Target.createTarget', {
        url:
          'http://127.0.0.1:' +
          web.httpServer.address().port +
          '/tests/browser-pagx-properties.html'
      });
      const { sessionId } = await browser.send('Target.attachToTarget', {
        targetId,
        flatten: true
      });
      const evaluate = async (expression) => {
        const r = await browser.send(
          'Runtime.evaluate',
          { expression, awaitPromise: true, returnByValue: true },
          sessionId
        );
        if (r.exceptionDetails) throw Error(r.exceptionDetails.exception?.description);
        return r.result.value;
      };
      for (let i = 0; i < 300 && !(await evaluate('typeof window.runProperties==="function"')); i++)
        await new Promise((r) => setTimeout(r, 50));
      const result = await evaluate('window.runProperties()');
      await writeFile(join(directory, 'report.json'), JSON.stringify(result, null, 2));
      assert.ok(!result.error, result.error);
      const shot = await browser.send('Page.captureScreenshot', { format: 'png' }, sessionId);
      await writeFile(join(directory, 'inspector.png'), Buffer.from(shot.data, 'base64'));
      const client = new VideoCutClient(api.url);
      await client.connect();
      const session = await client.createSession(result.project);
      const saved = await client.saveProject(session.id, session.version, directory);
      const reopened = await client.openProject(saved.path);
      assert.deepEqual(reopened.project.timeline, session.project.timeline);
    } finally {
      await browser?.close();
      await web?.close();
      await api?.close();
    }
  }
);
