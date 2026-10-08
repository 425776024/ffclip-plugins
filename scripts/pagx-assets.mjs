import { readFileSync, existsSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
const require = createRequire(import.meta.url);
export const pagxPackageRoot = dirname(require.resolve('@libpag/pagx/package.json'));
const nativeName = process.platform === 'win32' ? 'pagx.exe' : 'pagx';
const nativePath = new URL('../.local/pagx/build/' + nativeName, import.meta.url).pathname;
export const pagxNativeFiles = new Map(
  existsSync(nativePath)
    ? [[`dist/pagx-cli/${process.platform}-${process.arch}/${nativeName}`, nativePath]]
    : []
);
export const pagxAssetFiles = new Map([
  [
    'pagx-runtime/pagx-viewer.js',
    join(pagxPackageRoot, 'preview/wasm/viewer/pagx-viewer.st.esm.js')
  ],
  [
    'pagx-runtime/pagx-viewer.st.wasm',
    join(pagxPackageRoot, 'preview/wasm/viewer/pagx-viewer.st.wasm')
  ],
  ['pagx-runtime/NOTICE.txt', new URL('../packages/pagx/NOTICE.txt', import.meta.url).pathname],
  [
    'pagx-runtime/LICENSE.txt',
    new URL('../licenses/libpag-Apache-2.0.txt', import.meta.url).pathname
  ]
]);
export function pagxAssetBytes(name) {
  let bytes = readFileSync(pagxAssetFiles.get(name));
  if (name.endsWith('.js')) {
    const needle = 'var domElement = specialHTMLTargets[target] ||';
    let source = bytes.toString('utf8');
    if (source.split(needle).length !== 2)
      throw new Error('PAGX 0.4.47 canvas binding changed; review worker adapter');
    // Provide OffscreenCanvas directly to Emscripten, without a document/window shim.
    source = source.replace(
      needle,
      'var domElement = Module["videocutCanvases"]?.get(target) || specialHTMLTargets[target] ||'
    );
    const contextNeedle = 'createContext: (canvas, webGLContextAttributes) => {';
    if (source.split(contextNeedle).length !== 2)
      throw new Error('PAGX WebGL context binding changed; review worker adapter');
    // Partial rendering reuses unchanged pixels. OffscreenCanvas presentation may
    // otherwise discard them between absolute seeks and produce missing artwork.
    source = source.replace(
      contextNeedle,
      contextNeedle + '\n    webGLContextAttributes.preserveDrawingBuffer = true;'
    );
    bytes = Buffer.from(source.replace(/\/\/[#@]\s*sourceMappingURL=.*$/gm, ''));
  }
  return bytes;
}
export function pagxRuntimeAssets() {
  return {
    name: 'videocut-pagx-runtime',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const name = (req.url || '').split('?')[0].replace(/^\//, '');
        if (!pagxAssetFiles.has(name)) return next();
        res.setHeader(
          'Content-Type',
          name.endsWith('.wasm')
            ? 'application/wasm'
            : name.endsWith('.js')
              ? 'text/javascript'
              : 'text/plain'
        );
        res.end(pagxAssetBytes(name));
      });
    },
    generateBundle() {
      for (const name of pagxAssetFiles.keys())
        this.emitFile({ type: 'asset', fileName: name, source: pagxAssetBytes(name) });
    }
  };
}
