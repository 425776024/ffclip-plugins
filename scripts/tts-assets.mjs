import { readFileSync, realpathSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join, extname } from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const root = fileURLToPath(new URL('../', import.meta.url));
const ortDist = dirname(require.resolve('onnxruntime-web'));
// The JSPI/JSEP/WebGL/Node builds are deliberately outside this exact asset closure.
const ortFiles = [
  'ort.webgpu.min.mjs', 'ort.wasm.min.mjs',
  'ort-wasm-simd-threaded.asyncify.mjs', 'ort-wasm-simd-threaded.asyncify.wasm',
  'ort-wasm-simd-threaded.mjs', 'ort-wasm-simd-threaded.wasm'
];
export const ttsAssetFiles = new Map(ortFiles.map((name) => [`tts-runtime/${name}`, join(ortDist, name)]));
const packageDirectory = (name) => dirname(realpathSync(join(root, 'node_modules', name, 'package.json')));
ttsAssetFiles.set('tts-runtime/licenses/Kokoro-JS-Apache-2.0.txt', join(root, 'packages/tts/vendor/Kokoro-JS-Apache-2.0.txt'));
ttsAssetFiles.set('tts-runtime/licenses/HeadTTS-MIT.txt', join(root, 'packages/tts/vendor/HeadTTS-MIT.txt'));
ttsAssetFiles.set('tts-runtime/licenses/Misaki-Apache-2.0.txt', join(root, 'packages/tts/vendor/Misaki-Apache-2.0.txt'));
ttsAssetFiles.set('tts-runtime/ENGLISH-SOURCE.json', join(root, 'packages/tts/vendor/ENGLISH-SOURCE.json'));
ttsAssetFiles.set('tts-runtime/licenses/Pinyin-Pro-MIT.txt', join(packageDirectory('pinyin-pro'), 'LICENSE'));
ttsAssetFiles.set('tts-runtime/licenses/ONNXRuntime-MIT.txt', join(root, 'packages/tts/vendor/ONNXRuntime-MIT.txt'));
ttsAssetFiles.set('tts-runtime/NOTICE.txt', join(root, 'packages/tts/NOTICE.txt'));
ttsAssetFiles.set('tts-runtime/DISTRIBUTION.txt', join(root, 'packages/tts/DISTRIBUTION.txt'));
export function ttsAssetBytes(name) {
  const bytes = readFileSync(ttsAssetFiles.get(name));
  return extname(name) === '.mjs' ? Buffer.from(bytes.toString('utf8').replace(/\/\/[#@]\s*sourceMappingURL=.*$/gm, '')) : bytes;
}
export function ttsRuntimeAssets() {
  return {
    name: 'videocut-tts-runtime-assets',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const name = (req.url || '').split('?')[0].replace(/^\//, '');
        if (!ttsAssetFiles.has(name)) return next();
        res.setHeader('Content-Type', name.endsWith('.wasm') ? 'application/wasm' : name.endsWith('.mjs') ? 'text/javascript' : 'text/plain; charset=utf-8');
        res.end(ttsAssetBytes(name));
      });
    },
    generateBundle() {
      for (const name of ttsAssetFiles.keys()) this.emitFile({ type: 'asset', fileName: name, source: ttsAssetBytes(name) });
    }
  };
}
