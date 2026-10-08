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
  'real Editor offers only native PAGX templates, filters and edits selected clips',
  { timeout: 60000 },
  async () => {
    const directory = resolve('.local/pagx/editor-evidence');
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
      const apiClient = new VideoCutClient(api.url);
      await apiClient.connect();
      const initial = await apiClient.createSession();
      const { targetId } = await browser.send('Target.createTarget', {
        url:
          'http://127.0.0.1:' +
          web.httpServer.address().port +
          '/tests/browser-animation-editor.html?session=' +
          initial.id
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
      for (let i = 0; i < 300 && !(await evaluate('typeof window.runEditor==="function"')); i++)
        await new Promise((r) => setTimeout(r, 50));
      const result = await evaluate('window.runEditor()');
      await writeFile(join(directory, 'report.json'), JSON.stringify(result, null, 2));
      assert.ok(!result.error, result.error);
      const shot = await browser.send('Page.captureScreenshot', { format: 'png' }, sessionId);
      await writeFile(join(directory, 'inspector.png'), Buffer.from(shot.data, 'base64'));
    } finally {
      await browser?.close();
      await web?.close();
      await api?.close();
    }
  }
);
