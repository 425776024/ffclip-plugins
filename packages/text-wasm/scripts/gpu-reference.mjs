import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { root, run } from './common.mjs';
if (process.platform !== 'darwin')
  throw new Error(
    'Metal reference generation requires macOS. Browser WebGPU rendering has no such dependency.'
  );
await run(process.execPath, ['scripts/gpu-fixtures.mjs']);
const binary = join(root, '.cache/metal-reference');
await run('xcrun', [
  'clang++',
  '-std=c++17',
  '-fobjc-arc',
  'tests/gpu/metal-reference.mm',
  '-framework',
  'Metal',
  '-framework',
  'Foundation',
  '-o',
  binary
]);
const shaders = 'vendor/videocut/sdk/skia_runtime/src/text/shaders/metal';
const dir = join(root, '.cache/gpu-reference');
await run(binary, [shaders, dir]);
const report = JSON.parse(await readFile(join(dir, 'reference.json')));
const digest = async (path) =>
  createHash('sha256')
    .update(await readFile(join(root, path)))
    .digest('hex');
report.shaderSha256 = {};
for (const name of ['MetalTextSdf.metal', 'MetalGaussian.metal', 'MetalQtTextSoftGlow.metal'])
  report.shaderSha256[name] = await digest(`${shaders}/${name}`);
report.wasmSha256 = await digest('dist/videocut-text.wasm');
report.inputSha256 = await digest('.cache/gpu-reference/input.rgba');
report.distanceMeshSha256 = await digest('.cache/gpu-reference/distance.bin');
report.shapeMeshSha256 = await digest('.cache/gpu-reference/shape.bin');
await writeFile(join(dir, 'reference.json'), JSON.stringify(report, null, 2) + '\n');
console.log('Metal fixtures ready. Open demo/gpu.html and run 对比 Metal 参考.');
