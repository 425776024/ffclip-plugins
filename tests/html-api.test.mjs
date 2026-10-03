import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, mkdir, rm, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';
import { importHtml } from '../packages/server/html-import.mjs';

test('local HTML import freezes relative styles/scripts/images and fences resources', async (t) => {
  const root = await mkdtemp(join(tmpdir(), 'videocut-html-api-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(join(root, 'art'));
  await writeFile(
    join(root, 'art', 'graphic.svg'),
    '<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"/></svg>'
  );
  await writeFile(join(root, 'style.css'), '.card{background-image:url(art/graphic.svg)}');
  await writeFile(join(root, 'motion.js'), 'window.tick=t=>document.body.dataset.time=t;');
  await writeFile(
    join(root, 'title.html'),
    '<link rel="stylesheet" href="style.css"><img src="art/graphic.svg"><script src="https://cdn.jsdelivr.net/npm/gsap@3/dist/gsap.min.js"></script><script src="motion.js"></script>'
  );
  const server = await startServer({ roots: [root], port: 0 });
  t.after(() => server.close());
  const client = new VideoCutClient(server.url);
  await client.connect();
  const imported = await client.importHtml(join(root, 'title.html'), {
    width: 320,
    height: 180,
    duration: 240000,
    transparent: true
  });
  assert.equal(imported.html.width, 320);
  assert.equal(imported.html.duration, 240000);
  assert.match(imported.html.html, /data:image\/svg\+xml;base64,/);
  assert.match(imported.html.html, /window.tick/);
  assert.doesNotMatch(imported.html.html, /cdn.jsdelivr|src="motion.js"|href="style.css"/);
  await assert.rejects(
    client.request('/html-frames', {
      method: 'POST',
      body: JSON.stringify({ html: imported.html, time: 0, rasterScale: 0.5 })
    }),
    /原始尺寸/
  );
  const listing = await client.listFiles(root);
  assert.ok(listing.entries.some((entry) => entry.name === 'title.html'));
  const session = await client.createSession();
  const added = await client.editSession(
    session.id,
    [{ action: 'add_html_clip', html: imported.html }],
    session.version
  );
  assert.equal(added.project.timeline.tracks[0].items[0].clip.type, 'html-clip');
  await assert.rejects(client.exportProject(session.id, added.version, root), /HTML.*MP4/);
  await writeFile(join(root, 'external.html'), '<img src="https://example.com/graphic.png">');
  await assert.rejects(client.importHtml(join(root, 'external.html')), /本地授权/);
  await symlink('/etc/passwd', join(root, 'secret.js'));
  await writeFile(join(root, 'unsafe.html'), '<script src="secret.js"></script>');
  await assert.rejects(client.importHtml(join(root, 'unsafe.html')), /授权/);
});

test('HTML importer rejects embedded media and bounds bundled byte extent', async (t) => {
  const root = await mkdtemp(join(tmpdir(), 'videocut-html-limits-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const allowed = async (path) => path;
  await writeFile(join(root, 'media.html'), '<video src="data:video/mp4;base64,AAAA"></video>');
  await assert.rejects(importHtml(join(root, 'media.html'), {}, allowed), /音视频/);
  await writeFile(join(root, 'big.html'), 'a'.repeat(1048577));
  await assert.rejects(importHtml(join(root, 'big.html'), {}, allowed), /1 MiB/);
});
