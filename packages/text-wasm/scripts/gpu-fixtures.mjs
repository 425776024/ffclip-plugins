import { createTextEngine } from '../dist/index.mjs';
import { fixture, registerFonts, fonts } from '../tests/helpers.mjs';
import { mkdir, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { root } from './common.mjs';
const dir = join(root, '.cache/gpu-reference');
await mkdir(dir, { recursive: true });
const e = await createTextEngine();
const r = e.createRenderer();
try {
  registerFonts(r);
  await r.loadTemplate(await fixture('com.videocut.text.qt-type.flower-style-09'), {
    allowRasterFallback: true,
    bindings: { content: '花字' },
    fallbackFonts: [{ id: fonts[1].id, family: 'Source Han Sans SC' }]
  });
  const m = r.prepareSdfMesh({ width: 640, height: 360, range: 30 });
  for (const [name, data] of [
    ['distance.bin', m.distanceVertices],
    ['shape.bin', m.shapeVertices]
  ])
    await writeFile(join(dir, name), new Uint8Array(data.buffer));
  await writeFile(
    join(dir, 'mesh.json'),
    JSON.stringify({ ...m, distanceVertices: undefined, shapeVertices: undefined }, null, 2)
  );
  const f = r.render({ timeUs: 1500000, width: 640, height: 360 });
  const image = new Uint8Array(640 * 360 * 4);
  for (let y = 0; y < f.height; y++)
    for (let x = 0; x < f.width; x++) {
      const i = (y * f.width + x) * 4,
        o = ((y + f.originY) * 640 + x + f.originX) * 4;
      if (x + f.originX < 0 || x + f.originX >= 640 || y + f.originY < 0 || y + f.originY >= 360)
        continue;
      image[o] = Math.round((f.data[i] * f.data[i + 3]) / 255);
      image[o + 1] = Math.round((f.data[i + 1] * f.data[i + 3]) / 255);
      image[o + 2] = Math.round((f.data[i + 2] * f.data[i + 3]) / 255);
      image[o + 3] = f.data[i + 3];
    }
  await writeFile(join(dir, 'input.rgba'), image);
  console.log(
    JSON.stringify({
      glyphs: m.glyphCount,
      distanceVertices: m.distanceCount,
      shapeVertices: m.shapeCount,
      dir
    })
  );
} finally {
  e.dispose();
}
