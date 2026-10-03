import { build } from 'esbuild';
import { mkdir, writeFile } from 'node:fs/promises';
import { HtmlFrameRenderer } from '../packages/server/html-renderer.mjs';
import { ticks } from '../packages/core/project.mjs';

// Posters are actual template frames at a small viewport, with the authored stage scaled intact.
const bundled = await build({
  entryPoints: ['src/editor/html-presentation-presets.ts'],
  bundle: true,
  platform: 'node',
  format: 'esm',
  write: false,
  logLevel: 'silent'
});
const { PRESENTATION_PRESETS } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundled.outputFiles[0].text).toString('base64')
);
const overlayBundle = await build({
  entryPoints: ['src/editor/html-overlay-presets.ts'],
  bundle: true,
  platform: 'node',
  format: 'esm',
  write: false,
  logLevel: 'silent'
});
const { OVERLAY_PRESETS } = await import(
  'data:text/javascript;base64,' + Buffer.from(overlayBundle.outputFiles[0].text).toString('base64')
);
const renderer = new HtmlFrameRenderer();
await mkdir('src/editor/assets/motion', { recursive: true });
try {
  for (const preset of [...PRESENTATION_PRESETS, ...OVERLAY_PRESETS]) {
    const { png } = await renderer.capture({ ...preset.html, width: 640, height: 360 }, ticks(3));
    await writeFile(`src/editor/assets/motion/html-poster-${preset.id}.png`, png);
    console.log(`${preset.id}: ${Math.round(png.length / 1024)} KiB`);
  }
} finally {
  await renderer.close();
}
