// Build the open-source animation importer. No enterprise SDK or authorization key.
import { spawn } from 'node:child_process';
import { access, mkdir, writeFile, rename } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { cpus } from 'node:os';
const revision = 'ec175d317dc70d171beed2d34af0eaef205d7710';
const base = resolve('.local/pagx'),
  source = join(base, 'source'),
  build = join(base, 'build');
const exists = (p) =>
  access(p).then(
    () => true,
    () => false
  );
async function run(command, args, cwd = source) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, {
      cwd,
      stdio: 'inherit',
      env: { ...process.env, GIT_LFS_SKIP_SMUDGE: '1' }
    });
    child.on('error', reject);
    child.on('exit', (code) =>
      code === 0 ? resolve() : reject(Error(`${command} exited ${code}`))
    );
  });
}
await mkdir(base, { recursive: true });
if (!(await exists(join(source, '.git')))) {
  await run(
    'git',
    [
      'clone',
      '--depth=1',
      '--filter=blob:none',
      '--sparse',
      'https://github.com/Tencent/libpag.git',
      source
    ],
    base
  );
  await run('git', ['fetch', '--depth=1', 'origin', revision]);
  await run('git', ['checkout', '--detach', revision]);
  await run('git', [
    'sparse-checkout',
    'set',
    'include',
    'src',
    'cli',
    'spec',
    'linux',
    'mac',
    'win',
    'vendor',
    'tgfx'
  ]);
}
const { execFileSync } = await import('node:child_process');
if (
  execFileSync('git', ['rev-parse', 'HEAD'], { cwd: source, encoding: 'utf8' }).trim() !== revision
)
  throw Error(
    'Existing PAGX checkout differs from the pinned revision; use a separate VIDEOCUT_PAGX_CLI build'
  );
// Prevent this workspace's type:module from changing upstream extensionless Node tools.
await writeFile(join(source, 'package.json'), '{"private":true,"type":"commonjs"}\n');
if (!(await exists(join(source, '.videocut-dependencies-ready')))) {
  // Sparse source builds do not need the root repository's large demo-media LFS set.
  // depsync otherwise tries to prune unavailable blobs in this shallow checkout.
  const attrs = join(source, '.gitattributes'),
    saved = attrs + '.videocut-build';
  const hadAttrs = await exists(attrs);
  if (hadAttrs) await rename(attrs, saved);
  try {
    await run(process.platform === 'win32' ? 'npx.cmd' : 'npx', ['--yes', 'depsync@1.3.12']);
  } finally {
    if (hadAttrs) await rename(saved, attrs);
  }
  await writeFile(join(source, '.videocut-dependencies-ready'), revision + '\n');
}
await run('cmake', [
  '-S',
  source,
  '-B',
  build,
  '-G',
  'Ninja',
  '-DCMAKE_BUILD_TYPE=Release',
  '-DPAG_BUILD_CLI=ON',
  '-DPAG_USE_LIBAVC=OFF',
  '-DPAG_BUILD_SHARED=OFF',
  '-DPAG_BUILD_FRAMEWORK=OFF',
  '-DPAG_BUILD_TESTS=OFF'
]);
await run('cmake', [
  '--build',
  build,
  '--target',
  'pagx-cli',
  '-j',
  String(Math.min(8, cpus().length))
]);
await writeFile(
  join(base, 'build-info.json'),
  JSON.stringify({ revision, platform: process.platform, arch: process.arch, enterprise: false }) +
    '\n'
);
console.log(
  'PAGX importer ready: ' + join(build, process.platform === 'win32' ? 'pagx.exe' : 'pagx')
);
