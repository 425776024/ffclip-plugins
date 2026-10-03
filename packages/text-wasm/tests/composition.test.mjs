import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createTextEngine } from '../dist/index.mjs';
import { fixture, registerFonts, fonts } from './helpers.mjs';
const engine = await createTextEngine();
after(() => engine.dispose());
const options = {
  allowRasterFallback: true,
  experimentalComposition: true,
  bindings: { content: '绽放' },
  fallbackFonts: [{ id: fonts[1].id, family: 'Source Han Sans SC' }]
};
const digest = (f) => createHash('sha256').update(f.data).digest('hex');
for (const id of ['anim-studio-time-transform-softglow', 'anim-studio-dual-selector-radial'])
  test(`native composition plan: ${id} preserves clocks, layers and editable geometry`, async () => {
    const r = engine.createRenderer();
    registerFonts(r);
    try {
      const info = await r.loadTemplate(await fixture('com.videocut.text.qt-type.' + id), options);
      assert.equal(info.requiresBrowserComposition, true);
      assert.throws(() => r.draw({ width: 640, height: 360 }), /createBrowserTextComposition/);
      const a = r.render({ timeUs: 350000, width: 640, height: 360 }),
        b = r.render({ timeUs: 1800000, width: 640, height: 360 });
      const plan = b.compositionPlan;
      assert.equal(plan.decorations.length, 1);
      assert.equal(plan.effects.length, 1);
      assert.ok(plan.decorations[0].affine.every(Number.isFinite));
      assert.ok(plan.decorations[0].opacity > 0);
      assert.notEqual(a.compositionPlan.decorations[0].timeUs, plan.decorations[0].timeUs);
      assert.notEqual(digest(a), digest(b));
      assert.notDeepEqual(a.compositionPlan.effects, plan.effects);
      const again = r.render({ timeUs: 1800000, width: 640, height: 360 });
      assert.equal(digest(again), digest(b));
      assert.deepEqual(again.compositionPlan, plan);
      r.setText('动画文字');
      const edited = r.render({ timeUs: 1800000, width: 640, height: 360 });
      assert.notEqual(digest(edited), digest(b));
      assert.notDeepEqual(edited.compositionPlan.decorations[0].affine, plan.decorations[0].affine);
    } finally {
      r.dispose();
    }
  });
test('browser composition rejects unported template graphs, default CPU path still rejects decorations', async () => {
  for (const [id, experimentalComposition] of [
    ['anim-studio-scale-deepglow', true],
    ['anim-studio-time-transform-softglow', false]
  ]) {
    const r = engine.createRenderer();
    registerFonts(r);
    try {
      await assert.rejects(
        r.loadTemplate(await fixture('com.videocut.text.qt-type.' + id), {
          ...options,
          experimentalComposition
        }),
        /only|not supported/
      );
    } finally {
      r.dispose();
    }
  }
});
