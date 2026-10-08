import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readFile, readdir } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';
import { duration, ticks } from '../packages/core/project.mjs';
import { browserCommand } from '../bin/open-preview.mjs';

async function setup(t, options = {}) {
  const root = await mkdtemp(join(tmpdir(), 'videocut-starter-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const server = await startServer({
    roots: [root],
    port: 0,
    ffprobe: 'unavailable-ffprobe',
    ttsModelDir: join(root, 'tts'),
    asrModelDir: join(root, 'asr'),
    visionModelDir: join(root, 'vision'),
    ...options
  });
  t.after(() => server.close());
  return { root, server, client: new VideoCutClient(server.url) };
}

test('starter creates independent editable timelines in both languages without user media or models', async (t) => {
  const { root, client } = await setup(t);
  const zh = await client.initializeDemo({ locale: 'zh' });
  const en = await client.initializeDemo({ locale: 'en' });
  const second = await client.initializeDemo({ locale: 'zh' });
  assert.notEqual(zh.id, second.id);
  assert.notEqual(zh.project.id, second.project.id);
  assert.notEqual(zh.previewUrl, second.previewUrl);
  for (const s of [zh, en]) {
    assert.equal(s.example, 'starter');
    assert.equal(duration(s.project), ticks(18));
    assert.deepEqual(
      s.project.timeline.tracks.map((t) => t.items.length),
      [8, 3, 1, 1, 1]
    );
    assert.ok(
      s.project.timeline.tracks[1].items.every(
        (i) =>
          ['flower-frost', 'flower-gold'].includes(i.clip.text.template.id) &&
          i.clip.automation['visual.positionY'].keyframes.length > 1
      )
    );
    assert.equal(s.project.timeline.tracks[2].items[0].clip.type, 'pagx-clip');
    assert.match(s.project.timeline.tracks[2].items[0].clip.pagx.xml, /#a4e0d2/);
    assert.equal(s.project.assets.length, 2);
    const audio = s.project.assets.find((a) => a.kind === 'audio');
    const picture = s.project.assets.find((a) => a.kind === 'image');
    assert.equal(audio.duration, ticks(18));
    assert.equal(audio.streams[0].sample_rate, 48000);
    const imageResponse = await fetch(client.mediaUrl(s.id, picture.id));
    assert.equal(imageResponse.status, 200);
    assert.equal(
      Buffer.from(await imageResponse.arrayBuffer())
        .subarray(0, 2)
        .toString('hex'),
      'ffd8'
    );
    const response = await fetch(client.mediaUrl(s.id, audio.id));
    assert.equal(response.status, 200);
    const wav = Buffer.from(await response.arrayBuffer());
    assert.equal(wav.toString('ascii', 0, 4), 'RIFF');
    assert.ok(wav.subarray(78).some((value) => value !== 0));
  }
  assert.match(zh.project.timeline.tracks[0].items[0].clip.text.content, /灵感/);
  assert.match(en.project.timeline.tracks[0].items[0].clip.text.content, /ideas/);
  const title = zh.project.timeline.tracks[1].items[0];
  const changed = await client.editSession(
    zh.id,
    [{ action: 'set_text', itemId: title.id, content: '我的作品' }],
    zh.version
  );
  assert.equal(changed.project.timeline.tracks[1].items[0].clip.text.content, '我的作品');
  assert.equal(
    (await client.getSession(second.id)).project.timeline.tracks[1].items[0].clip.text.content,
    '成为作品'
  );
  await assert.rejects(
    client.request('/assets', {
      method: 'POST',
      body: JSON.stringify({ path: resolve('dist/web/starter/starter.html') })
    }),
    /授权/
  );
  await assert.rejects(client.initializeDemo({ locale: 'fr' }), /zh.*en/);
  assert.deepEqual(
    await readdir(root),
    [],
    'initialization does not download models or write user storage'
  );
});

test('starter saves and reopens as a portable project with editable animation, recipes and narration', async (t) => {
  const { root, client } = await setup(t);
  const original = await client.initializeDemo({ locale: 'en', name: 'Welcome' });
  const saved = await client.saveProject(original.id, original.version, root);
  const fresh = await setup(t, { roots: [root] });
  const reopened = await fresh.client.openProject(saved.path);
  const pagx = (s) => s.project.timeline.tracks[2].items[0].clip.pagx;
  assert.deepEqual(pagx(reopened), pagx(original));
  assert.equal(reopened.project.timeline.tracks[1].items[0].clip.text.template.id, 'flower-frost');
  for (const asset of original.project.assets) {
    const copied = reopened.project.assets.find((a) => a.kind === asset.kind);
    assert.deepEqual(await readFile(copied.path), await readFile(asset.path));
  }
  assert.deepEqual(
    reopened.project.timeline.tracks[3].items[0].clip.automation,
    original.project.timeline.tracks[3].items[0].clip.automation
  );
  assert.equal(duration(reopened.project), ticks(18));
});

test('packaged CLI exposes the initialization skill and opens a populated initial session', async (t) => {
  const exec = promisify(execFile);
  const printed = await exec(process.execPath, [
    resolve('dist/bin/videocut.mjs'),
    '--print-setup-skill'
  ]);
  assert.match(printed.stdout, /name: initialize-demo/);
  assert.match(printed.stdout, /initialize_demo/);
  const { server } = await setup(t, { initialDemo: { locale: 'en' } });
  assert.equal(server.initialSession.example, 'starter');
  assert.equal(server.initialSession.project.timeline.tracks.length, 5);
  assert.equal((await fetch(server.initialSession.previewUrl)).status, 200);
});

test('browser launch uses argument arrays on macOS, Windows and Linux', () => {
  const url = 'http://127.0.0.1:4318/projects/ffclip-灵感开始的地方';
  assert.deepEqual(browserCommand(url, 'darwin'), ['open', [url]]);
  assert.deepEqual(browserCommand(url, 'win32'), [
    'rundll32.exe',
    ['url.dll,FileProtocolHandler', url]
  ]);
  assert.deepEqual(browserCommand(url, 'linux'), ['xdg-open', [url]]);
  assert.throws(() => browserCommand('https://example.com'), /local/);
});
