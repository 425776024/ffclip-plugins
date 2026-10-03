import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join, extname } from 'node:path';
const require = createRequire(import.meta.url);
const dist = dirname(require.resolve('@huggingface/transformers'));
// Use this Transformers release's own ORT runtime, independently of Kokoro's newer ORT.
export const asrAssetFiles = new Map([
  ...[
    'transformers.min.js',
    'ort-wasm-simd-threaded.jsep.mjs',
    'ort-wasm-simd-threaded.jsep.wasm'
  ].map((name) => [`asr-runtime/${name}`, join(dist, name)]),
  ['asr-runtime/licenses/Transformers-Apache-2.0.txt', join(dist, '../LICENSE')],
  [
    'asr-runtime/licenses/ONNXRuntime-MIT.txt',
    new URL('../packages/tts/vendor/ONNXRuntime-MIT.txt', import.meta.url).pathname
  ],
  ['asr-runtime/NOTICE.txt', new URL('../packages/asr/NOTICE.txt', import.meta.url).pathname]
]);
export function asrAssetBytes(name) {
  const bytes = readFileSync(asrAssetFiles.get(name));
  return ['.js', '.mjs'].includes(extname(name))
    ? Buffer.from(bytes.toString('utf8').replace(/\/\/[#@]\s*sourceMappingURL=.*$/gm, ''))
    : bytes;
}
export function asrRuntimeAssets() {
  return {
    name: 'videocut-asr-runtime-assets',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const name = (req.url || '').split('?')[0].replace(/^\//, '');
        if (!asrAssetFiles.has(name)) return next();
        res.setHeader(
          'Content-Type',
          name.endsWith('.wasm')
            ? 'application/wasm'
            : /\.(js|mjs)$/.test(name)
              ? 'text/javascript'
              : 'text/plain; charset=utf-8'
        );
        res.end(asrAssetBytes(name));
      });
    },
    generateBundle() {
      for (const name of asrAssetFiles.keys())
        this.emitFile({ type: 'asset', fileName: name, source: asrAssetBytes(name) });
    }
  };
}
