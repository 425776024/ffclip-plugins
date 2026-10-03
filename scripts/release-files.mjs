import { textAssetFiles } from './text-assets.mjs';
import { ttsAssetFiles } from './tts-assets.mjs';
import { asrAssetFiles } from './asr-assets.mjs';
import { createHash } from 'node:crypto';
import { lstat, readdir, readFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const root = fileURLToPath(new URL('../', import.meta.url));
export const pluginFiles = [
  '.codex-plugin/plugin.json',
  '.mcp.json',
  'skills/video-editing/SKILL.md',
  'skills/initialize-demo/SKILL.md',
  'skills/visual-understanding/SKILL.md',
  'skills/motion-templates/SKILL.md',
  'skills/motion-templates/references/html-clip.md',
  'skills/motion-templates/assets/gsap-lower-third.html'
];
export const runtimeFiles = [
  'dist/gsap-runtime.js',
  'dist/licenses/GSAP.txt',
  'dist/licenses/Mediabunny-MPL-2.0.txt',
  'dist/licenses/NOTICE.txt',
  'dist/bin/videocut.mjs',
  'dist/client/index.mjs',
  'dist/client/index.d.mts',
  'dist/core/project.mjs',
  'dist/core/project.d.mts',
  'dist/core/types.d.ts',
  'dist/core/commands.d.ts',
  'dist/core/model.d.mts',
  'dist/core/operations.d.mts',
  'dist/core/transaction.d.mts',
  'dist/core/history.d.mts',
  'dist/core/immutable.d.mts',
  'dist/client/types.d.ts',
  'dist/server/types.d.ts',
  'dist/server/index.mjs',
  'dist/server/index.d.mts',
  'dist/web/index.html',
  'dist/web/starter/starter.html',
  'dist/web/starter/landscape.jpg',
  'dist/web/starter/narration-zh.wav',
  'dist/web/starter/narration-en.wav',
  'dist/web/starter/captions-zh.json',
  'dist/web/starter/captions-en.json'
];
export const digest = (bytes) => createHash('sha256').update(bytes).digest('hex');

// Do not follow links: a link in an otherwise allowed directory can expose source files.
export async function listFiles(directory, prefix = '') {
  if (!(await lstat(directory)).isDirectory()) throw new Error(`Not a directory: ${directory}`);
  const files = [];
  for (const name of (await readdir(directory)).sort()) {
    const path = join(directory, name);
    const info = await lstat(path);
    if (info.isSymbolicLink()) throw new Error(`Release cannot contain a symlink: ${path}`);
    if (info.isDirectory()) files.push(...(await listFiles(path, `${prefix}${name}/`)));
    else if (info.isFile()) files.push(`${prefix}${name}`);
    else throw new Error(`Release cannot contain a special file: ${path}`);
  }
  return files;
}

export function assertArtifact(path, bytes) {
  if (path === 'dist/web/starter/landscape.jpg') {
    if (bytes[0] !== 0xff || bytes[1] !== 0xd8 || bytes.at(-2) !== 0xff || bytes.at(-1) !== 0xd9)
      throw new Error(`Invalid starter picture: ${path}`);
    return;
  }
  if (/^dist\/web\/starter\/narration-(zh|en)\.wav$/.test(path)) {
    if (bytes.toString('ascii', 0, 4) !== 'RIFF' || bytes.toString('ascii', 8, 12) !== 'WAVE')
      throw new Error(`Invalid starter narration: ${path}`);
    return;
  }
  if (/\.(?:ttf|otf|ttc|otc|woff2?)$/i.test(path))
    throw new Error(`Bundled fonts are forbidden; use system fonts: ${path}`);
  if (textAssetFiles.has(path.replace(/^dist\/web\//, '')) && path.startsWith('dist/web/')) return;
  if (
    (ttsAssetFiles.has(path.replace(/^dist\/web\//, '')) ||
      asrAssetFiles.has(path.replace(/^dist\/web\//, ''))) &&
    path.startsWith('dist/web/')
  ) {
    if (path.endsWith('.wasm')) {
      if (!WebAssembly.validate(bytes)) throw new Error(`Invalid speech WASM runtime: ${path}`);
    } else if (
      /(?:sourceMappingURL|sourceURL)\s*=|["']sourcesContent["']\s*:/.test(bytes.toString('utf8'))
    ) {
      throw new Error(`Source map or embedded source found: ${path}`);
    }
    return;
  }
  if (/^dist\/web\/assets\/videocut-text-[A-Za-z0-9_-]+\.wasm$/.test(path)) {
    if (!WebAssembly.validate(bytes)) throw new Error('Invalid text WASM runtime');
    return;
  }
  if (/^dist\/web\/assets\/html-poster-[A-Za-z0-9_-]+\.png$/.test(path)) {
    if (bytes.subarray(0, 8).toString('hex') !== '89504e470d0a1a0a')
      throw new Error(`Invalid animation poster: ${path}`);
    return;
  }
  if (!runtimeFiles.includes(path) && !/^dist\/web\/assets\/[A-Za-z0-9_-]+\.(?:js|css)$/.test(path))
    throw new Error(`Unexpected release file (source and maps are forbidden): ${path}`);
  const content = bytes.toString('utf8');
  if (/(?:sourceMappingURL|sourceURL)\s*=|["']sourcesContent["']\s*:/.test(content))
    throw new Error(`Source map or embedded source found: ${path}`);
}

export async function verifyDist(packageRoot = root) {
  const manifest = JSON.parse(
    await readFile(join(packageRoot, '.local/release-files.json'), 'utf8')
  );
  const paths = await listFiles(join(packageRoot, 'dist'), 'dist/');
  if (JSON.stringify(paths.sort()) !== JSON.stringify(Object.keys(manifest).sort()))
    throw new Error('Release files changed since build; rebuild before packaging');
  for (const required of runtimeFiles) {
    if (!paths.includes(required)) throw new Error(`Missing runtime file: ${required}`);
  }
  for (const path of paths) {
    const bytes = await readFile(join(packageRoot, path));
    assertArtifact(path, bytes);
    if (digest(bytes) !== manifest[path])
      throw new Error(`Release file modified after build: ${path}`);
  }
  return paths;
}

export async function readRegular(path) {
  if (!(await lstat(path)).isFile()) throw new Error(`Expected a regular file: ${path}`);
  return readFile(path);
}

export function isMain(url) {
  return process.argv[1] && fileURLToPath(url) === resolve(process.argv[1]);
}
