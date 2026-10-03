import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, rm, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { request } from 'node:http';
import { startServer } from '../packages/server/index.mjs';
import {
  VideoCutClient,
  createProject,
  addAsset,
  ticks,
  identity
} from '../packages/client/index.mjs';

test('readable preview links resolve the right cut across duplicate names, renames and closure', async (t) => {
  const server = await startServer({ port: 0 });
  t.after(() => server.close());
  const client = new VideoCutClient(server.url);
  const first = await client.createSession(createProject('我的 花字作品'));
  const second = await client.createSession(createProject('我的 花字作品'));
  const path = (s) => new URL(s.previewUrl).pathname;
  assert.equal(decodeURIComponent(path(first)), '/projects/我的-花字作品');
  assert.equal(first.previewUrl, `${server.url}/projects/我的-花字作品`);
  assert.equal(decodeURIComponent(path(second)), '/projects/我的-花字作品-2');
  assert.equal(new URL(first.previewUrl).search, '');
  assert.equal(first.projectPath, null);
  assert.equal((await client.resolveSession(path(first))).id, first.id);
  assert.equal((await client.resolveSession(path(second))).id, second.id);
  assert.equal((await fetch(`${server.url}/api/sessions`)).status, 401);
  assert.equal(
    (
      await fetch(
        `${server.url}/api/sessions/resolve?previewPath=${encodeURIComponent(path(first))}`
      )
    ).status,
    401
  );
  const listing = await client.listSessions();
  assert.deepEqual(
    listing.map((s) => s.id),
    [first.id, second.id]
  );
  assert.equal(listing[0].name, first.project.name);
  assert.equal(listing[0].previewUrl, first.previewUrl);
  assert.equal(listing[0].projectPath, null);

  const renamed = await client.editSession(
    first.id,
    [{ action: 'configure_project', name: '改名 / # 百分比 100% ?' }],
    first.version
  );
  assert.equal(decodeURIComponent(path(renamed)), '/projects/改名-百分比-100');
  assert.equal((await client.resolveSession(path(first))).id, first.id);
  assert.equal((await client.resolveSession(path(first))).previewUrl, renamed.previewUrl);
  assert.equal((await client.resolveSession(path(renamed))).id, first.id);
  // Internal-id clients and legacy ?session= links can still attach to the same cut.
  assert.equal((await client.getSession(first.id)).project.name, renamed.project.name);
  const restored = await client.editSession(first.id, [{ action: 'undo' }], renamed.version);
  assert.equal(restored.previewUrl, first.previewUrl);

  await client.closeSession(first.id);
  const replacement = await client.createSession(createProject('我的 花字作品'));
  assert.equal(decodeURIComponent(path(replacement)), '/projects/我的-花字作品-3');
  await assert.rejects(client.resolveSession(path(first)), (e) => e.status === 404);
  await assert.rejects(client.resolveSession('/projects/unknown'), (e) => e.status === 404);
  await assert.rejects(
    client.resolveSession('/projects/../api/bootstrap'),
    (e) => e.status === 404
  );
  assert.equal(
    (await client.listSessions()).some((s) => s.id === first.id),
    false
  );
  const unicode = await client.createSession(createProject('𠮷'.repeat(81)));
  assert.equal([...decodeURIComponent(path(unicode)).slice('/projects/'.length)].length, 80);
  assert.equal((await client.resolveSession(path(unicode))).id, unicode.id);
});

test('local server isolates sessions, streams files, rejects stale edits and fences paths', async (t) => {
  const temp = await mkdtemp(join(tmpdir(), 'videocut-web-test-'));
  const { realpath } = await import('node:fs/promises');
  const root = await realpath(temp);
  await writeFile(join(root, 'test.mp4'), Buffer.from('0123456789'));
  await symlink('/etc', join(root, 'outside'));
  const server = await startServer({ port: 0, roots: [root] });
  t.after(async () => {
    await server.close();
    await rm(root, { recursive: true, force: true });
  });
  const client = new VideoCutClient(server.url);
  await client.connect();
  assert.equal((await fetch(`${server.url}/api/files`)).status, 401);
  assert.equal((await fetch(`${server.url}/api/clipboard/assets`, { method: 'POST' })).status, 401);
  assert.equal((await fetch(`${server.url}/api/clipboard/assets`, {
    method: 'POST', headers: { Origin: 'https://evil.example', Authorization: `Bearer ${client.token}` }
  })).status, 403);
  assert.equal(
    (await fetch(`${server.url}/api/bootstrap`, { headers: { Origin: 'https://evil.example' } }))
      .status,
    403
  );
  const rebindingStatus = await new Promise((resolve, reject) => {
    const req = request(
      `${server.url}/api/bootstrap`,
      { headers: { Host: 'attacker.example' } },
      (res) => {
        res.resume();
        resolve(res.statusCode);
      }
    );
    req.on('error', reject);
    req.end();
  });
  assert.equal(rebindingStatus, 403);
  await assert.rejects(client.listFiles('/etc'), /授权/);
  await assert.rejects(client.listFiles(join(root, 'outside')), /授权/);
  const a = await client.createSession(),
    b = await client.createSession();
  assert.notEqual(a.id, b.id);
  assert.equal(b.project.timeline.tracks.length, 0);
  const p = a.project;
  const media = {
    id: identity('asset'),
    name: 'test.mp4',
    kind: 'video',
    path: join(root, 'test.mp4'),
    duration: ticks(1),
    size: 10,
    width: 320,
    height: 180,
    hasAudio: false
  };
  addAsset(p, media);
  const next = await client.updateSession(a.id, p, 0);
  assert.equal(next.version, 1);
  const concurrent = await Promise.allSettled([
    client.updateSession(a.id, p, 1),
    client.updateSession(a.id, p, 1)
  ]);
  assert.equal(concurrent.filter((r) => r.status === 'fulfilled').length, 1);
  assert.equal(concurrent.find((r) => r.status === 'rejected').reason.status, 409);
  const range = await fetch(client.mediaUrl(a.id, media.id), { headers: { Range: 'bytes=3-6' } });
  assert.equal(range.status, 206);
  assert.equal(await range.text(), '3456');
  const suffix = await fetch(client.mediaUrl(a.id, media.id), { headers: { Range: 'bytes=-2' } });
  assert.equal(await suffix.text(), '89');
  assert.equal(
    (await fetch(client.mediaUrl(a.id, media.id), { headers: { Range: 'bytes=99-' } })).status,
    416
  );
  const controller = new AbortController();
  const stream = await fetch(client.eventsUrl(a.id), { signal: controller.signal });
  const reader = stream.body.getReader();
  assert.match(new TextDecoder().decode((await reader.read()).value), /"version":2/);
  p.name = 'Agent edit';
  await client.updateSession(a.id, p, 2);
  assert.match(new TextDecoder().decode((await reader.read()).value), /Agent edit/);
  const command = await client.controlPreview(a.id, 'seek', 0.5);
  assert.equal(command.deliveredTo, 1);
  const event = new TextDecoder().decode((await reader.read()).value);
  assert.match(event, /event: preview-control/);
  assert.match(event, /"timeSeconds":0.5/);
  await client.reportPreview(a.id, {
    clientId: 'test-browser',
    version: 3,
    commandSequence: command.sequence,
    playing: false,
    timeSeconds: 0.5,
    media: []
  });
  const status = await client.previewStatus(a.id);
  assert.equal(status.clients[0].commandSequence, command.sequence);
  assert.equal(status.clients[0].version, 3);
  assert.equal((await client.getSession(a.id)).version, 3);
  await assert.rejects(client.controlPreview(a.id, 'seek'), /timeSeconds/);
  await assert.rejects(client.controlPreview(a.id, 'seek', -1), /秒|时间/);
  controller.abort();
  assert.equal((await client.getSession(b.id)).project.timeline.tracks.length, 0);
  await client.closeSession(a.id);
  await assert.rejects(client.getSession(a.id), /会话已结束/);
});
