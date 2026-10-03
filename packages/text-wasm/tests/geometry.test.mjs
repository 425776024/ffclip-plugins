import test from 'node:test';
import assert from 'node:assert/strict';
import { createTextEngine } from '../dist/index.mjs';
import { fixture, registerFonts, fonts } from './helpers.mjs';

test('native control bounds stay stable while animation renders beyond the control box', async () => {
  const engine = await createTextEngine();
  const renderer = engine.createRenderer();
  try {
    registerFonts(renderer);
    await renderer.loadTemplate(
      await fixture('com.videocut.text.qt-type.anim-lua-letter-transform'),
      {
        bindings: { content: '测试' },
        allowRasterFallback: true,
        fallbackFonts: [{ id: fonts[1].id, family: 'Source Han Sans SC' }]
      }
    );
    const a = renderer.render({ timeUs: 350000, width: 640, height: 360 });
    const b = renderer.render({ timeUs: 1800000, width: 640, height: 360 });
    assert.deepEqual(a.controlBounds, b.controlBounds);
    const control = b.controlBounds;
    let outside = 0;
    for (let y = 0; y < b.height; y++)
      for (let x = 0; x < b.width; x++) {
        const px = x + b.originX,
          py = y + b.originY;
        if (
          b.data[(y * b.width + x) * 4 + 3] > 20 &&
          (px < control.x ||
            px >= control.x + control.width ||
            py < control.y ||
            py >= control.y + control.height)
        )
          outside++;
      }
    assert.ok(outside > 1000, `Expected animated pixels outside control box, got ${outside}`);
    const portrait = renderer.render({ timeUs: 1800000, width: 360, height: 640 });
    assert.ok(portrait.controlBounds.width > 0);
    assert.ok(
      portrait.originX + portrait.width <= 360 && portrait.originY + portrait.height <= 640
    );
  } finally {
    renderer.dispose();
    engine.dispose();
  }
});
