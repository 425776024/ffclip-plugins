import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'vite';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { Chromium } from '../packages/server/chromium.mjs';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';
import { createProject, addPagxClip, addAsset, ticks } from '../packages/core/project.mjs';
import { run } from '../packages/server/media.mjs';
import { pagxRuntimeAssets } from '../scripts/pagx-assets.mjs';
import { textTemplateAssets } from '../scripts/text-assets.mjs';
const base =
  '<style>body{margin:0}#box{position:absolute;left:20px;top:20px;width:60px;height:40px;background:red}</style><div id="box"></div>';
const content = (html) => ({
  html,
  width: 320,
  height: 180,
  duration: ticks(2),
  transparent: true
});

test(
  'PAGX converts CSS/GSAP/anime.js/tick, seeks immutable transparent frames and exports decoded audio/video',
  { timeout: 120000 },
  async () => {
    const evidence = resolve('.local/pagx/evidence');
    await mkdir(evidence, { recursive: true });
    let api, web, browser;
    try {
      api = await startServer({
        port: 0,
        roots: [evidence],
        visionModelDir: join(evidence, 'vision')
      });
      const client = new VideoCutClient(api.url);
      await client.connect();
      const anime = (await readFile('node_modules/animejs/lib/anime.min.js', 'utf8')).replace(
        /\/\/[#@]\s*sourceMappingURL=.*$/gm,
        ''
      );
      const inputs = [
        base +
          '<style>#box{animation:slide 2s linear both}@keyframes slide{to{transform:translateX(120px);opacity:0.5}}</style>',
        base +
          "<script>window.__timelines={main:gsap.timeline({paused:true}).to('#box',{x:120,opacity:0.5,duration:2,ease:'none'})}</script>",
        base +
          "<script>window.tick=t=>{box.style.transform='translateX('+60*t+'px)';box.style.opacity=1-t/4}</script>",
        base +
          '<script>' +
          anime +
          "</script><script>anime({targets:'#box',translateX:120,opacity:[1,0.5],duration:2000,easing:'linear'})</script>"
      ];
      const converted = [];
      for (const input of inputs) {
        const r = await client.convertHtmlToPagx({ html: content(input) });
        assert.equal(r.capturedAnimations, 1);
        assert.deepEqual(r.warnings, []);
        converted.push(r);
      }
      const titleHtml = content(
        '<style>body{margin:0}#title{position:absolute;left:20px;top:20px;font:32px Arial;color:white;white-space:nowrap}</style><div id="title">Title</div><script>window.__timelines={main:gsap.timeline({paused:true}).to("#title",{x:120,opacity:.5,duration:2,ease:"none"})}</script>'
      );
      const titlePagx = (await client.convertHtmlToPagx({ html: titleHtml })).pagx;
      const picture = join(evidence, 'image.png');
      await run('ffmpeg', [
        '-y',
        '-v',
        'error',
        '-f',
        'lavfi',
        '-i',
        'color=c=lime:s=24x24',
        '-frames:v',
        '1',
        picture
      ]);
      const graphic = join(evidence, 'text-image.pagx');
      await writeFile(
        graphic,
        '<pagx width="320" height="180"><Layer x="10" y="10"><Rectangle size="24,24"/><Fill><ImagePattern image="image.png"/></Fill></Layer><Layer x="60" y="20"><Text text="中文标题" fontSize="28"/><Fill color="#ffffff"/></Layer></pagx>'
      );
      const imported = await client.importPagx(graphic, { duration: ticks(2), transparent: true });
      assert.match(imported.pagx.xml, /data:image\/png;base64,/);
      assert.doesNotMatch(imported.pagx.xml, /image\.png/);
      await assert.rejects(
        client.convertHtmlToPagx({
          html: content(base + '<script>window.tick=t=>box.style.width=(60+t*20)+"px"</script>')
        }),
        /布局/
      );
      await assert.rejects(
        client.convertHtmlToPagx({
          html: content(base + '<script>window.tick=t=>box.textContent=String(t)</script>')
        }),
        /布局/
      );
      await assert.rejects(
        client.convertHtmlToPagx({ html: content('<canvas></canvas>') }),
        /Canvas/
      );
      const p = createProject('PAGX open-source export evidence');
      p.canvas = { width: 320, height: 180 };
      p.frameRate = { numerator: 30, denominator: 1 };
      addPagxClip(p, { pagx: converted[0].pagx });
      const wave = join(evidence, 'tone.wav');
      await run('ffmpeg', [
        '-y',
        '-v',
        'error',
        '-f',
        'lavfi',
        '-i',
        'sine=frequency=440:duration=2',
        '-ar',
        '48000',
        wave
      ]);
      addAsset(p, await client.importMedia(wave));
      const session = await client.createSession(p);
      const saved = await client.saveProject(session.id, session.version, evidence);
      const reopened = await client.openProject(saved.path);
      assert.deepEqual(
        reopened.project.timeline.tracks.flatMap((t) => t.items).find((i) => i.clip.pagx).clip.pagx,
        converted[0].pagx
      );
      web = await createServer({
        configFile: false,
        plugins: [pagxRuntimeAssets(), textTemplateAssets()],
        worker: { format: 'es' },
        server: {
          host: '127.0.0.1',
          port: 0,
          watch: { ignored: ['**/.local/**'] },
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
        url: origin + '/tests/browser-pagx.html'
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
      for (let i = 0; i < 100 && !(await evaluate('typeof window.runPagx==="function"')); i++)
        await new Promise((r) => setTimeout(r, 50));
      const urls = Object.fromEntries(
        p.assets.map((a) => {
          const u = new URL(client.mediaUrl(session.id, a.id));
          return [a.id, origin + u.pathname + u.search];
        })
      );
      await evaluate(
        `window.pagxResult=null;window.runPagx(${JSON.stringify({ project: p, urls, clips: converted.map((r) => r.pagx), graphic: imported.pagx, titleHtml, titlePagx })}).then(r=>window.pagxResult=r);true`
      );
      let result;
      for (let i = 0; i < 1000; i++) {
        result = await evaluate('window.pagxResult');
        if (result) break;
        await new Promise((r) => setTimeout(r, 50));
      }
      assert.ok(result, 'worker completed');
      assert.ifError(result.error);
      for (const f of result.frames) {
        assert.notEqual(f.before, f.middleHash);
        assert.equal(f.before, f.oldAfterSeek);
        assert.equal(f.before, f.rewound);
        assert.equal(f.minX, 50);
        assert.equal(f.maxX, 109);
        assert.equal(f.nonzero, 2400);
        assert.ok(Math.abs(f.pixel[3] - 223) <= 2);
      }
      assert.equal(new Set(result.frames.map((f) => f.middleHash)).size, 1);
      assert.equal(result.sceneHashes[1], result.sceneHashes[3]);
      assert.notEqual(result.sceneHashes[0], result.sceneHashes[1]);
      assert.ok(result.graphic.green > 400, 'embedded local PNG renders in worker');
      assert.ok(result.graphic.text > 200, 'Chinese text has visible glyph pixels');
      assert.equal(result.graphic.initial, result.graphic.end, 'static PAGX has stable frames');
      assert.ok(
        Math.abs(result.title.html.dx - result.title.pagx.dx) <= 1,
        'text translation must not be applied twice'
      );
      assert.ok(
        Math.abs(result.title.html.alpha - result.title.pagx.alpha) <= 3,
        'text opacity must not be applied twice'
      );
      const output = join(evidence, 'pagx-export.mp4');
      await writeFile(output, Buffer.from(result.base64, 'base64'));
      delete result.base64;
      const probe = JSON.parse(
        await run('ffprobe', [
          '-v',
          'error',
          '-show_streams',
          '-show_format',
          '-of',
          'json',
          output
        ])
      );
      assert.equal(probe.streams.find((s) => s.codec_type === 'video').width, 320);
      assert.ok(probe.streams.some((s) => s.codec_type === 'audio'));
      assert.ok(Math.abs(Number(probe.format.duration) - 2) < 0.1);
      await run('ffmpeg', ['-v', 'error', '-i', output, '-f', 'null', '-']);
      const decoded = await run(
        'ffmpeg',
        [
          '-v',
          'error',
          '-ss',
          '0.5',
          '-i',
          output,
          '-frames:v',
          '1',
          '-f',
          'rawvideo',
          '-pix_fmt',
          'rgb24',
          'pipe:1'
        ],
        { binary: true }
      );
      const at = (x, y) => [...decoded.subarray((y * 320 + x) * 3, (y * 320 + x) * 3 + 3)];
      assert.ok(
        at(60, 30)[0] > 180 && at(60, 30)[1] < 50,
        'encoded red shape is at the half-second position'
      );
      assert.ok(at(30, 30)[0] < 40, 'encoded earlier position is empty');
      await writeFile(
        join(evidence, 'report.json'),
        JSON.stringify(
          {
            result,
            probe,
            conversion: converted.map((r) => ({
              animations: r.capturedAnimations,
              warnings: r.warnings,
              conversion: r.conversion
            }))
          },
          null,
          2
        )
      );
      console.log(
        JSON.stringify({
          frames: result.frames,
          encoding: result.encoding,
          pagx60FramesMs: result.pagxMs,
          output
        })
      );
    } finally {
      await browser?.close();
      await web?.close();
      await api?.close();
    }
  }
);
