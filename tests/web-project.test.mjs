import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, readFile, rename, rm, readdir, symlink, realpath } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';
import { createProject, addAsset, addText, addHtmlClip, ticks } from '../packages/core/project.mjs';
import { saveWebProject, openWebProject } from '../packages/server/web-project.mjs';
import { existsSync } from 'node:fs';

test('native API roundtrip retains custom recipe and per-clip font size/color through the updated bridge', async (t) => {
  const nativeBridge = resolve('.local/bin/videocut-bridge');
  if (!existsSync(nativeBridge)) return t.skip('A platform-compatible native bridge is required');
  const root = await realpath(await mkdtemp(join(tmpdir(), 'videocut-native-api-')));
  const server = await startServer({ port: 0, roots: [root], nativeBridge });
  t.after(async () => { await server.close(); await rm(root, { recursive: true, force: true }); });
  const project = createProject('Styled native recipe');
  const template = { id: 'native-recipe-style', version: 1,
    recipe: { base: 'flower-style-38', backdrop: 'bubble-nine-slice', animation: 'anim-lua-letter-transform' },
    style: { color: '#ff5c70', fontSize: 180 } };
  addText(project, { content: '发布验收', template });
  const client = new VideoCutClient(server.url);
  const session = await client.createSession(project);
  const receipt = await client.exportProject(session.id, session.version, root);
  const reopened = await client.openProject(receipt.path);
  const value = reopened.project.timeline.tracks[0].items[0].clip.text;
  assert.equal(value.content, '发布验收');
  assert.deepEqual(value.template.recipe, template.recipe);
  assert.deepEqual(value.template.style, template.style);
});

test('portable projects retain editable HTML/recipes/keys and frozen resources across move, source removal and server restart', async (t) => {
  const root = await realpath(await mkdtemp(join(tmpdir(), 'videocut-portable-')));
  let server;
  t.after(async () => { await server?.close(); await rm(root, { recursive: true, force: true }); });
  const source = join(root, 'original.png');
  await writeFile(source, 'fixture-media');
  const project = createProject('完整工程');
  const image = addAsset(project, { id: 'fixture-image', name: 'Image', path: source, kind: 'image',
    duration: ticks(8), size: 13, width: 320, height: 180, hasAudio: false });
  image.clip.visual.opacity = 0.65;
  addText(project, { content: '发布验收', template: { id: 'custom-flower', version: 1,
    recipe: { base: 'flower-style-38', backdrop: 'bubble-nine-slice', animation: 'anim-lua-letter-transform' } } });
  const html = addHtmlClip(project, { html: { html: '<!doctype html><html><body><div>Title</div><script>window.__videocut={render(){}}</script></body></html>',
    width: 320, height: 180, duration: ticks(8), transparent: true, variables: { title: '原稿', count: 3, active: true } } });
  server = await startServer({ roots: [root], port: 0, nativeBridge: join(root, 'missing-bridge') });
  let client = new VideoCutClient(server.url);
  const session = await client.createSession(project);
  await assert.rejects(client.saveProject(session.id, 7, root), (error) => error.status === 409);
  assert.deepEqual(await readdir(root), ['original.png']);
  const saved = await client.saveProject(session.id, session.version, root);
  assert.equal(saved.format, 'videocut.web-project');
  assert.equal((await client.getSession(session.id)).projectPath, saved.path);
  const manifest = JSON.parse(await readFile(join(saved.path, 'project.json'), 'utf8'));
  assert.ok(manifest.project.assets[0].path.startsWith('media/'));
  assert.equal(manifest.project.assets[0].path.includes(root), false);
  assert.ok(Object.keys(manifest.files).some((file) => file.startsWith('resources/text-templates/')));
  const moved = join(root, 'moved.vcutweb');
  await server.close(); server = undefined;
  await rename(saved.path, moved); await rm(source);
  // No installed template directory: reopening must use the project's frozen closure.
  server = await startServer({ roots: [root], port: 0, staticDir: join(root, 'absent-static'),
    nativeBridge: join(root, 'missing-bridge'), initialProjectPath: moved });
  client = new VideoCutClient(server.url);
  const reopened = server.initialSession;
  assert.equal(reopened.projectPath, moved);
  const items = reopened.project.timeline.tracks.flatMap((track) => track.items);
  assert.deepEqual(items.find((i) => i.id === html.id).clip.html, html.clip.html);
  assert.equal(items.find((i) => i.id === image.id).clip.visual.opacity, 0.65);
  const text = items.find((i) => i.clip.text).clip.text;
  assert.deepEqual(text.template.recipe, { base: 'flower-style-38', backdrop: 'bubble-nine-slice', animation: 'anim-lua-letter-transform' });
  const resource = `${server.url}${text.template.resourceBase}templates/com.videocut.text.qt-type.flower-style-38/manifest.json`;
  assert.equal((await fetch(resource)).status, 200);
  assert.equal(await readFile(reopened.project.assets[0].path, 'utf8'), 'fixture-media');
  const edited = await client.editSession(reopened.id, [{ action: 'set_html_clip', itemId: html.id,
    html: { ...html.clip.html, variables: { title: '重开后编辑', count: 4, active: false } } }], reopened.version);
  const again = await client.saveProject(edited.id, edited.version, root);
  const second = await client.openProject(again.path);
  assert.equal(second.project.timeline.tracks.flatMap((track) => track.items).find((i) => i.id === html.id).clip.html.variables.title, '重开后编辑');
  assert.equal((await fetch(resource)).status, 200);
});

test('portable packages reject corrupt content, traversal, symlinks, changed source and existing destinations', async (t) => {
  const root = await realpath(await mkdtemp(join(tmpdir(), 'videocut-portable-security-')));
  t.after(() => rm(root, { recursive: true, force: true }));
  const source = join(root, 'source.png'); await writeFile(source, '1234');
  const project = createProject();
  addAsset(project, { id: 'source', name: 'Image', path: source, kind: 'image', duration: ticks(2), size: 4,
    width: 32, height: 32, hasAudio: false });
  const path = join(root, 'saved.vcutweb');
  await saveWebProject(project, path, resolve('dist/web'));
  const bytes = await readFile(join(path, 'project.json'));
  await assert.rejects(saveWebProject(project, path, resolve('dist/web')), (error) => error.code === 'EEXIST');
  assert.deepEqual(await readFile(join(path, 'project.json')), bytes);
  const manifest = JSON.parse(bytes), resource = manifest.project.assets[0].path;
  await writeFile(join(path, resource), 'corrupt');
  await assert.rejects(openWebProject(path), /校验失败/);
  await rm(join(path, resource)); await symlink(source, join(path, resource));
  await assert.rejects(openWebProject(path), /符号链接/);
  manifest.files['../source.png'] = { sha256: 'a'.repeat(64), size: 4 };
  delete manifest.files[resource];
  await writeFile(join(path, 'project.json'), JSON.stringify(manifest));
  await assert.rejects(openWebProject(path), /清单无效/);
  await writeFile(source, 'changed');
  await assert.rejects(saveWebProject(project, join(root, 'failed.vcutweb'), resolve('dist/web')), /素材已变更/);
  assert.equal((await readdir(root)).some((file) => file.startsWith('.videocut-save-') || file === 'failed.vcutweb'), false);
});
