import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, rm, realpath } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  createSoftwareUpdates,
  compareVersions,
  installOfficialUpdate,
  officialPackage
} from '../packages/server/software-updates.mjs';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';

async function fixture(t, { source = false, version = '0.2.7' } = {}) {
  const directory = await mkdtemp(join(tmpdir(), 'ffclip-update-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const root = source
    ? join(directory, 'source')
    : join(directory, 'node_modules/@ffclip-com/videocut');
  await mkdir(root, { recursive: true });
  const manifest = (version) =>
    writeFile(join(root, 'package.json'), JSON.stringify({ name: officialPackage, version }));
  await manifest(version);
  return { directory, root, manifest };
}
const metadata =
  (version = '0.2.8', name = officialPackage) =>
  async () =>
    new Response(JSON.stringify({ name, version }));
async function settled(service) {
  for (let i = 0; i < 100; i++) {
    if (service.status().state !== 'installing') return service.status();
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  throw new Error('Update did not finish');
}

test('version ordering handles numeric segments, prereleases and build metadata', () => {
  for (const [a, b] of [
    ['0.2.10', '0.2.9'],
    ['1.0.0', '1.0.0-rc.1'],
    ['1.0.0-rc.10', '1.0.0-rc.2'],
    ['1.0.0-beta', '1.0.0-2'],
    ['1.0.0-beta.1', '1.0.0-beta']
  ]) {
    assert.equal(compareVersions(a, b), 1);
    assert.equal(compareVersions(b, a), -1);
  }
  assert.equal(compareVersions('1.2.3+one', '1.2.3+two'), 0);
  for (const invalid of ['1.2', '01.2.3', '1.2.3-01', 'latest', '1.2.3;echo'])
    assert.throws(() => compareVersions(invalid, '1.2.3'));
});

test('registry checks share requests, cache for six hours and never install automatically', async (t) => {
  const f = await fixture(t);
  let now = 1000,
    calls = 0,
    installs = 0;
  const service = await createSoftwareUpdates({
    packageRoot: f.root,
    now: () => now,
    fetchMetadata: async (url, options) => {
      calls++;
      assert.equal(url, 'https://registry.npmjs.org/%40ffclip-com%2Fvideocut/latest');
      assert.ok(options.signal instanceof AbortSignal);
      return new Response(JSON.stringify({ name: officialPackage, version: '0.2.8' }));
    },
    installPackage: async () => {
      installs++;
    }
  });
  t.after(() => service.close());
  const values = await Promise.all([service.check(), service.check()]);
  assert.equal(calls, 1);
  assert.equal(values[0].available, true);
  assert.equal(values[1].currentVersion, '0.2.7');
  now += 6 * 60 * 60 * 1000 - 1;
  await service.check();
  assert.equal(calls, 1);
  now++;
  await service.check();
  assert.equal(calls, 2);
  assert.equal(installs, 0);
});

test('equal or newer running versions do not offer a downgrade', async (t) => {
  for (const version of ['0.2.8', '0.3.0']) {
    const f = await fixture(t, { version });
    const service = await createSoftwareUpdates({ packageRoot: f.root, fetchMetadata: metadata() });
    t.after(() => service.close());
    assert.equal((await service.check()).available, false);
    assert.equal((await service.install()).state, 'idle');
  }
});

test('offline and invalid registry responses never claim the version is current', async (t) => {
  const f = await fixture(t);
  for (const fetchMetadata of [
    async () => {
      throw new Error('offline');
    },
    metadata('bad'),
    metadata('0.3.0-beta.1'),
    metadata('0.3.0', 'other'),
    async () => new Response('', { status: 404 })
  ]) {
    const service = await createSoftwareUpdates({ packageRoot: f.root, fetchMetadata });
    t.after(() => service.close());
    const status = await service.check();
    assert.equal(status.available, false);
    assert.equal(status.latestVersion, null);
    assert.equal(status.checkedAt, null);
    assert.ok(status.checkError);
    await assert.rejects(service.install(), (e) => e.statusCode === 503);
  }
});

test('source checkouts refuse automatic installation without invoking npm', async (t) => {
  const f = await fixture(t, { source: true });
  let installs = 0;
  const service = await createSoftwareUpdates({
    packageRoot: f.root,
    fetchMetadata: metadata(),
    installPackage: async () => {
      installs++;
    }
  });
  assert.equal((await service.check()).canInstall, false);
  await assert.rejects(service.install(), (e) => e.statusCode === 409);
  assert.equal(installs, 0);
  await service.close();
});

test('all saves complete before a single installation and the running version stays unchanged', async (t) => {
  const f = await fixture(t);
  let releaseSave,
    installs = 0;
  const savedProjects = [{ name: 'Cut', path: join(f.directory, 'Cut.vcutweb') }];
  const saved = new Promise((resolve) => {
    releaseSave = resolve;
  });
  const service = await createSoftwareUpdates({
    packageRoot: f.root,
    fetchMetadata: metadata(),
    prepareInstall: async () => {
      await saved;
      return savedProjects;
    },
    installPackage: async () => {
      installs++;
      await f.manifest('0.2.8');
    }
  });
  t.after(() => service.close());
  await Promise.all([service.install(), service.install()]);
  assert.equal(service.status().phase, 'saving');
  assert.equal(installs, 0);
  releaseSave();
  const status = await settled(service);
  assert.equal(installs, 1);
  assert.equal(status.state, 'installed');
  assert.equal(status.currentVersion, '0.2.7');
  assert.equal(status.installedVersion, '0.2.8');
  assert.deepEqual(status.savedProjects, savedProjects);
  await service.install();
  assert.equal(installs, 1);
});

test('failed saves and active jobs prevent installation', async (t) => {
  const f = await fixture(t);
  let installs = 0;
  const service = await createSoftwareUpdates({
    packageRoot: f.root,
    fetchMetadata: metadata(),
    prepareInstall: async () => {
      throw new Error('save failed');
    },
    installPackage: async () => {
      installs++;
    }
  });
  t.after(() => service.close());
  await assert.rejects(
    service.install(() => {
      throw new Error('job running');
    }),
    /job running/
  );
  assert.equal(service.status().state, 'idle');
  await service.install();
  assert.equal((await settled(service)).error, 'save failed');
  assert.equal(installs, 0);
});

test('failed installations can retry but success requires reading the installed manifest', async (t) => {
  const f = await fixture(t);
  let calls = 0;
  const service = await createSoftwareUpdates({
    packageRoot: f.root,
    fetchMetadata: metadata(),
    installPackage: async () => {
      if (++calls === 1) throw new Error('EACCES');
      if (calls > 2) await f.manifest('0.2.8');
    }
  });
  t.after(() => service.close());
  await service.install();
  assert.equal((await settled(service)).error, 'EACCES');
  await service.install();
  assert.equal((await settled(service)).state, 'error');
  await service.install();
  assert.equal((await settled(service)).state, 'installed');
});

test('npm installation targets the existing global or local/npx prefix without a shell', async (t) => {
  const f = await fixture(t);
  for (const global of [true, false]) {
    const calls = [];
    await installOfficialUpdate(f.root, {
      npmCli: '/npm path/npm-cli.js',
      runCommand: async (command, args) => {
        assert.equal(command, process.execPath);
        calls.push(args);
        return args[1] === 'root'
          ? global
            ? join(f.directory, 'node_modules')
            : '/different/global/node_modules'
          : '';
      }
    });
    const args = calls[1];
    assert.ok(args.includes(`${officialPackage}@latest`));
    assert.ok(args.includes('--ignore-scripts'));
    assert.ok(args.includes('--engine-strict'));
    assert.equal(args.includes('--global'), global);
    if (!global) assert.equal(args[args.indexOf('--prefix') + 1], await realpath(f.directory));
    assert.equal(args[0], '/npm path/npm-cli.js');
  }
});

test('update endpoints require local credentials and preserve source checkouts', async (t) => {
  const f = await fixture(t);
  const nativeFetch = globalThis.fetch;
  let registryCalls = 0;
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    if (String(url).startsWith('https://registry.npmjs.org/')) {
      registryCalls++;
      return metadata('99.0.0')();
    }
    return nativeFetch(url, options);
  });
  const server = await startServer({ roots: [f.directory], port: 0 });
  t.after(() => server.close());
  assert.equal((await nativeFetch(`${server.url}/api/updates`)).status, 401);
  assert.equal(
    (await nativeFetch(`${server.url}/api/updates/install`, { method: 'POST' })).status,
    401
  );
  assert.equal(registryCalls, 0);
  const client = new VideoCutClient(server.url);
  await client.connect();
  const status = await client.request('/updates');
  assert.equal(status.latestVersion, '99.0.0');
  assert.equal(status.canInstall, false);
  assert.equal(
    (
      await nativeFetch(`${server.url}/api/updates/install`, {
        method: 'POST',
        headers: { Origin: 'https://elsewhere.example', Authorization: `Bearer ${client.token}` }
      })
    ).status,
    403
  );
  await assert.rejects(
    client.request('/updates/install', { method: 'POST', body: '{}' }),
    (e) => e.status === 409
  );
  assert.equal(registryCalls, 1);
  assert.equal(JSON.parse(await readFile(join(f.root, 'package.json'))).version, '0.2.7');
});

test('bundled plugin updates replace the runtime and skills while preserving MCP arguments and native binaries', async (t) => {
  const f = await fixture(t, { source: true });
  const plugin = join(f.directory, 'videocut-local');
  const runtime = join(plugin, 'runtime');
  await mkdir(join(runtime, 'dist/bin'), { recursive: true });
  await mkdir(join(runtime, 'native'), { recursive: true });
  await mkdir(join(plugin, '.codex-plugin'), { recursive: true });
  await mkdir(join(plugin, 'skills'), { recursive: true });
  await writeFile(
    join(runtime, 'package.json'),
    JSON.stringify({ name: officialPackage, version: '0.2.7' })
  );
  await writeFile(join(runtime, 'dist/bin/videocut.mjs'), 'old runtime');
  await writeFile(join(runtime, 'native/bridge'), 'keep native');
  await writeFile(
    join(plugin, '.codex-plugin/plugin.json'),
    JSON.stringify({ name: 'videocut-local', version: '0.2.7' })
  );
  await writeFile(join(plugin, '.mcp.json'), 'keep authorized roots');
  const service = await createSoftwareUpdates({ packageRoot: runtime, fetchMetadata: metadata() });
  assert.equal((await service.check()).canInstall, true);
  await service.close();
  await installOfficialUpdate(runtime, {
    npmCli: '/npm-cli.js',
    runCommand: async (_command, args) => {
      const stage = args[args.indexOf('--prefix') + 1];
      const downloaded = join(stage, 'node_modules/@ffclip-com/videocut');
      const shippedPlugin = join(downloaded, 'plugins/videocut-local');
      await mkdir(join(downloaded, 'dist/bin'), { recursive: true });
      await mkdir(join(shippedPlugin, '.codex-plugin'), { recursive: true });
      await mkdir(join(shippedPlugin, 'skills'), { recursive: true });
      await writeFile(join(downloaded, 'dist/bin/videocut.mjs'), 'new runtime');
      await writeFile(
        join(downloaded, 'package.json'),
        JSON.stringify({ name: officialPackage, version: '0.2.8' })
      );
      await writeFile(
        join(shippedPlugin, '.codex-plugin/plugin.json'),
        JSON.stringify({ name: 'videocut-local', version: '0.2.8' })
      );
      await writeFile(join(shippedPlugin, 'skills/SKILL.md'), 'new skill');
      return '';
    }
  });
  assert.equal(await readFile(join(runtime, 'dist/bin/videocut.mjs'), 'utf8'), 'new runtime');
  assert.equal(await readFile(join(plugin, '.mcp.json'), 'utf8'), 'keep authorized roots');
  assert.equal(await readFile(join(runtime, 'native/bridge'), 'utf8'), 'keep native');
  assert.equal(JSON.parse(await readFile(join(runtime, 'package.json'))).version, '0.2.8');
});
