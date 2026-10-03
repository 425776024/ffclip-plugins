import { build as bundle } from 'esbuild';
import { build as buildWeb } from 'vite';
import { chmod, copyFile, cp, lstat, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { root, listFiles, assertArtifact, digest, verifyDist } from './release-files.mjs';
import { buildTypes } from './build-types.mjs';
import { ttsAssetFiles } from './tts-assets.mjs';
import { asrAssetFiles } from './asr-assets.mjs';

const dist = join(root, 'dist');
const existing = await lstat(dist).catch((error) => {
  if (error.code !== 'ENOENT') throw error;
});
if (existing && !existing.isDirectory())
  throw new Error('Refusing to replace a non-directory dist');
// dist is exclusively generated output. Remove old artifacts before either build runs.
await rm(dist, { recursive: true, force: true });
await buildWeb({ root });
await cp(join(root, 'packages/examples/assets'), join(dist, 'web/starter'), { recursive: true });
await bundle({
  absWorkingDir: root,
  entryPoints: {
    'bin/videocut': 'bin/videocut.mjs',
    'client/index': 'packages/client/index.mjs',
    'core/project': 'packages/core/project.mjs',
    'server/index': 'packages/server/index.mjs'
  },
  outdir: dist,
  outExtension: { '.js': '.mjs' },
  bundle: true,
  platform: 'node',
  format: 'esm',
  target: 'node22',
  minify: true,
  sourcemap: false,
  sourcesContent: false,
  legalComments: 'none',
  logLevel: 'info'
});
await buildTypes(dist);
await chmod(join(dist, 'bin/videocut.mjs'), 0o755);
await mkdir(join(dist, 'licenses'), { recursive: true });
await copyFile(join(root, 'node_modules/gsap/dist/gsap.min.js'), join(dist, 'gsap-runtime.js'));
// The installed browser supplies Chromium. Only the offline animation library is shipped.
const gsapRuntime = await readFile(join(dist, 'gsap-runtime.js'), 'utf8');
await writeFile(
  join(dist, 'gsap-runtime.js'),
  gsapRuntime.replace(/\/\/[#@]\s*sourceMappingURL=.*$/gm, '')
);
await writeFile(
  join(dist, 'licenses/GSAP.txt'),
  'GSAP 3.15.0. Copyright GreenSock / Webflow. Standard GSAP license: https://gsap.com/standard-license/ . Distributed unmodified from https://registry.npmjs.org/gsap/-/gsap-3.15.0.tgz except removal of the source-map pointer. The copyright/license header remains in gsap-runtime.js.\n'
);
await copyFile(
  join(root, 'node_modules/mediabunny/LICENSE'),
  join(dist, 'licenses/Mediabunny-MPL-2.0.txt')
);
await writeFile(
  join(dist, 'licenses/NOTICE.txt'),
  'Mediabunny 1.61.0 (MPL-2.0) is included unmodified in browser/media bundles. Source: https://github.com/Vanilagy/mediabunny/tree/v1.61.0 . Source archive: https://registry.npmjs.org/mediabunny/-/mediabunny-1.61.0.tgz . No Mediabunny FFmpeg extensions are included.\n'
);
await writeFile(
  join(dist, 'licenses/NOTICE.txt'),
  (await readFile(join(dist, 'licenses/NOTICE.txt'), 'utf8')) +
    '\nBrowser speech runtime notices and licenses: dist/web/tts-runtime/NOTICE.txt and dist/web/tts-runtime/licenses/. Model and voice files are installed separately and are not shipped.\n'
);
for (const name of [...ttsAssetFiles.keys(), ...asrAssetFiles.keys()]) {
  if (!(await lstat(join(dist, 'web', name))).isFile())
    throw new Error(`Missing speech runtime asset: ${name}`);
}
const manifest = {};
for (const path of await listFiles(dist, 'dist/')) {
  const bytes = await readFile(join(root, path));
  assertArtifact(path, bytes);
  manifest[path] = digest(bytes);
}
// Build evidence stays in the private workspace and never enters either release.
await mkdir(join(root, '.local'), { recursive: true });
await writeFile(join(root, '.local/release-files.json'), JSON.stringify(manifest, null, 2) + '\n');
await verifyDist();
