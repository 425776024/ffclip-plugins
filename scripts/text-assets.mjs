import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join, extname } from 'node:path';
import { TEMPLATE_PARTS } from '../packages/text-wasm/src/recipes.mjs';

const root = fileURLToPath(new URL('../packages/text-wasm/', import.meta.url));
// An explicit resource closure, shared by dev serving, release output and packaging checks.
export const textAssetFiles = new Map();
const add = (url, path) => textAssetFiles.set(`text-templates/${url}`, join(root, path));
// Fonts are supplied by the user's operating system through the local server.
// The reference font fixtures are development inputs, never release assets.
for (const id of new Set(Object.values(TEMPLATE_PARTS).flat())) {
  const prefix = `templates/com.videocut.text.qt-type.${id}`;
  // Authored release packages replace the historical reference appearances.
  const source = 'presets';
  const manifest = JSON.parse(readFileSync(join(root, source, prefix, 'manifest.json'), 'utf8'));
  for (const path of ['manifest.json', ...manifest.files.map((f) => f.path)]) {
    if (!/^[A-Za-z0-9_./-]+$/.test(path) || path.split('/').some((p) => !p || p === '..'))
      throw new Error('Unsafe template resource path');
    add(`${prefix}/${path}`, `presets/${prefix}/${path}`);
  }
}
function licenses(dir = 'LICENSES') {
  for (const entry of readdirSync(join(root, dir), { withFileTypes: true })) {
    if (entry.isDirectory()) licenses(`${dir}/${entry.name}`);
    else if (entry.isFile()) add(`${dir}/${entry.name}`, `${dir}/${entry.name}`);
    else throw new Error('Template license must be a regular file');
  }
}
licenses();
add('NOTICE.md', 'NOTICE.md');
const types = {
  '.wasm': 'application/wasm',
  '.json': 'application/json',
  '.png': 'image/png',
  '.mp4': 'video/mp4',
  '.ttf': 'font/ttf',
  '.otf': 'font/otf'
};
export function textTemplateAssets() {
  return {
    name: 'videocut-text-template-assets',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const name = (req.url || '').split('?')[0].replace(/^\//, '');
        const path = textAssetFiles.get(name);
        if (!path) return next();
        res.setHeader('Content-Type', types[extname(path)] || 'text/plain');
        res.end(readFileSync(path));
      });
    },
    generateBundle() {
      for (const [fileName, path] of textAssetFiles)
        this.emitFile({ type: 'asset', fileName, source: readFileSync(path) });
    }
  };
}
