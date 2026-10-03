import { readFile, realpath, access, mkdtemp, cp, rename, rm, writeFile } from 'node:fs/promises';
import { basename, dirname, join, resolve, delimiter } from 'node:path';
import { run } from './media.mjs';

export const officialPackage = '@ffclip-com/videocut';
const registry = 'https://registry.npmjs.org';
const checkInterval = 6 * 60 * 60 * 1000;
const retryInterval = 30 * 60 * 1000;

function versionParts(value) {
  if (typeof value !== 'string' || value.length > 128) return null;
  const match =
    /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$/.exec(
      value
    );
  if (!match) return null;
  const pre = match[4]?.split('.') || [];
  if (pre.some((part) => /^0\d+$/.test(part))) return null;
  return { numbers: match.slice(1, 4).map(BigInt), pre };
}

export function compareVersions(left, right) {
  const a = versionParts(left),
    b = versionParts(right);
  if (!a || !b) throw new Error('Invalid package version');
  for (let i = 0; i < 3; i++) {
    if (a.numbers[i] !== b.numbers[i]) return a.numbers[i] > b.numbers[i] ? 1 : -1;
  }
  if (!a.pre.length || !b.pre.length)
    return a.pre.length === b.pre.length ? 0 : a.pre.length ? -1 : 1;
  for (let i = 0; i < Math.max(a.pre.length, b.pre.length); i++) {
    const x = a.pre[i],
      y = b.pre[i];
    if (x === y) continue;
    if (x === undefined || y === undefined) return x === undefined ? -1 : 1;
    const xn = /^\d+$/.test(x),
      yn = /^\d+$/.test(y);
    if (xn && yn) return BigInt(x) > BigInt(y) ? 1 : -1;
    if (xn !== yn) return xn ? -1 : 1;
    return x > y ? 1 : -1;
  }
  return 0;
}

function installedPrefix(root) {
  const scope = dirname(root),
    modules = dirname(scope);
  return basename(root) === 'videocut' &&
    basename(scope) === '@ffclip-com' &&
    basename(modules) === 'node_modules'
    ? dirname(modules)
    : null;
}

async function bundledPlugin(root) {
  if (basename(root) !== 'runtime') return false;
  try {
    const metadata = JSON.parse(
      await readFile(join(dirname(root), '.codex-plugin/plugin.json'), 'utf8')
    );
    await access(join(root, 'dist/bin/videocut.mjs'));
    return metadata.name === 'videocut-local';
  } catch {
    return false;
  }
}

async function installBundledRuntime(packageRoot, npmCli, runCommand, signal) {
  const pluginRoot = dirname(packageRoot);
  const stage = await mkdtemp(join(pluginRoot, '.ffclip-update-'));
  const replaced = [];
  try {
    await runCommand(
      process.execPath,
      [
        npmCli,
        'install',
        '--prefix',
        stage,
        `${officialPackage}@latest`,
        `--registry=${registry}`,
        '--engine-strict',
        '--ignore-scripts',
        '--no-audit',
        '--no-fund'
      ],
      { signal, timeout: 5 * 60 * 1000, maxOutput: 1024 * 1024 }
    );
    signal?.throwIfAborted();
    const downloaded = join(stage, 'node_modules/@ffclip-com/videocut');
    const metadata = JSON.parse(await readFile(join(downloaded, 'package.json'), 'utf8'));
    if (metadata.name !== officialPackage || !versionParts(metadata.version))
      throw new Error('下载的更新版本信息无效');
    const shippedPlugin = join(downloaded, 'plugins/videocut-local');
    const targets = [
      [join(downloaded, 'dist'), join(packageRoot, 'dist')],
      [join(downloaded, 'package.json'), join(packageRoot, 'package.json')],
      [join(shippedPlugin, 'skills'), join(pluginRoot, 'skills')],
      [
        join(shippedPlugin, '.codex-plugin/plugin.json'),
        join(pluginRoot, '.codex-plugin/plugin.json')
      ]
    ];
    // Leave the user's MCP arguments, authorized folders and native bridge intact.
    for (const [source, destination] of targets) {
      const incoming = join(stage, `payload-${replaced.length}`);
      const backup = join(stage, `backup-${replaced.length}`);
      await cp(source, incoming, { recursive: true });
      await rename(destination, backup);
      replaced.push({ destination, backup });
      await rename(incoming, destination);
    }
    const buildPath = join(pluginRoot, 'BUILD.json');
    const build = await readFile(buildPath, 'utf8')
      .then(JSON.parse)
      .catch(() => null);
    if (build)
      await writeFile(
        buildPath,
        JSON.stringify(
          { ...build, version: metadata.version, builtAt: new Date().toISOString() },
          null,
          2
        ) + '\n'
      );
  } catch (error) {
    for (const { destination, backup } of replaced.reverse()) {
      await rm(destination, { recursive: true, force: true });
      await rename(backup, destination);
    }
    throw error;
  } finally {
    await rm(stage, { recursive: true, force: true });
  }
}

export async function findNpmCli() {
  const candidates = [
    join(dirname(process.execPath), 'node_modules/npm/bin/npm-cli.js'),
    join(dirname(process.execPath), '../lib/node_modules/npm/bin/npm-cli.js')
  ];
  if (process.env.npm_execpath?.endsWith('npm-cli.js'))
    candidates.unshift(process.env.npm_execpath);
  for (const directory of (process.env.PATH || '').split(delimiter)) {
    if (!directory) continue;
    for (const executable of ['npm', 'npm.cmd']) {
      const path = await realpath(join(directory, executable)).catch(() => null);
      if (!path) continue;
      if (path.endsWith('npm-cli.js')) candidates.push(path);
      candidates.push(join(dirname(path), 'node_modules/npm/bin/npm-cli.js'));
    }
  }
  for (const path of candidates) {
    if (
      await access(path).then(
        () => true,
        () => false
      )
    )
      return resolve(path);
  }
  throw new Error('未找到 npm，请先安装 Node.js 22+，或交给 AI 助手更新。');
}

/** Run npm through Node so paths with spaces and Windows .cmd wrappers need no shell. */
export async function installOfficialUpdate(
  packageRoot,
  { signal, npmCli, runCommand = run } = {}
) {
  packageRoot = await realpath(packageRoot);
  const prefix = installedPrefix(packageRoot);
  const bundle = !prefix && (await bundledPlugin(packageRoot));
  if (!prefix && !bundle) throw new Error('源码运行不支持自动覆盖，请交给 AI 助手安装官方版。');
  npmCli ||= await findNpmCli();
  if (bundle) return installBundledRuntime(packageRoot, npmCli, runCommand, signal);
  const globalRoot = (
    await runCommand(process.execPath, [npmCli, 'root', '--global'], { signal, timeout: 15000 })
  ).trim();
  const canonicalGlobal = await realpath(globalRoot).catch(() => resolve(globalRoot));
  const modules = dirname(dirname(packageRoot));
  const samePath = (a, b) =>
    process.platform === 'win32' ? a.toLowerCase() === b.toLowerCase() : a === b;
  const target = samePath(canonicalGlobal, modules)
    ? ['--global']
    : ['--prefix', prefix, '--no-save', '--package-lock=false'];
  await runCommand(
    process.execPath,
    [
      npmCli,
      'install',
      ...target,
      `${officialPackage}@latest`,
      `--registry=${registry}`,
      '--engine-strict',
      '--ignore-scripts',
      '--no-audit',
      '--no-fund'
    ],
    { signal, timeout: 5 * 60 * 1000, maxOutput: 1024 * 1024 }
  );
}

/** This service never installs until its authenticated POST route is called. */
export async function createSoftwareUpdates({
  packageRoot,
  fetchMetadata = fetch,
  installPackage = installOfficialUpdate,
  prepareInstall = async () => [],
  now = Date.now
}) {
  packageRoot = await realpath(packageRoot);
  const manifest = () => readFile(join(packageRoot, 'package.json'), 'utf8').then(JSON.parse);
  const initial = await manifest();
  const canInstall =
    initial.name === officialPackage &&
    (Boolean(installedPrefix(packageRoot)) || (await bundledPlugin(packageRoot)));
  /** @type {import('../client/types.js').SoftwareUpdateStatus} */
  let status = {
    packageName: officialPackage,
    currentVersion: initial.version,
    latestVersion: null,
    available: false,
    canInstall,
    state: 'idle',
    checkedAt: null,
    installedVersion: null,
    error: null,
    checkError: null,
    phase: null,
    savedProjects: []
  };
  let nextCheck = 0,
    pendingCheck,
    installing;
  const controller = new AbortController();
  const snapshot = () => ({ ...status });
  async function check(force = false) {
    if (installing || status.state === 'installed') return snapshot();
    if (pendingCheck) return pendingCheck;
    if (!force && now() < nextCheck) return snapshot();
    pendingCheck = (async () => {
      try {
        const response = await fetchMetadata(
          `${registry}/${encodeURIComponent(officialPackage)}/latest`,
          {
            signal: AbortSignal.any([controller.signal, AbortSignal.timeout(10000)]),
            headers: { Accept: 'application/json' },
            redirect: 'error'
          }
        );
        if (!response.ok) throw new Error(`npm HTTP ${response.status}`);
        const latest = await response.json();
        const version = versionParts(latest.version);
        if (latest.name !== officialPackage || !version || version.pre.length)
          throw new Error('npm 最新正式版本信息无效');
        status = {
          ...status,
          latestVersion: latest.version,
          available: compareVersions(latest.version, initial.version) > 0,
          checkedAt: new Date(now()).toISOString(),
          checkError: null
        };
        nextCheck = now() + checkInterval;
      } catch (error) {
        // An offline check cannot claim that the installed version is current.
        status = { ...status, checkError: String(error.message || error) };
        nextCheck = now() + retryInterval;
      }
      return snapshot();
    })().finally(() => {
      pendingCheck = undefined;
    });
    return pendingCheck;
  }
  async function install(assertReady = () => {}) {
    if (installing || status.state === 'installed') return snapshot();
    if (!canInstall)
      throw Object.assign(new Error('源码运行不支持自动覆盖，请交给 AI 助手安装官方版。'), {
        statusCode: 409
      });
    await check();
    // Two clients may have awaited the same registry request.
    if (installing || status.state === 'installed') return snapshot();
    if (status.checkError || !status.latestVersion)
      throw Object.assign(new Error('暂时无法检查最新版，请稍后重试。'), { statusCode: 503 });
    if (!status.available) return snapshot();
    assertReady();
    const expected = status.latestVersion;
    status = { ...status, state: 'installing', phase: 'saving', error: null };
    installing = (async () => {
      try {
        const savedProjects = await prepareInstall();
        status = { ...status, savedProjects, phase: 'installing' };
        await installPackage(packageRoot, { signal: controller.signal });
        const installed = await manifest();
        if (installed.name !== officialPackage || compareVersions(installed.version, expected) < 0)
          throw new Error('安装后版本校验未通过，请交给 AI 助手检查。');
        status = {
          ...status,
          state: 'installed',
          phase: null,
          installedVersion: installed.version
        };
      } catch (error) {
        status = {
          ...status,
          state: 'error',
          phase: null,
          error: String(error.message || error).slice(-4000)
        };
      }
    })().finally(() => {
      installing = undefined;
    });
    return snapshot();
  }
  return {
    check,
    install,
    status: snapshot,
    async close() {
      controller.abort();
      await Promise.allSettled([pendingCheck, installing]);
    }
  };
}
