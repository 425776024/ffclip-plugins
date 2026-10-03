import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { root, isMain } from './release-files.mjs';
import { runNpm } from './npm-command.mjs';

const registry = 'https://registry.npmjs.org/';
const stable = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;

export function compareVersions(left, right) {
  const a = left.split('.').map(Number);
  const b = right.split('.').map(Number);
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] > b[i] ? 1 : -1;
  return 0;
}

export function nextVersion(current, published, requested = 'patch') {
  if (!stable.test(current)) throw new Error(`本地版本必须是正式版本：${current}`);
  const versions = published.filter((version) => stable.test(version));
  const baseline = [current, ...versions].sort(compareVersions).at(-1);
  if (['patch', 'minor', 'major'].includes(requested)) {
    const parts = baseline.split('.').map(Number);
    const index = { major: 0, minor: 1, patch: 2 }[requested];
    parts[index]++;
    for (let i = index + 1; i < 3; i++) parts[i] = 0;
    return parts.join('.');
  }
  if (!stable.test(requested)) throw new Error('版本参数仅支持 patch、minor、major 或 x.y.z。');
  if (published.includes(requested)) throw new Error(`npm 已存在 ${requested}，不能重复发布。`);
  if (compareVersions(requested, baseline) < 0)
    throw new Error(`指定版本 ${requested} 小于本地或 npm 的版本 ${baseline}。`);
  return requested;
}

export async function registryVersions(name) {
  const response = await fetch(`${registry}${encodeURIComponent(name)}`, {
    headers: { accept: 'application/vnd.npm.install-v1+json' },
    signal: AbortSignal.timeout(30000)
  });
  if (response.status === 404) return [];
  if (!response.ok) throw new Error(`读取 npm 版本失败：HTTP ${response.status}`);
  const metadata = await response.json();
  if (!metadata.versions || typeof metadata.versions !== 'object')
    throw new Error('npm 返回了无效的版本清单。');
  return Object.keys(metadata.versions);
}

export async function publishNpm(
  args = process.argv.slice(2),
  { packageRoot = root, npm = runNpm, versions = registryVersions } = {}
) {
  if (args.includes('--help')) {
    console.log(`用法：npm run release:npm -- [patch|minor|major|x.y.z] [--dry-run]
默认从本地与 npm 的最高正式版本递增 patch，同步 npm 和插件版本。
--dry-run 只构建、校验和生成 tgz，恢复本地版本，不登录、不发布。
真实发布时自动打开 npm 登录/二次授权页面；不创建 Git 提交或标签。`);
    return;
  }
  const dryRun = args.includes('--dry-run');
  const positional = args.filter((arg) => arg !== '--dry-run');
  if (positional.length > 1) throw new Error('只接受一个版本参数；使用 --help 查看用法。');
  const requested = positional[0] || 'patch';
  if (!['patch', 'minor', 'major'].includes(requested) && !stable.test(requested))
    throw new Error(`未知参数：${requested}；使用 --help 查看用法。`);

  const packagePath = join(packageRoot, 'package.json');
  const original = await readFile(packagePath, 'utf8');
  const pkg = JSON.parse(original);
  if (pkg.private) throw new Error('private 包不能发布。');
  console.log(`检查 ${pkg.name} 的 npm 版本…`);
  const version = nextVersion(pkg.version, await versions(pkg.name), requested);
  const backups = new Map([[packagePath, original]]);
  for (const relative of [
    'plugins/videocut-local/.codex-plugin/plugin.json',
    'package-lock.json',
    'npm-shrinkwrap.json'
  ]) {
    const path = join(packageRoot, relative);
    try {
      backups.set(path, await readFile(path, 'utf8'));
    } catch (error) {
      if (error.code !== 'ENOENT' || relative.includes('plugin.json')) throw error;
    }
  }
  const registryArgs = [`--registry=${registry}`];
  if (pkg.name.startsWith('@'))
    registryArgs.push(`--${pkg.name.split('/')[0]}:registry=${registry}`);
  let publishAttempted = false;
  let published = false;
  try {
    for (const [path, contents] of backups) {
      const metadata = JSON.parse(contents);
      metadata.version = version;
      if (metadata.packages?.['']) metadata.packages[''].version = version;
      await writeFile(path, JSON.stringify(metadata, null, 2) + '\n');
    }
    console.log(`\n${dryRun ? '预演' : '发布'}：${pkg.name}@${version}\n构建并校验发布文件…`);
    await npm(['run', 'build']);
    await npm(['run', 'check:package']);
    const output = join(packageRoot, '.local/npm-release');
    await mkdir(output, { recursive: true });
    const result = await npm(['pack', '--ignore-scripts', '--json', '--pack-destination', output], {
      capture: true
    });
    const [pack] = JSON.parse(result.stdout);
    if (pack.name !== pkg.name || pack.version !== version)
      throw new Error('打包结果与目标版本不一致。');
    const tarball = join(output, pack.filename);
    console.log(
      `\n安装包：${tarball}\n文件：${pack.entryCount}；大小：${(pack.size / 1024 / 1024).toFixed(2)} MB`
    );
    if (dryRun) {
      console.log('预演完成。本地版本会恢复；未执行登录或发布。');
      return;
    }
    try {
      const account = await npm(['whoami', ...registryArgs], { capture: true });
      console.log(`npm 账号：${account.stdout.trim()}`);
    } catch (error) {
      if (!/ENEEDAUTH|E401|E403/.test(error.stderr || '')) throw error;
      console.log('需要 npm 登录，请完成即将打开的网页授权。');
      await npm(['login', '--auth-type=web', ...registryArgs], { authorize: true });
    }
    // Publish the exact checked tarball, rather than rebuilding via prepack.
    publishAttempted = true;
    await npm(
      [
        'publish',
        tarball,
        '--ignore-scripts',
        '--access=public',
        '--tag=latest',
        '--auth-type=web',
        ...registryArgs
      ],
      { authorize: true }
    );
    published = true;
    console.log(
      `\n发布成功：${pkg.name}@${version}\nhttps://www.npmjs.com/package/${pkg.name}\n安装更新：npm install -g ${pkg.name}@latest`
    );
  } finally {
    if (dryRun || !publishAttempted) {
      for (const [path, contents] of backups) await writeFile(path, contents);
    } else if (!published) {
      console.error(
        `\n发布未确认成功，保留目标版本 ${version}。检查 npm 后可用 npm run release:npm -- ${version} 重试。`
      );
    }
  }
}

if (isMain(import.meta.url)) {
  publishNpm().catch((error) => {
    console.error(`\n发布失败：${error.message}`);
    if (error.stderr) console.error(error.stderr.trim());
    process.exitCode = 1;
  });
}
