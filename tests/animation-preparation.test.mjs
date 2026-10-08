import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';
import { ticks } from '../packages/core/project.mjs';
const html = {
  html: '<style>body{margin:0;width:320px;height:180px}div{width:40px;height:30px;background:red;animation:slide 2s linear both}@keyframes slide{to{transform:translateX(80px)}}</style><div></div>',
  width: 320,
  height: 180,
  duration: ticks(2),
  transparent: true
};
test(
  'HTML authoring defaults to PAGX, reports fallback, preserves input and never bypasses authorization',
  { timeout: 30000 },
  async (t) => {
    const root = await mkdtemp(join(tmpdir(), 'videocut-prepare-'));
    const server = await startServer({
      port: 0,
      roots: [root],
      visionModelDir: join(root, 'vision')
    });
    t.after(async () => {
      await server.close();
      await rm(root, { recursive: true, force: true });
    });
    const client = new VideoCutClient(server.url);
    await client.connect();
    const source = JSON.stringify(html);
    const converted = await client.prepareAnimation({ html });
    assert.equal(converted.type, 'pagx-clip');
    assert.equal(converted.conversion.fallback, false);
    assert.equal(JSON.stringify(html), source);
    const unsupported = {
      ...html,
      html: '<div style="transform:rotateY(20deg);width:40px;height:40px;background:red"></div>'
    };
    const fallback = await client.prepareAnimation({ html: unsupported });
    assert.equal(fallback.type, 'html-clip');
    assert.equal(fallback.conversion.fallback, true);
    assert.match(fallback.conversion.reason, /3D/);
    assert.deepEqual(fallback.html, unsupported);
    const explicit = await client.prepareAnimation({ html, renderer: 'html' });
    assert.equal(explicit.type, 'html-clip');
    assert.equal(explicit.conversion.fallback, false);
    const path = join(root, 'input.html');
    await writeFile(path, html.html);
    const imported = await client.prepareAnimation({
      path,
      width: 320,
      height: 180,
      duration: ticks(2),
      transparent: true
    });
    assert.equal(imported.type, 'pagx-clip');
    await assert.rejects(client.prepareAnimation({ path: '/etc/passwd' }), /授权/);
    await assert.rejects(client.prepareAnimation({ html: { ...html, width: 0 } }), /宽度/);
    const session = await client.createSession();
    const added = await client.editSession(
      session.id,
      [{ action: 'add_pagx_clip', pagx: converted.pagx }],
      session.version
    );
    assert.equal(added.project.timeline.tracks[0].items[0].clip.type, 'pagx-clip');
    const retained = await client.editSession(
      session.id,
      [{ action: 'add_html_clip', html: fallback.html }],
      added.version
    );
    assert.equal(retained.project.timeline.tracks[0].items[0].clip.type, 'html-clip');
  }
);
