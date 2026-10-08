import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { createServer } from 'vite';
import { Chromium } from '../packages/server/chromium.mjs';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';
import { pagxRuntimeAssets } from '../scripts/pagx-assets.mjs';
import { textTemplateAssets } from '../scripts/text-assets.mjs';
import { PAGX_TEMPLATES, createPagxTemplate } from '../packages/pagx/templates.mjs';
import { createProject, addPagxClip, addAsset, ticks } from '../packages/core/project.mjs';
import { run } from '../packages/server/media.mjs';

test(
  'all native PAGX templates render, reverse-seek, and export through the real WASM worker',
  { timeout: 180000 },
  async () => {
    const directory = resolve(process.env.PAGX_EVIDENCE_DIR || '.local/pagx/native-evidence');
    await mkdir(directory, { recursive: true });
    const sources = resolve(process.env.PAGX_SOURCE_DIR || '.local/pagx/native-templates');
    await mkdir(sources, { recursive: true });
    let api, web, browser;
    try {
      api = await startServer({
        port: 0,
        roots: [directory],
        visionModelDir: join(directory, 'vision')
      });
      const client = new VideoCutClient(api.url);
      await client.connect();
      web = await createServer({
        configFile: false,
        plugins: [pagxRuntimeAssets(), textTemplateAssets()],
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
      const origin = 'http://127.0.0.1:' + web.httpServer.address().port;
      browser = await new Chromium().start();
      const { targetId } = await browser.send('Target.createTarget', {
        url: origin + '/tests/browser-pagx-benchmark.html'
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
      for (let i = 0; i < 200 && !(await evaluate('typeof window.runBenchmark==="function"')); i++)
        await new Promise((r) => setTimeout(r, 50));
      async function task(input) {
        await evaluate(
          `window.nativeResult=null;window.runBenchmark(${JSON.stringify(input)}).then(r=>window.nativeResult=r);true`
        );
        let result;
        for (let attempt = 0; attempt < 600; attempt++) {
          result = await evaluate('window.nativeResult');
          if (result) break;
          await new Promise((r) => setTimeout(r, 200));
        }
        assert.ok(result, 'Native render timed out');
        assert.ok(!result.error, result.error);
        for (const file of result.files || [])
          await writeFile(join(directory, file.name), Buffer.from(file.base64, 'base64'));
        delete result.files;
        return result;
      }
      const templates = PAGX_TEMPLATES.filter(
        (t) =>
          !process.env.PAGX_TEMPLATE_IDS || process.env.PAGX_TEMPLATE_IDS.split(',').includes(t.id)
      );
      const entries = [];
      for (const t of templates)
        for (const locale of ['zh', 'en'])
          entries.push({
            ...createPagxTemplate(t.id, { locale }),
            id: t.id + '-' + locale,
            probes:
              t.id === 'lower-third' ? [[200, 735]] : t.id === 'tick-title' ? [[300, 530]] : []
          });
      entries.push({
        id: 'font-style-probe',
        regions: [
          [0, 0, 300, 180],
          [320, 0, 300, 180]
        ],
        pagx: {
          width: 640,
          height: 180,
          duration: 960000,
          transparent: true,
          xml: '<pagx width="640" height="180"><Layer id="type"><TextBox width="300" height="180"><Text text="字重" fontFamily="system" fontStyle="Regular" fontSize="100"/><Fill color="#ffffff"/></TextBox><TextBox left="320" width="300" height="180"><Text text="字重" fontFamily="system" fontStyle="Semibold" fontSize="100"/><Fill color="#ffffff"/></TextBox></Layer><Animations><Animation duration="240" frameRate="30" loop="once"><Object target="type"><Channel name="alpha" type="float"><Key time="0" value="0"/><Key time="12" value="1"/><Key time="240" value="1"/></Channel></Object></Animation></Animations></pagx>'
        }
      });
      entries.push({
        id: 'matrix-probe',
        probes: [
          [180, 60],
          [50, 60]
        ],
        pagx: {
          width: 320,
          height: 180,
          duration: 960000,
          transparent: true,
          xml: '<pagx width="320" height="180"><Layer id="box" left="20" top="20" width="60" height="60"><Rectangle width="60" height="60"/><Fill color="#ff0000"/></Layer><Animations><Animation duration="240" frameRate="30" loop="once"><Object target="box"><Channel name="matrix" type="matrix"><Key time="0" value="1,0,0,1,20,20"/><Key time="120" value="1,0,0,1,140,20"/><Key time="240" value="1,0,0,1,20,20"/></Channel></Object></Animation></Animations></pagx>'
        }
      });
      for (const entry of entries)
        await writeFile(join(sources, entry.id + '.pagx'), entry.pagx.xml);
      const quality = await task({ mode: 'native-quality', entries });
      for (const row of quality.reports) {
        if (row.id.startsWith('lower-third'))
          assert.ok(row.frames[3].probes[0][3] > 200, row.id + ' retained plate');
        if (row.id.startsWith('tick-title'))
          assert.ok(row.frames[3].probes[0][3] > 80, row.id + ' retained panel');
        assert.ok(row.frames[3].nonzero > 2000, row.id + ' must contain visible artwork');
        assert.notEqual(row.frames[0].hash, row.frames[3].hash, row.id + ' entrance must animate');
        assert.ok(
          row.frames.at(-1).rewind.changedForegroundPercent < 0.5,
          row.id + ' reverse seek must reproduce the frame'
        );
      }
      const fontStyles = quality.reports.find((r) => r.id === 'font-style-probe').frames[3]
        .coverage;
      const fontCatalog = await (await fetch(origin + '/api/fonts')).json();
      const cjkFamily = fontCatalog.fonts.find((f) => f.id === fontCatalog.defaults.cjk).family;
      const family = fontCatalog.fonts.filter((f) => f.family === cjkFamily);
      if (
        family.some((f) => f.weight === 400) &&
        family.some((f) => f.weight >= 500 && f.weight <= 700)
      )
        assert.ok(
          fontStyles[1] > fontStyles[0] * 1.1,
          'Semibold must use a heavier installed face than Regular'
        );
      const matrix = quality.reports.find((r) => r.id === 'matrix-probe');
      assert.deepEqual(matrix.frames[0].probes[1], [255, 0, 0, 255]);
      assert.deepEqual(matrix.frames[3].probes[0], [255, 0, 0, 255]);
      assert.equal(matrix.frames[3].probes[1][3], 0);
      if (process.env.PAGX_WRITE_POSTERS === '1') {
        const { copyFile } = await import('node:fs/promises');
        for (const template of templates) {
          await copyFile(
            join(directory, `poster-${template.id}-zh.png`),
            resolve(`src/editor/assets/motion/pagx-poster-${template.id}.png`)
          );
          await copyFile(
            join(directory, `poster-${template.id}-en.png`),
            resolve(`src/editor/assets/motion/pagx-poster-${template.id}-en.png`)
          );
        }
      }
      if (process.env.PAGX_RENDER_ONLY === '1') {
        await writeFile(join(directory, 'report.json'), JSON.stringify({ quality }, null, 2));
        return;
      }
      const wave = join(directory, 'tone.wav');
      await run('ffmpeg', [
        '-y',
        '-v',
        'error',
        '-f',
        'lavfi',
        '-i',
        'sine=frequency=440:duration=8',
        wave
      ]);
      const audio = await client.importMedia(wave);
      const exports = [];
      for (const { id } of templates)
        for (const locale of process.env.PAGX_EXPORT_BILINGUAL === '1' ? ['zh', 'en'] : ['zh']) {
          const project = createProject(id);
          project.canvas = { width: 1920, height: 1080 };
          addPagxClip(project, { pagx: createPagxTemplate(id, { locale }).pagx, length: ticks(8) });
          addAsset(project, structuredClone(audio));
          const session = await client.createSession(project);
          const urls = Object.fromEntries(
            project.assets.map((a) => {
              const u = new URL(client.mediaUrl(session.id, a.id));
              return [a.id, origin + u.pathname + u.search];
            })
          );
          const exported = await task({ mode: 'export', project, urls });
          const movie = join(directory, 'export.mp4');
          const probe = JSON.parse(
            await run('ffprobe', ['-v', 'error', '-show_streams', '-of', 'json', movie])
          );
          assert.equal(probe.streams.find((s) => s.codec_type === 'video').nb_frames, '240');
          assert.ok(probe.streams.some((s) => s.codec_type === 'audio'));
          await run('ffmpeg', ['-v', 'error', '-i', movie, '-f', 'null', '-']);
          const { copyFile } = await import('node:fs/promises');
          await copyFile(movie, join(directory, id + (locale === 'en' ? '-en' : '') + '.mp4'));
          exports.push({ id, locale, ...exported, probe, decoded: true });
        }
      if (!process.env.PAGX_TEMPLATE_IDS)
        for (const locale of ['zh', 'en']) {
          const session = await client.initializeDemo({ locale });
          const project = session.project;
          const urls = Object.fromEntries(
            project.assets.map((a) => {
              const u = new URL(client.mediaUrl(session.id, a.id));
              return [a.id, origin + u.pathname + u.search];
            })
          );
          const id = 'starter-' + locale;
          await writeFile(
            join(sources, id + '.pagx'),
            project.timeline.tracks[2].items[0].clip.pagx.xml
          );
          await task({ mode: 'scene-quality', id, project, urls, times: [0.6, 4, 7, 10, 13, 16] });
          const exported = await task({ mode: 'export', project, urls });
          const movie = join(directory, 'export.mp4');
          const probe = JSON.parse(
            await run('ffprobe', ['-v', 'error', '-show_streams', '-of', 'json', movie])
          );
          assert.equal(probe.streams.find((s) => s.codec_type === 'video').nb_frames, '540');
          assert.ok(probe.streams.some((s) => s.codec_type === 'audio'));
          await run('ffmpeg', ['-v', 'error', '-i', movie, '-f', 'null', '-']);
          const { copyFile } = await import('node:fs/promises');
          await copyFile(movie, join(directory, id + (locale === 'en' ? '-en' : '') + '.mp4'));
          exports.push({ id, locale, ...exported, probe, decoded: true });
        }
      await writeFile(
        join(directory, 'report.json'),
        JSON.stringify({ quality, exports }, null, 2)
      );
    } finally {
      await browser?.close();
      await web?.close();
      await api?.close();
    }
  }
);
