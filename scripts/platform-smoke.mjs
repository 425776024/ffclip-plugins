// Run against an installed release, without dev dependencies, native bridges or model downloads.
import assert from 'node:assert/strict';
import { mkdtemp, rename, rm, readFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { execFileSync } from 'node:child_process';

const installation = resolve(process.argv[2] || 'node_modules/@ffclip-com/videocut');
const load = (path) => import(pathToFileURL(join(installation, path)).href);
const [{ startServer }, { VideoCutClient, addText, addHtmlClip, ticks }] = await Promise.all([
  load('dist/server/index.mjs'), load('dist/client/index.mjs')
]);
const temporary = await mkdtemp(join(tmpdir(), 'videocut-installed-platform-'));
let server;
try {
  const help = execFileSync(process.execPath, [join(installation, 'dist/bin/videocut.mjs'), '--help'], { encoding: 'utf8' });
  assert.match(help, /\.vcutweb/);
  const options = { port: 0, roots: [temporary], nativeBridge: join(temporary, 'absent-bridge'),
    ttsModelDir: join(temporary, 'tts'), asrModelDir: join(temporary, 'asr'), visionModelDir: join(temporary, 'vision') };
  server = await startServer(options);
  let client = new VideoCutClient(server.url);
  assert.equal((await client.connect()).nativeExport, false);
  const session = await client.createSession();
  addText(session.project, { content: '跨平台 Unicode 🚀', template: { id: 'portable-flower', version: 1,
    recipe: { base: 'flower-style-03', backdrop: 'bubble-tile' } } });
  addHtmlClip(session.project, { html: { html: '<html><body><script>window.__videocut={render(){}}</script></body></html>',
    width: 320, height: 180, duration: ticks(2), transparent: true, variables: { title: 'Editable' } } });
  const updated = await client.updateSession(session.id, session.project, session.version);
  await assert.rejects(client.saveProject(session.id, session.version, temporary), (error) => error.status === 409);
  const receipt = await client.saveProject(updated.id, updated.version, temporary);
  const moved = join(temporary, '跨平台重开.vcutweb');
  await server.close(); server = undefined;
  await rename(receipt.path, moved);
  server = await startServer({ ...options, initialProjectPath: moved });
  client = new VideoCutClient(server.url);
  const reopened = server.initialSession;
  const clips = reopened.project.timeline.tracks.flatMap((track) => track.items.map((item) => item.clip));
  assert.equal(clips.find((clip) => clip.html).html.variables.title, 'Editable');
  const text = clips.find((clip) => clip.text).text;
  assert.equal(text.template.recipe.backdrop, 'bubble-tile');
  const manifest = await fetch(`${server.url}${text.template.resourceBase}templates/com.videocut.text.qt-type.flower-style-03/manifest.json`);
  assert.equal(manifest.status, 200);
  await assert.rejects(client.renderVideo(reopened.id, reopened.version, temporary), (error) => error.code === 'BROWSER_REQUIRED');
  const models = await client.listTtsVoices();
  assert.deepEqual(models.installedDtypes, []);
  const receiptData = JSON.parse(await readFile(join(installation, 'dist/web/tts-runtime/ENGLISH-SOURCE.json'), 'utf8'));
  assert.equal(receiptData.headtts.license, 'MIT');
  console.log(JSON.stringify({ platform: process.platform, arch: process.arch, node: process.version,
    installation, passed: true, checks: ['installed-cli', 'api', 'version-conflict', 'portable-save',
      'move-and-restart', 'editable-html-and-recipe', 'frozen-resources', 'browser-required', 'no-model-download'],
    browserRendering: 'requires separate browser acceptance', nativeBridge: 'not part of portable release' }, null, 2));
} finally {
  await server?.close();
  await rm(temporary, { recursive: true, force: true });
}
