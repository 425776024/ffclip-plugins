import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { loadTextTemplate } from '../dist/index.mjs';
export const catalog = JSON.parse(await readFile(new URL('../demo/catalog.json', import.meta.url), 'utf8'));
export const fonts = await Promise.all(catalog.fonts.map(async (font) => ({ ...font,
  bytes: await readFile(new URL(font.url, new URL('../demo/', import.meta.url))) })));
export async function fixture(id) {
  return loadTextTemplate(new URL(`../fixtures/templates/${id}/manifest.json`, import.meta.url), {
    fetch: async (url) => {
      try { return new Response(await readFile(fileURLToPath(url)), { status: 200 }); }
      catch { return new Response('', { status: 404 }); }
    }
  });
}
export function registerFonts(renderer) {
  for (const font of fonts) renderer.registerAsset(font.id, font.mediaType, font.bytes);
}
export function nontransparent(frame) {
  let count = 0;
  for (let i = 3; i < frame.data.length; i += 4) if (frame.data[i]) count++;
  return count;
}
