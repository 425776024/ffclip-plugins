import { readFile, readdir } from 'node:fs/promises';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { root } from './common.mjs';
const expected = ['build-info.json', 'index.d.mts', 'recipes.mjs', 'recipes.d.mts', 'index.mjs', 'videocut-text.mjs', 'videocut-text.wasm', 'webgpu.mjs', 'gpu-shaders.mjs', 'webgpu.d.mts', 'browser-composition.mjs', 'browser-composition.d.mts', 'composition-gpu.mjs', 'composition-shaders.mjs'];
const actual = (await readdir(join(root, 'dist'))).sort();
if (JSON.stringify(actual) !== JSON.stringify(expected.sort())) throw new Error('Unexpected or missing distribution files');
const bytes = await readFile(join(root, 'dist/videocut-text.wasm'));
if (!WebAssembly.validate(bytes)) throw new Error('Invalid WASM binary');
const info = JSON.parse(await readFile(join(root, 'dist/build-info.json'), 'utf8'));
if (info.wasmSha256 !== createHash('sha256').update(bytes).digest('hex')) throw new Error('Stale WASM build manifest');
for (const name of expected.filter(name=>name.endsWith('.mjs'))) {
  const content = await readFile(join(root, 'dist', name), 'utf8');
  if (content.includes('/Users/') || /sourceMappingURL\s*=/.test(content)) throw new Error(`Build contains local paths or source maps: ${name}`);
}
console.log(`Verified standalone ${info.profile}: ${(bytes.length / 1024 / 1024).toFixed(2)} MiB WASM`);
