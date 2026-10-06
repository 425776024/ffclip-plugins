import test from 'node:test';
import assert from 'node:assert/strict';
import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, mkdir, cp, readFile, writeFile, rm, rename, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { promisify } from 'node:util';
import { existsSync } from 'node:fs';
import { checkPackage } from '../scripts/check-package.mjs';
import { buildPlugin } from '../scripts/build-plugin.mjs';
import { root, listFiles, verifyDist, assertArtifact, digest } from '../scripts/release-files.mjs';

const exec = promisify(execFile);
async function temporary(t) {
  const path = await mkdtemp(join(tmpdir(), 'videocut-release-'));
  t.after(() => rm(path, { recursive: true, force: true }));
  return path;
}

test(
  'actual npm tarball installs without source and exposes working CLI, API and types',
  { timeout: 30000 },
  async (t) => {
    const work = await temporary(t);
    const expected = await checkPackage();
    const expectedPosters = expected.files.filter(
      (file) => file.path.startsWith('dist/web/assets/html-poster-') && file.path.endsWith('.png')
    );
    assert.ok(expectedPosters.length > 0);
    const { stdout } = await exec(
      'npm',
      ['pack', '--ignore-scripts', '--json', '--pack-destination', work],
      { cwd: root }
    );
    const [pack] = JSON.parse(stdout);
    const tarball = join(work, pack.filename);
    const archive = await exec('tar', ['-tzf', tarball]);
    assert.deepEqual(
      archive.stdout
        .trim()
        .split('\n')
        .map((p) => p.replace(/^package\//, ''))
        .sort(),
      expected.files.map((f) => f.path).sort()
    );
    const consumer = join(work, 'consumer');
    await mkdir(consumer);
    await exec('npm', [
      'install',
      '--prefix',
      consumer,
      '--ignore-scripts',
      '--omit=dev',
      '--no-audit',
      '--no-fund',
      tarball
    ]);
    const installed = join(consumer, 'node_modules/@ffclip-com/videocut');
    const files = await listFiles(installed);
    assert.ok(files.includes('plugins/videocut-local/skills/visual-understanding/SKILL.md'));
    assert.ok(files.includes('plugins/videocut-local/skills/initialize-demo/SKILL.md'));
    assert.equal(
      files.some((path) => path.endsWith('.onnx')),
      false,
      'vision weights require initialization consent and are not bundled'
    );
    assert.ok(files.every((path) => !/\.(ttf|otf|ttc|otc|woff2?)$/i.test(path)));
    assert.ok(
      files.every((p) => !/^(?:src|packages|native|scripts|tests)\//.test(p) && !p.endsWith('.map'))
    );
    for (const p of files.filter((p) => p.startsWith('dist/')))
      assertArtifact(p, await readFile(join(installed, p)));
    const help = await exec(join(consumer, 'node_modules/.bin/videocut-web'), ['--help'], {
      cwd: work
    });
    assert.match(help.stdout, /Usage: videocut-web/);
    const skill = await exec(
      join(consumer, 'node_modules/.bin/videocut-web'),
      ['--print-setup-skill'],
      { cwd: work }
    );
    assert.match(skill.stdout, /name: initialize-demo/);
    await writeFile(
      join(consumer, 'smoke.mjs'),
      String.raw`
    import assert from 'node:assert/strict';
    import { VideoCutClient, addText } from '@ffclip-com/videocut';
    import { ticks } from '@ffclip-com/videocut/core';
    import { startServer } from '@ffclip-com/videocut/server';
    const server = await startServer({port: 0, roots: [process.cwd()]});
    try {
      const client = new VideoCutClient(server.url);
      await client.connect();
      const starter = await client.initializeDemo({ locale: 'en' });
      assert.equal(starter.example, 'starter');
      assert.equal(starter.project.timeline.tracks.length, 5);
      assert.equal(starter.project.timeline.tracks[2].items[0].clip.html.duration, ticks(18));
      for (const asset of starter.project.assets)
        assert.equal((await fetch(client.mediaUrl(starter.id, asset.id))).status, 200);
      const session = await client.createSession();
      addText(session.project, {content: 'Release smoke', length: ticks(2)});
      const result = await client.updateSession(session.id, session.project, 0);
      assert.equal(result.version, 1);
      const manifestUrl = new URL('/text-templates/templates/com.videocut.text.qt-type.anim-studio-time-transform-softglow/manifest.json', server.url);
      const manifestResponse = await fetch(manifestUrl);
      assert.match(manifestResponse.headers.get('content-type'), /application\/json/);
      const manifest = await manifestResponse.json();
      for (const file of manifest.files) {
        const resource = await fetch(new URL(file.path, manifestUrl));
        assert.equal(resource.status, 200);
        assert.ok((await resource.arrayBuffer()).byteLength > 0);
      }
      const fontCatalog = await (await fetch(new URL('/api/fonts', server.url))).json();
      assert.ok(Array.isArray(fontCatalog.fonts));
      if (fontCatalog.defaults.sans) {
        const font = await fetch(new URL('/api/fonts/' + fontCatalog.defaults.sans, server.url));
        assert.match(font.headers.get('content-type'), /font\/(ttf|otf)/);
        assert.ok((await font.arrayBuffer()).byteLength > 1000);
      }
      const unknownFont = await fetch(new URL('/api/fonts/not-a-font-id', server.url));
      assert.equal(unknownFont.status, 404);
      const { readdir } = await import('node:fs/promises');
      const webAssets = await readdir('node_modules/@ffclip-com/videocut/dist/web/assets');
      const posters = webAssets.filter(name => /^html-poster-.*\.png$/.test(name));
      assert.equal(posters.length, ${expectedPosters.length});
      for (const name of posters) {
        const poster = await fetch(new URL('/assets/' + name, server.url));
        assert.equal(poster.status, 200);
        assert.match(poster.headers.get('content-type'), /image\/png/);
        assert.equal(Buffer.from(await poster.arrayBuffer()).subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
      }
      const wasmName = webAssets.find(name => name.endsWith('.wasm'));
      const wasm = await fetch(new URL('/assets/' + wasmName, server.url));
      assert.match(wasm.headers.get('content-type'), /application\/wasm/);
      assert.ok(WebAssembly.validate(await wasm.arrayBuffer()));
      const page = await fetch(result.previewUrl);
      assert.equal(page.status, 200);
      const html = await page.text();
      const assets = [...html.matchAll(/(?:src|href)="(\/assets\/[^\"]+)"/g)];
      assert.ok(assets.length >= 2);
      for (const [, path] of assets) {
        const asset = await fetch(new URL(path, server.url));
        assert.equal(asset.status, 200);
        assert.ok((await asset.text()).length > 100);
      }
    } finally { await server.close(); }
  `
    );
    await exec(process.execPath, ['smoke.mjs'], { cwd: consumer });
    await writeFile(
      join(consumer, 'types.mts'),
      `
    import { VideoCutClient, createProject, addText, editTimeline, type EditorCommand } from '@ffclip-com/videocut';
    import { ticks } from '@ffclip-com/videocut/core';
    import { startServer } from '@ffclip-com/videocut/server';
    const project = createProject();
    addText(project, { content: 'Types', length: ticks(2), template: { id: 'cube', version: 1 } });
    const server = await startServer({ port: 0, initialProject: project });
    const client = new VideoCutClient(server.url);
    await client.createSession(project);
    await client.initializeDemo({ locale: 'en' });
    const image = await client.describeImage('/authorized/image.png', 'Describe in Chinese.', { id: 'session', timeoutMs: 0 });
    const imageText: string | undefined = image.text;
    const video = await client.describeVideo('/authorized/video.mp4', 'Describe the action.', { segmentSeconds: 5, framesPerSegment: 3 });
    const segmentText: string | undefined = video.segments?.[0].text;
    const sourceTime: number | undefined = video.segments?.[0].samples[0].sourceSeconds;
    await client.waitVision('session', 'job', { timeoutMs: 0 });
    await client.cancelVision('session', 'job');
    // @ts-expect-error A video sampling count must be numeric.
    await client.describeVideo('/authorized/video.mp4', 'Describe.', { framesPerSegment: 'three' });
    const move: EditorCommand = { action: 'move_clips', itemIds: ['item-1'], deltaSeconds: 2 };
    const result = editTimeline(project, [move]);
    const patchCount: number = result.patches.length;
    const impact: string[] = result.changes.impacts;
    // @ts-expect-error Unknown command cannot enter the public contract.
    const unknown: EditorCommand = { action: 'not_a_command' };
    // @ts-expect-error Numeric properties cannot receive a string.
    const wrong: EditorCommand = { action: 'set_property', itemId: 'a', property: 'visual.opacity', value: 'opaque' };
    // @ts-expect-error Effect parameters are specific to the selected template.
    const wrongEffect: EditorCommand = { action: 'add_effect', itemId: 'a', templateId: 'blur', parameters: { preset: 'warm' } };
    await server.close();
  `
    );
    await exec(
      process.execPath,
      [
        join(root, 'node_modules/typescript/bin/tsc'),
        '--noEmit',
        '--strict',
        '--skipLibCheck',
        '--module',
        'NodeNext',
        '--target',
        'ES2022',
        'types.mts'
      ],
      { cwd: consumer }
    );
  }
);

test(
  'plugin rebuild removes old source; relocated bundle serves MCP and web assets',
  { timeout: 20000 },
  async (t) => {
    const work = await temporary(t);
    const output = join(work, 'videocut-local');
    const bridgePath = join(root, '.local/bin/videocut-bridge');
    const bridge = existsSync(bridgePath) ? bridgePath : undefined;
    await buildPlugin({ output, roots: [work], bridge });
    await mkdir(join(output, 'runtime/packages/server'), { recursive: true });
    await writeFile(join(output, 'runtime/packages/server/index.mjs'), 'PRIVATE OLD SOURCE');
    await writeFile(
      join(output, 'runtime/dist/web/stale.js.map'),
      '{"sourcesContent":["private"]}'
    );
    await buildPlugin({ output, roots: [work], bridge });
    assert.ok(
      (await listFiles(output)).every((p) => !p.includes('/packages/') && !p.endsWith('.map'))
    );
    const moved = join(work, 'moved', 'videocut-local');
    await mkdir(join(work, 'moved'));
    await rename(output, moved);
    const config = JSON.parse(await readFile(join(moved, '.mcp.json'))).mcpServers.videocut;
    assert.equal(config.command, 'node');
    assert.equal(config.cwd, '.');
    assert.ok(!JSON.stringify(config).includes(root));
    const child = spawn(config.command, config.args, {
      cwd: resolve(moved, config.cwd),
      stdio: ['pipe', 'pipe', 'pipe']
    });
    t.after(() => child.kill());
    child.stderr.resume();
    const pending = new Map();
    let counter = 0;
    createInterface({ input: child.stdout }).on('line', (line) => {
      const response = JSON.parse(line);
      pending.get(response.id)?.(response);
      pending.delete(response.id);
    });
    const rpc = (method, params = {}) =>
      new Promise((done) => {
        const id = ++counter;
        pending.set(id, done);
        child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
      });
    const init = await rpc('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: 'packaging-test', version: '1' }
    });
    assert.equal(init.result.protocolVersion, '2024-11-05');
    child.stdin.write(
      JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n'
    );
    const tools = await rpc('tools/list');
    assert.ok(tools.result.tools.some((t) => t.name === 'edit_timeline'));
    assert.ok(tools.result.tools.some((t) => t.name === 'open_project'));
    assert.ok(tools.result.tools.some((t) => t.name === 'synthesize_speech'));
    assert.ok(tools.result.tools.some((t) => t.name === 'cancel_tts'));
    for (const name of ['install_tts_model', 'synthesize_speech'])
      assert.deepEqual(
        tools.result.tools.find((t) => t.name === name).inputSchema.properties.dtype.enum,
        ['fp32']
      );
    for (const name of ['describe_image', 'describe_video'])
      assert.deepEqual(tools.result.tools.find((tool) => tool.name === name).inputSchema.required, [
        'path',
        'prompt'
      ]);
    const tool = async (name, args = {}) => {
      const response = await rpc('tools/call', { name, arguments: args });
      assert.ok(!response.result.isError, JSON.stringify(response));
      return JSON.parse(response.result.content[0].text);
    };
    const session = await tool('create_session');
    const voiceCatalog = await tool('list_voices');
    assert.equal(voiceCatalog.model, 'kokoro-v1.1-zh');
    assert.ok(voiceCatalog.voices.some((voice) => voice.id === 'zf_001'));
    assert.equal((await tool('get_tts_status', { id: session.id })).state, 'idle');
    const updated = await tool('add_text', { id: session.id, content: '打包插件', length: 240000 });
    assert.equal(updated.version, 1);
    assert.equal((await tool('get_session', { id: session.id })).version, 1);
    const absentBrowser = await rpc('tools/call', {
      name: 'render_video',
      arguments: { id: session.id, version: 1, directory: work }
    });
    assert.equal(absentBrowser.result.isError, true);
    assert.match(absentBrowser.result.content[0].text, /请打开作品网页/);
    if (bridge) {
      const copied = join(moved, 'runtime/native/videocut-bridge');
      assert.equal(digest(await readFile(copied)), digest(await readFile(bridge)));
      const native = await tool('export_project', { id: session.id, version: 1, directory: work });
      assert.ok(native.verified);
      const inspected = await exec(copied, ['inspect', native.path], { cwd: work });
      assert.equal(JSON.parse(inspected.stdout).texts[0].content, '打包插件');
      t.diagnostic(
        'Relocated plugin used its bundled native bridge to export and reopen a .vcut project'
      );
    } else {
      t.diagnostic('Optional native bridge unavailable; native plugin export not exercised');
    }
    const html = await (await fetch(session.previewUrl)).text();
    assert.match(html, /\/assets\/index-.*\.js/);
    const [, asset] = html.match(/src="(\/assets\/[^\"]+)"/);
    assert.equal((await fetch(new URL(asset, session.previewUrl))).status, 200);
    const exited = once(child, 'exit');
    child.stdin.end();
    assert.equal((await exited)[0], 0);
  }
);

test('release guard blocks added source, modified bundles, maps, symlinks and widened npm files', async (t) => {
  const fixture = await temporary(t);
  for (const path of [
    'dist',
    '.local/release-files.json',
    'plugins',
    'package.json',
    'README.md',
    'LICENSE',
    'THIRD_PARTY_NOTICES.md',
    'licenses/WebAV-MIT.txt'
  ]) {
    await mkdir(join(fixture, path, '..'), { recursive: true });
    await cp(join(root, path), join(fixture, path), { recursive: true });
  }
  const extra = join(fixture, 'dist/web/assets/private.js');
  await writeFile(extra, 'const privateSource = true;');
  await assert.rejects(verifyDist(fixture), /files changed/);
  await rm(extra);
  const bundle = join(fixture, 'dist/server/index.mjs');
  const original = await readFile(bundle);
  await writeFile(bundle, await readFile(join(root, 'packages/server/index.mjs')));
  await assert.rejects(verifyDist(fixture), /modified after build/);
  const inline = Buffer.concat([
    original,
    Buffer.from('\n//# sourceMappingURL=data:application/json;base64,e30=')
  ]);
  await writeFile(bundle, inline);
  const manifestPath = join(fixture, '.local/release-files.json');
  const manifest = JSON.parse(await readFile(manifestPath));
  manifest['dist/server/index.mjs'] = digest(inline);
  await writeFile(manifestPath, JSON.stringify(manifest));
  await assert.rejects(verifyDist(fixture), /Source map/);
  await writeFile(bundle, original);
  await cp(join(root, '.local/release-files.json'), manifestPath);
  await symlink(join(root, 'native/videocut_bridge.cpp'), extra);
  await assert.rejects(verifyDist(fixture), /symlink/);
  await rm(extra);
  const pkgPath = join(fixture, 'package.json');
  const pkg = JSON.parse(await readFile(pkgPath));
  pkg.files.push('native');
  await writeFile(pkgPath, JSON.stringify(pkg));
  await mkdir(join(fixture, 'native'));
  await writeFile(join(fixture, 'native/private.cpp'), 'PRIVATE SOURCE');
  await assert.rejects(checkPackage(fixture), /Unexpected: native\/private.cpp/);
});

test('plugin refuses source files masquerading as a native bridge and preserves unrelated output', async (t) => {
  const work = await temporary(t);
  const output = join(work, 'videocut-local');
  await assert.rejects(
    buildPlugin({ output, bridge: join(root, 'native/videocut_bridge.cpp') }),
    /compiled Mach-O/
  );
  await mkdir(output);
  await writeFile(join(output, 'keep.txt'), 'user data');
  await assert.rejects(buildPlugin({ output }));
  assert.equal(await readFile(join(output, 'keep.txt'), 'utf8'), 'user data');
});
