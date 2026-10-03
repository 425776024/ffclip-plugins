import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { mkdtemp, rm, readdir } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';
import { createProject, addHtmlClip, addText, ticks } from '../packages/core/project.mjs';
import { HtmlVideoCache, nativeHtmlPlan, createNativeHtmlExportJob } from '../packages/server/native-html-export.mjs';
import { run } from '../packages/server/media.mjs';

function project() {
  const p = createProject('native-test'); p.canvas = { width: 320, height: 180 };
  addHtmlClip(p, { html: { html: '<body style="background:red"></body>', width: 320, height: 180, transparent: false, duration: ticks(.6) } });
  return p;
}
test('native HTML backend only selects a full-canvas unmodified single visual', () => {
  const p = project();
  assert.ok(nativeHtmlPlan(p));
  assert.equal(nativeHtmlPlan(p, 'webm'), null);
  const i = p.timeline.tracks[0].items[0];
  i.clip.visual.opacity = .5; assert.equal(nativeHtmlPlan(p), null); i.clip.visual.opacity = 1;
  i.clip.effects.push({ id: 'blur' }); assert.equal(nativeHtmlPlan(p), null); i.clip.effects = [];
  i.clip.automation.opacity = {}; assert.equal(nativeHtmlPlan(p), null); i.clip.automation = {};
  addText(p, { content: 'overlay', length: ticks(.6) }); assert.equal(nativeHtmlPlan(p), null);
});
test('encoded HTML cache stays within its RAM budget and drops least recently used clips', () => {
  assert.throws(() => new HtmlVideoCache(0), RangeError);
  const cache = new HtmlVideoCache(10);
  cache.set('a', Buffer.alloc(4)); cache.set('b', Buffer.alloc(4)); cache.get('a');
  cache.set('c', Buffer.alloc(4)); assert.equal(cache.get('b'), undefined); assert.equal(cache.bytes, 8);
  cache.set('large', Buffer.alloc(11)); assert.equal(cache.bytes, 8);
  cache.clear(); assert.equal(cache.bytes, 0); assert.equal(cache.entries.size, 0);
});
const renderer = resolve('.local/bin/ffclip-html-renderer');
test('native raw-pixel export writes all frames and reuses encoded frames only from RAM', { skip: process.platform !== 'darwin' || !existsSync(renderer), timeout: 30000 }, async t => {
  const root = await mkdtemp(join(tmpdir(), 'ffclip-native-html-')); t.after(() => rm(root, { recursive: true, force: true }));
  const cache = new HtmlVideoCache(), p = project();
  async function render(name) {
    const job = await createNativeHtmlExportJob(p, 1, join(root, name), () => {}, renderer, 'ffmpeg', cache);
    try { return await job.completed; } finally { await job.dispose(); }
  }
  const a = await render('first'), b = await render('cached');
  assert.equal(a.pngFrames, 0); assert.equal(a.frameTransferBytes, 0);
  assert.equal(a.native.reused, a.frames - 1);
  assert.equal(a.cacheHit, false); assert.equal(b.cacheHit, true);
  const info = JSON.parse(await run('ffprobe', ['-v','error','-show_entries','stream=width,height,nb_frames,avg_frame_rate','-of','json',a.path]));
  assert.equal(info.streams[0].nb_frames, String(a.frames)); assert.equal(info.streams[0].width, 320);
  const digest = path => run('ffmpeg', ['-v','error','-i',path,'-map','0:v:0','-c','copy','-f','hash','-hash','sha256','-']);
  assert.equal(await digest(a.path), await digest(b.path), 'RAM reuse keeps the original encoded video packets');
  p.timeline.tracks[0].items[0].clip.html.html = '<body style="background:blue"></body>';
  const c = await render('changed'); assert.equal(c.cacheHit, false);
  assert.deepEqual((await readdir(root)).sort(), ['cached.mp4','changed.mp4','first.mp4'], 'No frame/cache files or intermediates are retained');
});
