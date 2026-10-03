import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readFile, stat, readdir, realpath, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { Readable } from 'node:stream';
import { startServer } from '../packages/server/index.mjs';
import { run } from '../packages/server/media.mjs';
import { createBrowserExportJob } from '../packages/server/template-export.mjs';
import { VideoCutClient, createProject, addText, ticks } from '../packages/client/index.mjs';
async function fixture(t) {
  const dir = await realpath(await mkdtemp(join(tmpdir(), 'videocut-export-')));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const p = createProject('export');
  p.canvas = { width: 160, height: 90 };
  p.frameRate = { numerator: 10, denominator: 1 };
  addText(p, { length: ticks(0.3) });
  return { p, dir };
}
const stream = (bytes) => Readable.from([bytes]);
async function nextEvent(reader, name) {
  let buffer = '';
  for (;;) {
    const { done, value } = await reader.read();
    if (done) throw new Error('SSE closed');
    buffer += Buffer.from(value).toString();
    let end;
    while ((end = buffer.indexOf('\n\n')) >= 0) {
      const part = buffer.slice(0, end);
      buffer = buffer.slice(end + 2);
      if (part.startsWith(`event: ${name}\n`)) return JSON.parse(part.split('\ndata: ')[1]);
    }
  }
}
test('browser chunk writer applies random offsets with auth, order, frozen version, and cleanup', async (t) => {
  const { p, dir } = await fixture(t);
  const source = join(dir, 'source.mp4');
  await run('ffmpeg', [
    '-v',
    'error',
    '-f',
    'lavfi',
    '-i',
    'color=red:size=160x90:rate=10',
    '-t',
    '0.3',
    '-c:v',
    'libx264',
    source
  ]);
  const bytes = await readFile(source);
  const job = await createBrowserExportJob(p, 7, join(dir, 'output'), 'mp4', () => {});
  t.after(() => job.dispose());
  const worker = job.claim();
  await job.configure(worker, { encoding: 'browser', hasAudio: false });
  p.name = 'changed';
  assert.equal(job.spec.project.name, 'export');
  assert.equal(job.claim(), null);
  await assert.rejects(job.chunk(stream(bytes), 'wrong', 0, 0), /渲染客户端/);
  await assert.rejects(job.chunk(stream(bytes), worker, 0, 1), /顺序/);
  await job.chunk(stream(bytes), worker, 0, 0);
  await job.chunk(stream(bytes.subarray(0, 40)), worker, 0, 1);
  await assert.rejects(job.finish(worker, bytes.length + 1), /不完整/);
  const receipt = await job.finish(worker, bytes.length);
  assert.equal(receipt.version, 7);
  assert.deepEqual(await readFile(receipt.path), bytes);
  await run('ffmpeg', ['-v', 'error', '-i', receipt.path, '-f', 'null', '-']);
  await job.dispose();
  assert.ok(!(await readdir(dir)).some((f) => f.includes('.partial-')));
});
test('raw common-compositor frames and bounded PCM encode via the original local FFmpeg', async (t) => {
  const { p, dir } = await fixture(t),
    job = await createBrowserExportJob(p, 0, join(dir, 'raw'), 'mp4', () => {});
  t.after(() => job.dispose());
  const worker = job.claim();
  await job.configure(worker, { encoding: 'frames-with-pcm', hasAudio: true });
  const pixels = Buffer.alloc(160 * 90 * 4);
  for (let i = 0; i < pixels.length; i += 4) {
    pixels[i] = 255;
    pixels[i + 3] = 255;
  }
  await assert.rejects(job.frame(stream(pixels), worker, 1), /顺序/);
  for (let i = 0; i < 3; i++) await job.frame(stream(pixels), worker, i);
  const pcm = Buffer.alloc(14400 * 8);
  for (let i = 0; i < 14400; i++) {
    pcm.writeFloatLE(0.2 * Math.sin((i * Math.PI * 880) / 48000), i * 8);
    pcm.writeFloatLE(0.2 * Math.sin((i * Math.PI * 880) / 48000), i * 8 + 4);
  }
  await job.audio(stream(pcm), worker, 0, 48000, 2, 0);
  const receipt = await job.finish(worker, { frames: 3 });
  assert.equal(receipt.encoding, 'frames-with-pcm');
  const info = JSON.parse(
    await run('ffprobe', [
      '-v',
      'error',
      '-show_streams',
      '-show_format',
      '-of',
      'json',
      receipt.path
    ])
  );
  assert.equal(info.streams[0].width, 160);
  assert.ok(info.streams.some((s) => s.codec_name === 'aac'));
  assert.ok(Math.abs(Number(info.format.duration) - 0.3) < 0.05);
  const frame = await run(
    'ffmpeg',
    [
      '-v',
      'error',
      '-i',
      receipt.path,
      '-frames:v',
      '1',
      '-f',
      'rawvideo',
      '-pix_fmt',
      'rgb24',
      '-'
    ],
    { binary: true }
  );
  assert.ok(frame[0] > 230 && frame[1] < 20 && frame[2] < 20);
});
test('cancellation removes partial writes and disallows stale clients', async (t) => {
  const { p, dir } = await fixture(t),
    job = await createBrowserExportJob(p, 0, join(dir, 'cancel'), 'webm', () => {});
  const worker = job.claim();
  await job.configure(worker, { encoding: 'browser', hasAudio: false });
  await job.chunk(stream(Buffer.alloc(100)), worker, 0, 0);
  job.fail(new Error('用户取消导出'));
  await assert.rejects(job.completed, /取消/);
  await job.dispose();
  await assert.rejects(job.chunk(stream(Buffer.alloc(10)), worker, 0, 1), /结束/);
  assert.deepEqual(await readdir(dir), []);
});
test('HTTP export requires a browser, claims one worker and freezes the authoring version', async (t) => {
  const { p, dir } = await fixture(t),
    server = await startServer({ roots: [dir], port: 0 });
  t.after(() => server.close());
  const c = new VideoCutClient(server.url),
    s = await c.createSession(p);
  await assert.rejects(
    c.renderVideo(s.id, 0, dir),
    (e) => e.status === 409 && e.snapshot.code === 'BROWSER_REQUIRED'
  );
  const abort = new AbortController(),
    events = await fetch(c.eventsUrl(s.id), { signal: abort.signal }),
    reader = events.body.getReader();
  t.after(() => {
    abort.abort();
  });
  const event = nextEvent(reader, 'render-request');
  const exporting = c.renderVideo(s.id, 0, dir);
  exporting.catch(() => {});
  const { id } = await event;
  const base = `/sessions/${s.id}/render-job?job=${id}`,
    call = (action, options = {}) =>
      c.request(base + '&action=' + action, { method: 'POST', ...options });
  const spec = await call('claim');
  assert.equal(spec.version, 0);
  await assert.rejects(call('claim'), (e) => e.status === 409);
  const updated = await c.editSession(s.id, [{ action: 'configure_project', name: 'edited' }], 0);
  assert.equal(updated.version, 1);
  assert.equal(spec.project.name, 'export');
  await call('cancel');
  await assert.rejects(exporting, /取消/);
  assert.equal((await c.renderStatus(s.id)).phase, 'error');
});

test('concurrent render requests serialize and frozen media keeps the original path after edits', async (t) => {
  const { p, dir } = await fixture(t);
  const firstPath = join(dir, 'first.mp4'),
    secondPath = join(dir, 'second.mp4');
  await writeFile(firstPath, 'first');
  await writeFile(secondPath, 'second');
  p.assets.push({
    id: 'frozen-asset',
    name: 'file',
    kind: 'video',
    path: firstPath,
    size: 5,
    duration: ticks(1),
    width: 160,
    height: 90,
    hasAudio: false
  });
  const server = await startServer({ roots: [dir], port: 0 });
  t.after(() => server.close());
  const c = new VideoCutClient(server.url),
    s = await c.createSession(p);
  const abort = new AbortController(),
    events = await fetch(c.eventsUrl(s.id), { signal: abort.signal }),
    reader = events.body.getReader();
  t.after(() => abort.abort());
  const event = nextEvent(reader, 'render-request');
  const exports = [c.renderVideo(s.id, 0, dir), c.renderVideo(s.id, 0, dir)];
  exports.forEach((p) => p.catch(() => {}));
  const job = await event;
  const base = `/sessions/${s.id}/render-job?job=${job.id}`;
  await c.request(base + '&action=claim', { method: 'POST' });
  p.assets[0].path = secondPath;
  p.assets[0].size = 6;
  await c.updateSession(s.id, p, 0);
  assert.equal(
    await (await fetch(c.mediaUrl(s.id, 'frozen-asset') + '&job=' + job.id)).text(),
    'first'
  );
  assert.equal(await (await fetch(c.mediaUrl(s.id, 'frozen-asset'))).text(), 'second');
  await c.request(base + '&action=cancel', { method: 'POST' });
  const results = await Promise.allSettled(exports);
  assert.equal(results.filter((r) => r.status === 'rejected' && r.reason.status === 409).length, 1);
  assert.equal(
    results.filter((r) => r.status === 'rejected' && /取消/.test(r.reason.message)).length,
    1
  );
  assert.ok(!(await readdir(dir)).some((f) => f.includes('.partial-')));
});

test('missing FFmpeg leaves browser output available and refuses both native fallbacks before locking configuration', async (t) => {
  const { p, dir } = await fixture(t),
    missing = join(dir, 'ffmpeg-does-not-exist');
  const job = await createBrowserExportJob(
    p,
    3,
    join(dir, 'browser-only'),
    'webm',
    () => {},
    missing
  );
  t.after(() => job.dispose());
  assert.equal(job.spec.localEncoder, false);
  const worker = job.claim(),
    bytes = Buffer.alloc(64, 0x42);
  await assert.rejects(job.chunk(stream(bytes), worker, 0, 0), /顺序/);
  for (const encoding of ['video-with-pcm', 'frames-with-pcm'])
    await assert.rejects(job.configure(worker, { encoding, hasAudio: true }), /FFmpeg.*WebM/);
  await job.configure(worker, { encoding: 'browser', hasAudio: true });
  await job.chunk(stream(bytes), worker, 0, 0);
  await job.chunk(stream(Buffer.from('WEBM')), worker, 0, 1);
  const receipt = await job.finish(worker, { size: 64, frames: 3, audioFrames: 14400 });
  assert.equal(receipt.encoding, 'browser');
  const expected = Buffer.from(bytes);
  expected.write('WEBM');
  assert.deepEqual(await readFile(receipt.path), expected);
  await job.dispose();
  assert.deepEqual(await readdir(dir), ['browser-only.webm']);
});

test('HTTP browser export succeeds with nonexistent FFmpeg and FFprobe paths', async (t) => {
  const { p, dir } = await fixture(t),
    server = await startServer({
      roots: [dir],
      port: 0,
      ffmpeg: join(dir, 'missing-ffmpeg'),
      ffprobe: join(dir, 'missing-ffprobe')
    });
  t.after(() => server.close());
  const client = new VideoCutClient(server.url),
    session = await client.createSession(p);
  const abort = new AbortController(),
    events = await fetch(client.eventsUrl(session.id), { signal: abort.signal }),
    reader = events.body.getReader();
  t.after(() => abort.abort());
  const event = nextEvent(reader, 'render-request'),
    pending = client.renderVideo(session.id, session.version, dir, 'webm');
  pending.catch(() => {});
  const { id } = await event,
    base = `/sessions/${session.id}/render-job?job=${id}`;
  const spec = await client.request(base + '&action=claim', { method: 'POST' });
  assert.equal(spec.localEncoder, false);
  const headers = { 'X-Render-Worker': spec.worker };
  await assert.rejects(
    client.request(base + '&action=configure', {
      method: 'POST',
      headers,
      body: JSON.stringify({ encoding: 'frames-with-pcm', hasAudio: false })
    }),
    /FFmpeg.*WebM/
  );
  await client.request(base + '&action=configure', {
    method: 'POST',
    headers,
    body: JSON.stringify({ encoding: 'browser', hasAudio: false })
  });
  const bytes = Buffer.alloc(64, 0x57);
  await client.request(base + '&action=chunk&position=0&sequence=0', {
    method: 'POST',
    headers: { ...headers, 'Content-Type': 'application/octet-stream' },
    body: bytes
  });
  await client.request(base + '&action=finish', {
    method: 'POST',
    headers,
    body: JSON.stringify({ size: bytes.length, frames: 3, audioFrames: 0 })
  });
  const receipt = await pending;
  assert.equal(receipt.encoding, 'browser');
  assert.deepEqual(await readFile(receipt.path), bytes);
  assert.ok(!(await readdir(dir)).some((name) => name.includes('.partial-')));
});
