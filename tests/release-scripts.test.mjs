import test from 'node:test';
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdtemp, mkdir, readFile, writeFile, rm, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { authorizationUrls, runNpm } from '../scripts/npm-command.mjs';
import { nextVersion, publishNpm } from '../scripts/publish-npm.mjs';
import { zipDirectory } from '../scripts/build-website.mjs';

const exec = promisify(execFile);
async function temporary(t) {
  const path = await mkdtemp(join(tmpdir(), 'ffclip-scripts-'));
  t.after(() => rm(path, { recursive: true, force: true }));
  return path;
}

test('release version advances past local and published versions, rejecting overwrites and downgrades', () => {
  assert.equal(nextVersion('0.2.8', ['0.2.9', '0.3.0-beta.1']), '0.2.10');
  assert.equal(nextVersion('0.3.0', ['0.2.9'], 'minor'), '0.4.0');
  assert.equal(nextVersion('0.2.8', [], 'major'), '1.0.0');
  assert.equal(nextVersion('0.2.9', ['0.2.8'], '0.2.9'), '0.2.9');
  assert.throws(() => nextVersion('0.2.8', ['0.2.9'], '0.2.9'), /重复发布/);
  assert.throws(() => nextVersion('0.2.8', ['0.3.0'], '0.2.9'), /小于/);
  assert.throws(() => nextVersion('0.2.8', [], '--force'), /版本参数/);
});

test('only official HTTPS npm login and auth URLs qualify for automatic opening', () => {
  assert.deepEqual(
    authorizationUrls(
      `\x1b[32mhttps://www.npmjs.com/login?next=/login/cli/abc\x1b[0m\nhttps://npmjs.com/auth/abc\nhttps://www.npmjs.com/package/foo\nhttps://www.npmjs.com.evil.test/login/abc\nhttp://www.npmjs.com/login/abc`
    ),
    ['https://www.npmjs.com/login?next=/login/cli/abc', 'https://npmjs.com/auth/abc']
  );
});

test(
  'authorization runner provides a real PTY, opens split URLs once, and propagates exit status',
  {
    skip: !['darwin', 'linux'].includes(process.platform),
    timeout: 15000
  },
  async (t) => {
    const work = await temporary(t);
    const cli = join(work, 'fake-npm.mjs');
    await writeFile(
      cli,
      `
    import assert from 'node:assert/strict';
    assert.equal(process.stdin.isTTY, true);
    assert.equal(process.stdout.isTTY, true);
    assert.ok(process.argv.includes('--browser=false'));
    process.stdout.write('Authenticate your account at:\\nhttps://www.npmjs.com/log');
    setTimeout(() => {
      console.log('in?next=/login/cli/test');
      console.log('https://www.npmjs.com/login?next=/login/cli/test');
      process.exit(process.argv.includes('--fail') ? 7 : 0);
    }, 100);
  `
    );
    const opened = [];
    await runNpm(['login'], { cli, authorize: true, opener: async (url) => opened.push(url) });
    assert.deepEqual(opened, ['https://www.npmjs.com/login?next=/login/cli/test']);
    await assert.rejects(
      runNpm(['publish', '--fail'], { cli, authorize: true, opener: async () => {} }),
      (error) => error.code === 7
    );
  }
);

async function releaseFixture(t) {
  const packageRoot = await temporary(t);
  const plugin = join(packageRoot, 'plugins/videocut-local/.codex-plugin/plugin.json');
  await mkdir(join(packageRoot, 'plugins/videocut-local/.codex-plugin'), { recursive: true });
  const original = '{"name":"@ffclip-com/videocut","version":"0.2.8"}\n';
  await writeFile(join(packageRoot, 'package.json'), original);
  await writeFile(plugin, '{"name":"videocut-local","version":"0.2.8"}\n');
  await writeFile(
    join(packageRoot, 'package-lock.json'),
    '{"version":"0.2.8","packages":{"":{"version":"0.2.8"}}}\n'
  );
  const calls = [];
  const npm = async (args, options) => {
    calls.push({ args, options });
    const pkg = JSON.parse(await readFile(join(packageRoot, 'package.json'), 'utf8'));
    assert.equal(JSON.parse(await readFile(plugin, 'utf8')).version, pkg.version);
    const lock = JSON.parse(await readFile(join(packageRoot, 'package-lock.json'), 'utf8'));
    assert.equal(lock.version, pkg.version);
    assert.equal(lock.packages[''].version, pkg.version);
    if (args[0] === 'pack')
      return {
        stdout: JSON.stringify([
          {
            name: pkg.name,
            version: pkg.version,
            filename: 'package.tgz',
            entryCount: 10,
            size: 100
          }
        ])
      };
    return { stdout: 'test-account\n' };
  };
  return { packageRoot, npm, calls, original, versions: async () => ['0.2.8'] };
}

test('dry-run builds and packs once, restores metadata byte-for-byte, and never logs in or publishes', async (t) => {
  const fixture = await releaseFixture(t);
  await publishNpm(['--dry-run'], fixture);
  assert.deepEqual(
    fixture.calls.map(({ args }) => args.slice(0, 2)),
    [
      ['run', 'build'],
      ['run', 'check:package'],
      ['pack', '--ignore-scripts']
    ]
  );
  assert.equal(await readFile(join(fixture.packageRoot, 'package.json'), 'utf8'), fixture.original);
  assert.equal(
    JSON.parse(await readFile(join(fixture.packageRoot, 'package-lock.json'), 'utf8')).packages['']
      .version,
    '0.2.8'
  );
});

test('expired credentials invoke web login and publish the checked tarball with public latest access', async (t) => {
  const fixture = await releaseFixture(t);
  const npm = fixture.npm;
  fixture.npm = async (args, options) => {
    if (args[0] === 'whoami')
      throw Object.assign(new Error('expired'), { stderr: 'npm error E401' });
    return npm(args, options);
  };
  await publishNpm([], fixture);
  const login = fixture.calls.find(({ args }) => args[0] === 'login');
  assert.equal(login.options.authorize, true);
  assert.ok(login.args.includes('--auth-type=web'));
  const publish = fixture.calls.find(({ args }) => args[0] === 'publish');
  assert.equal(publish.args[1], join(fixture.packageRoot, '.local/npm-release/package.tgz'));
  for (const arg of [
    '--ignore-scripts',
    '--access=public',
    '--tag=latest',
    '--registry=https://registry.npmjs.org/'
  ])
    assert.ok(publish.args.includes(arg));
  assert.equal(publish.options.authorize, true);
  assert.equal(
    JSON.parse(await readFile(join(fixture.packageRoot, 'package.json'), 'utf8')).version,
    '0.2.9'
  );
});

test('build failures restore versions; an uncertain publish preserves the target for retry', async (t) => {
  const fixture = await releaseFixture(t);
  const npm = fixture.npm;
  fixture.npm = async () => {
    throw new Error('build failed');
  };
  await assert.rejects(publishNpm([], fixture), /build failed/);
  assert.equal(await readFile(join(fixture.packageRoot, 'package.json'), 'utf8'), fixture.original);
  fixture.npm = async (args, options) => {
    if (args[0] === 'publish') throw new Error('connection lost');
    return npm(args, options);
  };
  await assert.rejects(publishNpm([], fixture), /connection lost/);
  assert.equal(
    JSON.parse(await readFile(join(fixture.packageRoot, 'package.json'), 'utf8')).version,
    '0.2.9'
  );
});

test(
  'ZIP independently extracts Unicode and binary content with files at the website root',
  {
    skip: process.platform === 'win32'
  },
  async (t) => {
    const work = await temporary(t);
    const directory = join(work, 'dist');
    await mkdir(join(directory, 'assets'), { recursive: true });
    const binary = Buffer.from([0, 255, 17, 31, 0, 128]);
    await writeFile(join(directory, 'index.html'), '<html>官网</html>');
    await writeFile(join(directory, 'assets/中文.bin'), binary);
    const { bytes, count } = await zipDirectory(directory);
    assert.equal(count, 2);
    const archive = join(work, 'website.zip');
    await writeFile(archive, bytes);
    await exec('unzip', ['-t', archive]);
    const extracted = join(work, 'extracted');
    await exec('unzip', ['-q', archive, '-d', extracted], {
      env: process.platform === 'darwin' ? { ...process.env, LC_ALL: 'en_US.UTF-8' } : process.env
    });
    assert.equal(await readFile(join(extracted, 'index.html'), 'utf8'), '<html>官网</html>');
    assert.deepEqual(await readFile(join(extracted, 'assets/中文.bin')), binary);
    await symlink(join(directory, 'index.html'), join(directory, 'link.html'));
    await assert.rejects(zipDirectory(directory), /普通文件/);
  }
);
