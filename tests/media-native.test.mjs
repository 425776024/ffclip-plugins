import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdir, realpath, readFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { startServer } from '../packages/server/index.mjs';
import { run } from '../packages/server/media.mjs';
import {
  VideoCutClient,
  addAsset,
  addText,
  createProject,
  ticks,
  splitItem,
  moveItem,
  trimItem
} from '../packages/client/index.mjs';

test('real media import, browser-required export contract, native publication and native reopen', async (t) => {
  try {
    await run('ffmpeg', ['-version']);
  } catch {
    t.skip('FFmpeg fixture generator is not installed');
    return;
  }
  await mkdir('.local/qa', { recursive: true });
  const root = await realpath('.local/qa');
  const video = join(root, '测试 sample.mp4'),
    audio = join(root, 'tone.wav'),
    image = join(root, 'still.png');
  await run('ffmpeg', [
    '-hide_banner',
    '-loglevel',
    'error',
    '-y',
    '-f',
    'lavfi',
    '-i',
    'testsrc2=size=320x180:rate=30',
    '-f',
    'lavfi',
    '-i',
    'sine=frequency=440:sample_rate=48000',
    '-t',
    '3',
    '-c:v',
    'libx264',
    '-pix_fmt',
    'yuv420p',
    '-c:a',
    'aac',
    video
  ]);
  await run('ffmpeg', [
    '-hide_banner',
    '-loglevel',
    'error',
    '-y',
    '-f',
    'lavfi',
    '-i',
    'sine=frequency=660:sample_rate=48000',
    '-t',
    '2',
    audio
  ]);
  await run('ffmpeg', [
    '-hide_banner',
    '-loglevel',
    'error',
    '-y',
    '-f',
    'lavfi',
    '-i',
    'color=red:size=100x100',
    '-frames:v',
    '1',
    image
  ]);
  const bridge = resolve('.local/bin/videocut-bridge');
  const server = await startServer({
    port: 0,
    roots: [root],
    nativeBridge: bridge,
    ffprobe: '/definitely/unavailable/ffprobe'
  });
  t.after(() => server.close());
  const client = new VideoCutClient(server.url);
  await client.connect();
  const session = await client.createSession(),
    p = session.project;
  p.canvas = { width: 320, height: 180 };
  p.name = 'refactor-verification';
  const [v, a, img] = await Promise.all([
    client.importMedia(video),
    client.importMedia(audio),
    client.importMedia(image)
  ]);
  assert.equal(v.hasAudio, true);
  assert.equal(v.width, 320);
  assert.equal(a.kind, 'audio');
  assert.equal(img.kind, 'image');
  const first = addAsset(p, v);
  const right = splitItem(p, first.id, ticks(1));
  moveItem(p, right.id, ticks(1.5));
  trimItem(p, right.id, ticks(1.5), ticks(3));
  right.clip.visual.positionX = 15;
  right.clip.visual.scaleX = 0.8;
  right.clip.visual.scaleY = 0.8;
  right.clip.visual.opacity = 0.7;
  right.clip.audio.gainLinear = 0.25;
  addAsset(p, a).clip.audio.gainLinear = 0.2;
  const still = addAsset(p, img);
  trimItem(p, still.id, 0, ticks(1));
  still.clip.visual.scaleX = 0.2;
  still.clip.visual.scaleY = 0.2;
  const title = addText(p, {
    content: "你好 VideoCut 100% : '测试'",
    fontSize: 24,
    color: '#34d1bf',
    length: ticks(2)
  });
  title.clip.visual.positionY = 30;
  splitItem(p, title.id, ticks(1));
  const updated = await client.updateSession(session.id, p, 0);
  await assert.rejects(
    client.renderVideo(session.id, updated.version, root),
    (e) => e.snapshot?.code === 'BROWSER_REQUIRED'
  );
  if (!existsSync(bridge)) {
    t.diagnostic('Native roundtrip not run: build:native is required');
    return;
  }
  const result = await client.exportProject(session.id, updated.version, root);
  assert.equal((await client.getSession(session.id)).projectPath, result.path);
  assert.equal((await client.listSessions())[0].projectPath, result.path);
  assert.ok(result.verified);
  assert.equal(result.name, p.name);
  assert.equal(result.assets.length, 4);
  assert.equal(result.tracks.length, 5); // embedded audio is a native linked audio track
  const readback = JSON.parse(await run(bridge, ['inspect', result.path]));
  assert.equal(readback.projectId, p.id);
  assert.equal(readback.texts.length, 2);
  assert.equal(readback.texts[0].content, title.clip.text.content);
  assert.equal(readback.texts[0].fontSize, 24);
  const nativeRight = readback.tracks.flatMap((t) => t.items).find((i) => i.id === right.id);
  assert.equal(nativeRight.begin, ticks(1.5));
  assert.equal(nativeRight.sourceBegin, ticks(1));
  assert.equal(nativeRight.visual.opacity, 0.7);
  const audioRight = readback.tracks
    .flatMap((t) => t.items)
    .find((i) => i.id === `audio-${right.id}`);
  assert.equal(audioRight.audio.gain, 0.25);
  assert.equal(existsSync(join(result.path, 'web-session.json')), false);
  const reopened = await client.openProject(result.path);
  assert.equal(reopened.projectPath, result.path);
  const fromDirectory = await startServer({
    port: 0,
    roots: [root],
    nativeBridge: bridge,
    initialProjectPath: result.path
  });
  t.after(() => fromDirectory.close());
  assert.equal(fromDirectory.initialSession.projectPath, result.path);
  assert.equal(reopened.project.version, 1);
  assert.equal(reopened.project.id, p.id);
  assert.equal(
    reopened.project.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === right.id).clip
      .visual.opacity,
    0.7
  );
  t.diagnostic(`Native verified package: ${result.path}`);
});
