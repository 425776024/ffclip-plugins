// Real HTML/PAGX comparison. Run with local Chromium, FFmpeg and the pinned PAGX CLI.
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { cpus, totalmem } from 'node:os';
import { build } from 'esbuild';
import { createServer } from 'vite';
import { Chromium } from '../packages/server/chromium.mjs';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';
import {
  createProject,
  addHtmlClip,
  addPagxClip,
  addAsset,
  ticks
} from '../packages/core/project.mjs';
import { run } from '../packages/server/media.mjs';
import { pagxRuntimeAssets } from './pagx-assets.mjs';
import { textTemplateAssets } from './text-assets.mjs';

const output = resolve(process.env.PAGX_BENCH_OUTPUT || '.local/pagx/benchmark');
const trials = Number(process.env.PAGX_BENCH_TRIALS || 3);
const modes = (process.env.PAGX_BENCH_MODES || 'render,export').split(',');
if (modes.some(mode => !['render', 'export', 'endtoend'].includes(mode))) throw Error('Unknown PAGX_BENCH_MODES; use render, export or endtoend');
const existingInput = process.env.PAGX_BENCH_INPUT;
await mkdir(output, { recursive: true });
const report = {
  environment: {
    cpu: cpus()[0].model,
    ram: totalmem(),
    platform: process.platform,
    arch: process.arch,
    node: process.version
  },
  method: {
    fps: 30,
    trials,
    renderFrames: 120,
    renderIncludesGpuReadback: true,
    htmlExportPrefetch: true,
    independentCachePerTrial: true
  },
  cases: [],
  benchmarks: []
};
const persist = () => writeFile(join(output, 'report.json'), JSON.stringify(report, null, 2));
const bundle = await build({
  entryPoints: ['src/editor/html-presets.ts'],
  bundle: true,
  platform: 'node',
  format: 'esm',
  write: false,
  loader: { '.png': 'dataurl' },
  logLevel: 'silent'
});
const { HTML_PRESETS } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64')
);
const controlled = (id, name, body, script) => ({
  id,
  name,
  html: {
    width: 1920,
    height: 1080,
    duration: ticks(6),
    transparent: true,
    html: `<!doctype html><html><head><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}*{box-sizing:border-box}.shape{position:absolute;width:120px;height:90px;background:#ff6633}.title{position:absolute;left:140px;top:330px;font-family:'PingFang SC';font-size:80px;color:white;white-space:nowrap}.bar{position:absolute;left:140px;top:500px;width:900px;height:14px;background:#65f4d2;transform-origin:left center}</style></head><body>${body}<script>${script}</script></body></html>`
  }
});
const fixtures = [
  controlled(
    'control-shapes',
    '1080p 连续二维矢量',
    Array.from(
      { length: 30 },
      (_, i) =>
        `<div class="shape" style="left:${120 + (i % 6) * 275}px;top:${80 + Math.floor(i / 6) * 185}px;background:hsl(${i * 13} 70% 60%)"></div>`
    ).join(''),
    `window.tick=t=>document.querySelectorAll('.shape').forEach((e,i)=>{e.style.transform='translateX('+Math.sin(t*2+i)*65+'px) rotate('+(t*45+i*9)+'deg)';e.style.opacity=String(.65+.3*Math.sin(t+i));});`
  ),
  controlled(
    'control-title',
    '1080p 中文 GSAP 标题',
    '<div class="title">让创意动起来 · VideoCut</div><div class="bar"></div>',
    `gsap.defaults({force3D:false});window.__timelines={main:gsap.timeline({paused:true}).fromTo('.title',{x:-80,opacity:0},{x:0,opacity:1,duration:.8,ease:'power2.out'},0).fromTo('.bar',{scaleX:0},{scaleX:1,duration:1},.2).to('.title',{x:200,duration:4,ease:'none'},1).to('.title,.bar',{opacity:0,duration:.7},5.3)};`
  ),
  ...HTML_PRESETS.map(({ id, name, html }) => ({ id, name, html }))
];
if (process.env.PAGX_BENCH_CASES) {
  const ids = process.env.PAGX_BENCH_CASES.split(',');
  fixtures.splice(0, fixtures.length, ...fixtures.filter((f) => ids.includes(f.id)));
}
let api, web, browser;
try {
  api = await startServer({
    port: 0,
    roots: [output],
    visionModelDir: join(output, 'vision'),
    htmlRenderer: null
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
  report.environment.browser = await browser.send('Browser.getVersion');
  const { targetId } = await browser.send('Target.createTarget', {
    url: origin + '/tests/browser-pagx-benchmark.html'
  });
  const { sessionId } = await browser.send('Target.attachToTarget', { targetId, flatten: true });
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
  async function task(input, directory) {
    await mkdir(directory, { recursive: true });
    await evaluate(
      `window.benchmarkResult=null;window.benchmarkProgress=null;window.runBenchmark(${JSON.stringify(input)}).then(r=>window.benchmarkResult=r);true`
    );
    const deadline = Date.now() + 180000;
    let result;
    while (Date.now() < deadline) {
      result = await evaluate('window.benchmarkResult');
      if (result) break;
      await new Promise((r) => setTimeout(r, 200));
    }
    if (!result) throw Error('Browser benchmark timed out');
    if (result.error) throw Error(result.error);
    for (const f of result.files || [])
      await writeFile(join(directory, f.name), Buffer.from(f.base64, 'base64'));
    delete result.files;
    return result;
  }
  for (const fixture of fixtures) {
    const row = {
      id: fixture.id,
      name: fixture.name,
      width: fixture.html.width,
      height: fixture.html.height,
      seconds: fixture.html.duration / 120000
    };
    report.cases.push(row);
    const started = performance.now();
    try {
      if (existingInput) {
        Object.assign(
          fixture,
          JSON.parse(await readFile(join(existingInput, fixture.id, 'input.json'), 'utf8'))
        );
        row.converted = true;
        row.inputFrom = resolve(existingInput, fixture.id, 'input.json');
        continue;
      }
      const converted = await client.convertHtmlToPagx({ html: fixture.html, name: fixture.name });
      row.conversionMs = performance.now() - started;
      row.xmlBytes = Buffer.byteLength(converted.pagx.xml);
      row.animations = converted.capturedAnimations;
      row.warnings = converted.warnings;
      row.converted = true;
      fixture.pagx = converted.pagx;
      const directory = join(output, fixture.id);
      await mkdir(directory, { recursive: true });
      await writeFile(join(directory, 'source.html'), fixture.html.html);
      await writeFile(join(directory, 'converted.pagx'), converted.pagx.xml);
      await writeFile(
        join(directory, 'input.json'),
        JSON.stringify({ html: fixture.html, pagx: fixture.pagx })
      );
      row.quality = await task(
        {
          mode: 'quality',
          html: fixture.html,
          pagx: fixture.pagx,
          times: [0, 0.3, 1.2, row.seconds / 2, row.seconds - 0.15, 0.3]
        },
        directory
      );
      row.reverseStable = row.quality.frames[1].pagxHash === row.quality.frames.at(-1).pagxHash;
      console.log(
        JSON.stringify({
          id: row.id,
          conversionMs: row.conversionMs,
          quality: row.quality.frames.map((f) => ({
            t: f.seconds,
            mae: f.foregroundMae,
            changed: f.changedForegroundPercent,
            iou: f.alphaIoU
          })),
          warnings: row.warnings.length
        })
      );
    } catch (error) {
      row.error = error.message;
      row.conversionMs ??= performance.now() - started;
      console.log(JSON.stringify({ id: row.id, error: row.error }));
    }
    await persist();
  }
  const wave = join(output, 'tone.wav');
  await run('ffmpeg', [
    '-y',
    '-v',
    'error',
    '-f',
    'lavfi',
    '-i',
    'sine=frequency=440:duration=6',
    '-ar',
    '48000',
    wave
  ]);
  const audio = await client.importMedia(wave);
  const candidates = fixtures.filter(
    (f) =>
      f.pagx && ['control-shapes', 'control-title', 'lower-third', 'prism-launch'].includes(f.id)
  );
  for (const fixture of candidates)
    for (let trial = 0; trial < trials; trial++) {
      for (const kind of trial % 2 ? ['pagx', 'html'] : ['html', 'pagx'])
        for (const mode of modes) {
          const project = createProject(`${fixture.id} ${kind} ${mode} ${trial}`);
          project.canvas = { width: fixture.html.width, height: fixture.html.height };
          project.frameRate = { numerator: 30, denominator: 1 };
          const length = Math.min(ticks(6), fixture.html.duration);
          const nonce = `benchmark-${fixture.id}-${kind}-${mode}-${trial}-${Date.now()}`;
          if (kind === 'html')
            addHtmlClip(project, {
              html: { ...fixture.html, html: fixture.html.html + '<!--' + nonce + '-->' },
              length
            });
          else
            addPagxClip(project, {
              pagx: { ...fixture.pagx, xml: fixture.pagx.xml + '<!--' + nonce + '-->' },
              length
            });
          if (mode !== 'render') addAsset(project, structuredClone(audio));
          const session = await client.createSession(project);
          const urls = Object.fromEntries(
            project.assets.map((a) => {
              const u = new URL(client.mediaUrl(session.id, a.id));
              return [a.id, origin + u.pathname + u.search];
            })
          );
          const directory = join(output, fixture.id, `${kind}-${mode}-${trial}`);
          const row = { id: fixture.id, kind, mode, trial };
          report.benchmarks.push(row);
          try {
            Object.assign(
              row,
              await task(
                {
                  mode,
                  project,
                  urls,
                  frames: 120,
                  session: { id: session.id, version: session.version },
                  directory
                },
                directory
              )
            );
            if (mode !== 'render') {
              const movie = row.receipt?.path || join(directory, 'export.mp4');
              row.probe = JSON.parse(
                await run('ffprobe', [
                  '-v',
                  'error',
                  '-show_streams',
                  '-show_format',
                  '-of',
                  'json',
                  movie
                ])
              );
              await run('ffmpeg', ['-v', 'error', '-i', movie, '-f', 'null', '-']);
              row.decoded = true;
            }
            console.log(
              JSON.stringify({
                id: row.id,
                kind,
                mode,
                trial,
                cold: row.coldMs,
                p50: row.frameMs?.p50,
                p95: row.frameMs?.p95,
                ms: row.elapsedMs
              })
            );
          } catch (error) {
            row.error = error.message;
            console.log(JSON.stringify(row));
          }
          await persist();
        }
    }
} finally {
  await browser?.close();
  await web?.close();
  await api?.close();
  await persist();
}
// Vite's prebundler can keep an idle worker handle after all browsers/servers close.
process.exit(0);
