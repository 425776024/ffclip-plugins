import { mkdir, readFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { join } from 'node:path';
import { toolsRoot, lock, run } from './common.mjs';
await mkdir(toolsRoot, { recursive: true });
async function checkout(path, url, revision) {
  if (existsSync(join(path, '.git'))) {
    const actual = execFileSync('git', ['-C', path, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
    if (actual !== revision) throw new Error(`Dependency revision mismatch at ${path}: expected ${revision}, found ${actual}`);
    return;
  }
  if (existsSync(path)) throw new Error(`Refusing to replace existing dependency: ${path}`);
  await run('git', ['init', path]);
  await run('git', ['-C', path, 'remote', 'add', 'origin', url]);
  await run('git', ['-C', path, 'fetch', '--depth', '1', 'origin', revision]);
  await run('git', ['-C', path, 'checkout', '--detach', 'FETCH_HEAD']);
}
const emsdk = join(toolsRoot, 'emsdk');
await checkout(emsdk, 'https://github.com/emscripten-core/emsdk.git', lock.emsdk);
await run(join(emsdk, 'emsdk'), ['install', lock.emscripten]);
await run(join(emsdk, 'emsdk'), ['activate', lock.emscripten]);
const skia = join(toolsRoot, 'skia');
await checkout(skia, 'https://skia.googlesource.com/skia.git', lock.skia);
const deps = await readFile(join(skia, 'DEPS'), 'utf8');
const needed = new Set(['expat', 'freetype', 'harfbuzz', 'icu', 'libjpeg-turbo', 'libpng', 'libwebp', 'wuffs', 'zlib']);
for (const match of deps.matchAll(/["'](third_party\/externals\/([^"']+))["']\s*:\s*["']([^"']+)@([a-f0-9]{40})["']/g)) {
  if (needed.delete(match[2])) await checkout(join(skia, match[1]), match[3], match[4]);
}
if (needed.size) throw new Error(`Missing pinned dependencies: ${[...needed].join(', ')}`);
await run('python3', [join(skia, 'bin/fetch-gn')], { cwd: skia });
