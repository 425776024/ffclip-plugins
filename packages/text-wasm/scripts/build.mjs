import { readFile, writeFile, mkdir, copyFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import { root, toolsRoot, buildRoot, lock, run } from './common.mjs';
const skia = join(toolsRoot, 'skia');
const skiaBuild = join(toolsRoot, 'skia-wasm-simd');
const emsdk = join(toolsRoot, 'emsdk');
const emcmake = join(emsdk, 'upstream/emscripten/emcmake');
if (!existsSync(emcmake) || !existsSync(join(skia, 'bin/gn')))
  throw new Error('Build tools are missing. Run npm run setup first.');
const args = {
  target_cpu: 'wasm', is_debug: false, is_official_build: true, is_component_build: false,
  is_trivial_abi: true, is_canvaskit: true, skia_emsdk_dir: emsdk, werror: false,
  // Emscripten lowers Skia's four-lane SSE4.1 raster pipeline to WASM SIMD.
  // The ordinary wasm target otherwise selects the one-pixel scalar pipeline.
  extra_cflags: ['-msimd128', '-ffp-contract=off'],
  cc_wrapper: `python3 "${join(root, 'scripts/skia-compiler.py')}"`,
  skia_enable_ganesh: false, skia_enable_graphite: false, skia_use_webgl: false,
  skia_use_dawn: false, skia_use_vulkan: false, skia_use_metal: false, skia_use_gl: false,
  skia_use_fontconfig: false, skia_use_freetype: true,
  skia_enable_fontmgr_custom_directory: false, skia_enable_fontmgr_custom_embedded: true,
  skia_enable_fontmgr_custom_empty: true, skia_use_freetype_woff2: false,
  skia_use_icu: true, skia_use_system_icu: false, skia_use_harfbuzz: true,
  skia_use_system_harfbuzz: false, skia_use_system_freetype2: false,
  skia_use_system_libjpeg_turbo: false, skia_use_system_libpng: false,
  skia_use_system_libwebp: false, skia_use_system_zlib: false,
  skia_use_expat: true, skia_use_system_expat: false, skia_use_dng_sdk: false,
  skia_use_piex: false, skia_enable_skottie: true, skia_enable_svg: true,
  skia_enable_skparagraph: true, skia_enable_skshaper: true, skia_enable_pdf: false,
  skia_enable_tools: false, skia_use_partition_alloc: false
};
await mkdir(skiaBuild, { recursive: true });
await writeFile(join(skiaBuild, 'args.gn'), Object.entries(args).map(([k, v]) => `${k} = ${JSON.stringify(v)}`).join('\n') + '\n');
await run(join(skia, 'bin/gn'), ['gen', skiaBuild, `--root=${skia}`]);
// Ninja tracks the wrapper path, not its contents. Invalidate only the two
// translation units whose compiler arguments it changes when the wrapper changes.
const wrapperHash = createHash('sha256').update(await readFile(join(root, 'scripts/skia-compiler.py'))).digest('hex');
const wrapperStamp = join(skiaBuild, '.videocut-wrapper-sha256');
if (!existsSync(wrapperStamp) || (await readFile(wrapperStamp, 'utf8')) !== wrapperHash) {
  await run('ninja', ['-C', skiaBuild, '-t', 'clean', 'obj/src/core/core.SkOpts.o', 'obj/src/core/core.SkBlurEngine.o']);
}
await run('ninja', ['-C', skiaBuild, '-j8', 'skia', 'skparagraph', 'skshaper', 'skunicode', 'skottie', 'skresources', 'svg']);
await writeFile(wrapperStamp, wrapperHash);
await run(emcmake, ['cmake', '-S', root, '-B', buildRoot, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', `-DSKIA_ROOT=${skia}`, `-DSKIA_BUILD=${skiaBuild}`]);
await run('cmake', ['--build', buildRoot, '-j8']);
await mkdir(join(root, 'dist'), { recursive: true });
for (const name of ['preset-designs.mjs', 'recipes.mjs', 'recipes.d.mts', 'index.mjs', 'index.d.mts', 'webgpu.mjs', 'gpu-shaders.mjs', 'webgpu.d.mts', 'browser-composition.mjs', 'browser-composition.d.mts', 'composition-gpu.mjs', 'composition-shaders.mjs']) await copyFile(join(root, 'src', name), join(root, 'dist', name));
const bytes = await readFile(join(root, 'dist/videocut-text.wasm'));
const source = JSON.parse(await readFile(join(root, 'vendor/SOURCE.json'), 'utf8'));
await writeFile(join(root, 'dist/build-info.json'), JSON.stringify({ profile: 'wasm-raster-v1', ...lock,
  wasmFeatures: ['simd128'], rasterPipeline: 'four-lane-sse41-via-emscripten', blurPipeline: 'skvx-wasm-simd', compilerWrapperSha256: wrapperHash,
  sourceCommit: source.sourceCommit, wasmBytes: bytes.length,
  wasmSha256: createHash('sha256').update(bytes).digest('hex') }, null, 2) + '\n');
await run(process.execPath, ['scripts/check-dist.mjs']);
