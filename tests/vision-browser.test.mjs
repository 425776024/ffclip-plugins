import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'vite';
import vue from '@vitejs/plugin-vue';
import { readFile, mkdtemp, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { Chromium } from '../packages/server/chromium.mjs';
import { run } from '../packages/server/media.mjs';

// The model fixture isolates integration from weight quality, cost and download consent.
const transformersFixture = `
export const env = { backends: { onnx: { wasm: {} } } };
export class RawImage { constructor(data, width, height, channels) { Object.assign(this, {data,width,height,channels}); } }
export const AutoProcessor = { async from_pretrained(id, options) {
  if (env.allowRemoteModels !== false || !options.local_files_only || id !== 'fastvlm-0.5b') throw Error('Remote model access');
  const processor = async (image, text) => ({ input_ids: { dims: [1, 12] }, pixel_values: image.data, fixture_prompt: text });
  processor.apply_chat_template = (messages) => messages[0].content;
  processor.batch_decode = (output) => output;
  return processor;
} };
export const AutoModelForImageTextToText = { async from_pretrained(id, options) {
  if (!options.local_files_only || options.dtype.embed_tokens !== 'fp16' || options.dtype.vision_encoder !== 'q4') throw Error('Invalid local model config');
  return { async generate({pixel_values, fixture_prompt}) {
    const colors = new Set();
    for (let i = 0; i < pixel_values.length; i += 52) {
      if (pixel_values[i] > 180 && pixel_values[i + 2] < 80) colors.add('red');
      else if (pixel_values[i + 2] > 180 && pixel_values[i] < 80) colors.add('blue');
    }
    if (fixture_prompt.includes('chronological storyboard') && !fixture_prompt.includes('Panel labels are source timestamps')) throw Error('Missing temporal prompt');
    const text = fixture_prompt.includes('Return JSON for the interval.') ? JSON.stringify({ colors: [...colors] }) : [...colors].join(' then ') + ' pixels';
    return { slice() { return [text]; } };
  }, async dispose() {} };
} };
`;

test(
  'browser initialization dialog and real image/video decode with a simulated generation provider',
  { timeout: 60000 },
  async (t) => {
    const directory = await mkdtemp(join(tmpdir(), 'videocut-vision-browser-'));
    t.after(() => rm(directory, { recursive: true, force: true }));
    const videoPath = join(directory, 'scene.mp4'),
      imagePath = join(directory, 'scene.png');
    await run(process.env.FFMPEG || 'ffmpeg', [
      '-v',
      'error',
      '-f',
      'lavfi',
      '-i',
      'color=red:s=64x64:r=25:d=1',
      '-f',
      'lavfi',
      '-i',
      'color=blue:s=64x64:r=25:d=1',
      '-filter_complex',
      '[0:v][1:v]concat=n=2:v=1:a=0',
      '-c:v',
      'libx264',
      '-pix_fmt',
      'yuv420p',
      videoPath
    ]);
    await run(process.env.FFMPEG || 'ffmpeg', [
      '-v',
      'error',
      '-i',
      videoPath,
      '-frames:v',
      '1',
      imagePath
    ]);
    const video = await readFile(videoPath),
      image = await readFile(imagePath);
    const fixture = {
      name: 'vision-integration-fixture',
      configureServer(server) {
        server.middlewares.use((req, res, next) => {
          const url = new URL(req.url, 'http://127.0.0.1');
          if (url.pathname === '/asr-runtime/transformers.min.js') {
            res.setHeader('Content-Type', 'text/javascript');
            res.end(transformersFixture);
            return;
          }
          if (url.pathname !== '/api/sessions/abc/vision-media') return next();
          const bytes = url.searchParams.get('kind') === 'image' ? image : video;
          let start = 0,
            end = bytes.length - 1,
            status = 200;
          const range = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range || '');
          if (range) {
            start = Number(range[1]);
            end = Math.min(Number(range[2] || end), end);
            status = 206;
            res.setHeader('Content-Range', `bytes ${start}-${end}/${bytes.length}`);
          }
          res.writeHead(status, {
            'Content-Type': bytes === image ? 'image/png' : 'video/mp4',
            'Accept-Ranges': 'bytes',
            'Content-Length': end - start + 1
          });
          res.end(req.method === 'HEAD' ? undefined : bytes.subarray(start, end + 1));
        });
      }
    };
    const server = await createServer({
      configFile: false,
      root: process.cwd(),
      plugins: [fixture, vue()],
      server: { host: '127.0.0.1', port: 0 },
      worker: { format: 'es' }
    });
    await server.listen();
    t.after(() => server.close());
    const browser = await new Chromium().start();
    t.after(() => browser.close());
    const { targetId } = await browser.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await browser.send('Target.attachToTarget', { targetId, flatten: true });
    await browser.send(
      'Page.navigate',
      { url: `http://127.0.0.1:${server.httpServer.address().port}/tests/browser-vision.html` },
      sessionId
    );
    let report;
    for (let i = 0; i < 150; i++) {
      const value = await browser.send(
        'Runtime.evaluate',
        { expression: 'window.visionReport', returnByValue: true },
        sessionId
      );
      report = value.result.value;
      if (report?.complete) break;
      await new Promise((resolve) => setTimeout(resolve, 200));
    }
    assert.equal(report?.complete, true, JSON.stringify(report));
    assert.equal(report.error, undefined, report.error);
    assert.equal(report.rows.length, 14);
  }
);
