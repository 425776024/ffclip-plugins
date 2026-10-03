import {
  mkdir,
  mkdtemp,
  writeFile,
  readFile,
  access,
  lstat,
  rename,
  rm,
  chmod
} from 'node:fs/promises';
import { resolve, join, dirname, basename, relative, isAbsolute } from 'node:path';
import {
  root,
  pluginFiles,
  verifyDist,
  readRegular,
  listFiles,
  isMain,
  digest
} from './release-files.mjs';

async function exists(path) {
  return lstat(path).catch((error) => {
    if (error.code !== 'ENOENT') throw error;
  });
}

export async function buildPlugin({
  output = join(root, '.local/plugin-bundle/videocut-local'),
  roots = [],
  bridge
} = {}) {
  const pkg = JSON.parse(await readFile(join(root, 'package.json'), 'utf8'));
  output = resolve(output);
  if (basename(output) !== 'videocut-local')
    throw new Error('Plugin folder must be named videocut-local');
  for (const reserved of [join(root, 'dist'), join(root, 'plugins')]) {
    const rel = relative(reserved, output);
    if (rel === '' || (!rel.startsWith('..') && !isAbsolute(rel)))
      throw new Error('Plugin output must be separate from build inputs');
  }
  const paths = await verifyDist();
  const hashes = JSON.parse(await readFile(join(root, '.local/release-files.json'), 'utf8'));
  for (const path of roots) await access(resolve(path));
  let binary;
  if (bridge) {
    binary = await readRegular(resolve(bridge));
    const magic = binary.subarray(0, 4).toString('hex');
    if (
      ![
        '7f454c46',
        'feedface',
        'feedfacf',
        'cefaedfe',
        'cffaedfe',
        'cafebabe',
        'bebafeca',
        'cafebabf',
        'bfbafeca'
      ].includes(magic) &&
      binary.subarray(0, 2).toString('ascii') !== 'MZ'
    )
      throw new Error(
        '--native-bridge must be a compiled Mach-O, ELF or PE binary; source/scripts are forbidden'
      );
  }
  const prior = await exists(output);
  if (prior) {
    if (!prior.isDirectory()) throw new Error('Refusing to replace a non-directory plugin output');
    // Only replace a previous generated bundle, never an arbitrary user directory.
    const build = JSON.parse(await readRegular(join(output, 'BUILD.json')));
    const manifest = JSON.parse(await readRegular(join(output, '.codex-plugin/plugin.json')));
    const runtime = JSON.parse(await readRegular(join(output, 'runtime/package.json')));
    if (
      manifest.name !== 'videocut-local' ||
      !['videocut-local', pkg.name].includes(runtime.name) ||
      (build.builder !== 'videocut-local' && !(build.builtAt && build.node))
    )
      throw new Error(
        'Output is not a recognized generated VideoCut plugin; choose a new directory'
      );
  }
  await mkdir(dirname(output), { recursive: true });
  const stage = await mkdtemp(join(dirname(output), '.videocut-build-'));
  try {
    const expected = [];
    async function put(path, bytes, mode) {
      await mkdir(dirname(join(stage, path)), { recursive: true });
      await writeFile(join(stage, path), bytes);
      if (mode) await chmod(join(stage, path), mode);
      expected.push(path);
    }
    for (const path of pluginFiles.filter((path) => path !== '.mcp.json'))
      await put(path, await readRegular(join(root, 'plugins/videocut-local', path)));
    for (const path of paths) {
      const bytes = await readRegular(join(root, path));
      if (digest(bytes) !== hashes[path]) throw new Error(`Release changed during copy: ${path}`);
      await put(`runtime/${path}`, bytes, path.endsWith('/videocut.mjs') ? 0o755 : undefined);
    }
    const license = await readRegular(join(root, 'LICENSE'));
    await put('LICENSE', license);
    await put('runtime/LICENSE', license);
    const runtime = Object.fromEntries(
      ['name', 'version', 'description', 'type', 'license', 'engines', 'bin', 'exports'].map(
        (key) => [key, pkg[key]]
      )
    );
    await put('runtime/package.json', JSON.stringify(runtime, null, 2) + '\n');
    const args = ['runtime/dist/bin/videocut.mjs', '--mcp', '--port', '0'];
    for (const path of roots) args.push('--root', resolve(path));
    if (binary) {
      const path = `runtime/native/videocut-bridge${process.platform === 'win32' ? '.exe' : ''}`;
      await put(path, binary, 0o755);
      args.push('--native-bridge', path);
    }
    // Codex resolves a relative cwd against the installed plugin root.
    await put(
      '.mcp.json',
      JSON.stringify(
        {
          mcpServers: { videocut: { command: 'node', args, cwd: '.' } }
        },
        null,
        2
      ) + '\n'
    );
    await put(
      'BUILD.json',
      JSON.stringify(
        {
          builder: 'videocut-local',
          version: pkg.version,
          builtAt: new Date().toISOString(),
          includesNativeBridge: Boolean(binary)
        },
        null,
        2
      ) + '\n'
    );
    if (JSON.stringify((await listFiles(stage)).sort()) !== JSON.stringify(expected.sort()))
      throw new Error('Unexpected file in staged plugin');
    await verifyDist();
    const backup = `${stage}-previous`;
    if (prior) await rename(output, backup);
    try {
      await rename(stage, output);
    } catch (error) {
      if (prior) await rename(backup, output);
      throw error;
    }
    if (prior) await rm(backup, { recursive: true, force: true });
    return output;
  } finally {
    await rm(stage, { recursive: true, force: true });
  }
}

if (isMain(import.meta.url)) {
  const options = { roots: [] };
  const args = process.argv.slice(2);
  for (let i = 0; i < args.length; i += 2) {
    const [flag, value] = args.slice(i, i + 2);
    if (!value) throw new Error(`Missing value: ${flag}`);
    if (flag === '--output') options.output = value;
    else if (flag === '--root') options.roots.push(value);
    else if (flag === '--native-bridge') options.bridge = value;
    else throw new Error(`Unknown option: ${flag}`);
  }
  console.log(`Built plugin with compiled artifacts only: ${await buildPlugin(options)}`);
}
