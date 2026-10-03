import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { createTextEngine } from '../dist/index.mjs';
import { recipes, loadRecipe } from '../demo/complex-recipes.mjs';
import { fonts, registerFonts } from './helpers.mjs';
import { composeRecipe } from '../dist/recipes.mjs';
const engine = await createTextEngine();
after(() => engine.dispose());
for (const recipe of recipes.filter((r) => r.backdrop))
  test(`${recipe.id}: native backdrop contributes pixels and its asset closure does not alias the flower`, async () => {
    const { bundle } = await loadRecipe(recipe, async (url) => new Response(await readFile(url)));
    assert.ok(bundle.assets.has('backdrop/assets/asset-000.png'));
    assert.ok(bundle.assets.has('assets/asset-000.png'));
    const hashes = [];
    for (const visible of [true, false]) {
      const r = engine.createRenderer();
      registerFonts(r);
      try {
        const composition = structuredClone(bundle.composition);
        // Keep geometry and native entity centering identical. Removing the
        // layer also moves glyphs and cannot prove that the backdrop was drawn.
        if (!visible)
          composition.appearance.backdrops.layers.forEach((layer) => { layer.transform.opacity = 0; });
        await r.loadTemplate(
          { ...bundle, composition },
          {
            bindings: { content: recipe.text },
            allowRasterFallback: true,
            fallbackFonts: [{ id: fonts[1].id, family: 'Source Han Sans SC' }]
          }
        );
        const f = r.render({ timeUs: 350000, width: 640, height: 360 });
        hashes.push(createHash('sha256').update(f.data).digest('hex'));
      } finally {
        r.dispose();
      }
    }
    assert.notEqual(hashes[0], hashes[1]);
  });

test('a custom flower carries native channel rules and independently samples backdrop and letter motion', async () => {
  const recipe = {
    id: 'skill-flower',
    base: 'flower-style-03',
    backdrop: 'bubble-tile',
    animation: 'anim-lua-letter-transform'
  };
  const fetcher = async (url) => new Response(await readFile(url));
  const geometry = new Map();
  const sample = async (selection, times, content = '心动', backdropOpacity = 1) => {
    const { bundle } = await loadRecipe(selection, fetcher);
    const channels = bundle.composition.rules.map((rule) => rule.channel);
    if (selection.backdrop) assert.ok(channels.includes('bubble'));
    if (selection.animation) {
      assert.ok(channels.includes('animations'));
      assert.ok(channels.includes('decorations'));
      assert.equal(channels.filter((channel) => channel === 'animations').length, 1);
    }
    assert.ok(channels.includes('glyph.fill'));
    if (selection.backdrop)
      bundle.composition.appearance.backdrops.layers.forEach((layer) => { layer.transform.opacity = backdropOpacity; });
    const r = engine.createRenderer();
    registerFonts(r);
    try {
      await r.loadTemplate(bundle, {
        bindings: { content },
        allowRasterFallback: true,
        fallbackFonts: [{ id: fonts[1].id, family: 'Source Han Sans SC' }]
      });
      return times.map((timeUs) => {
        const frame = r.render({ timeUs, width: 640, height: 360 });
        const key = JSON.stringify([selection, content, timeUs]);
        const bounds = {
          controlBounds: frame.controlBounds,
          width: frame.width, height: frame.height, originX: frame.originX, originY: frame.originY
        };
        if (backdropOpacity === 1) geometry.set(key, bounds);
        else assert.deepEqual(bounds, geometry.get(key), 'opacity regression must compare identical geometry');
        const data = new Uint8Array(640 * 360 * 4);
        for (let y = 0; y < frame.height; y++)
          data.set(frame.data.subarray(y * frame.rowBytes, (y + 1) * frame.rowBytes),
            ((frame.originY + y) * 640 + frame.originX) * 4);
        return createHash('sha256').update(data).digest('hex');
      });
    } finally { r.dispose(); }
  };
  const [first, later, backwards] = await sample(recipe, [300000, 800000, 300000]);
  assert.notEqual(first, later, 'selected native letter program must contribute animated pixels');
  assert.equal(first, backwards, 'absolute source time must rewind exactly');
  const [nine] = await sample({ ...recipe, backdrop: 'bubble-nine-slice' }, [300000]);
  const [noBackdrop] = await sample({ ...recipe, backdrop: undefined }, [300000]);
  const [noAnimation] = await sample({ ...recipe, animation: undefined }, [300000]);
  assert.notEqual(first, nine, 'changing the backdrop on one custom recipe must change pixels');
  assert.notEqual(first, noBackdrop, 'selected native backdrop must contribute pixels');
  assert.notEqual(first, noAnimation, 'selected native animation must contribute pixels');
  const [longFirst, longLater, longBackwards] = await sample(recipe, [300000, 800000, 300000], '花字创作');
  const [longNine] = await sample({ ...recipe, backdrop: 'bubble-nine-slice' }, [300000], '花字创作');
  const [longInvisibleBackdrop] = await sample(recipe, [300000], '花字创作', 0);
  assert.notEqual(longFirst, longLater, 'four-character native letter animation must contribute pixels');
  assert.equal(longFirst, longBackwards, 'four-character absolute source time must rewind exactly');
  assert.notEqual(longFirst, longNine, 'four-character recipe backdrop changes must contribute pixels');
  assert.notEqual(longFirst, longInvisibleBackdrop, 'four-character backdrop opacity must contribute pixels with identical bounds');
});

test('repeated channel selection replaces the selector authority rather than duplicating rules', async () => {
  const rule = { channel: 'bubble', mode: 'keep', content_slot_id: 'content', target_layer_id: '', semantic_role: '' };
  const sources = {
    'flower-style-03': { composition: { rules: [rule], appearance: {} }, animation: { animation: { execution_graph: { nodes: [] } } }, assets: new Map() },
    'bubble-tile': { composition: { rules: [{ ...rule, mode: 'replace' }], appearance: { backdrops: { layers: [] } }, resources: [] }, assets: new Map() }
  };
  const { bundle } = await composeRecipe({ base: 'flower-style-03', backdrop: 'bubble-tile' }, async (id) => structuredClone(sources[id]));
  assert.deepEqual(bundle.composition.rules, [{ ...rule, mode: 'replace' }]);
});

test('only the two supported flower backdrop adaptations use implicit native composition', async () => {
  const load = async () => ({
    composition: { rules: [], appearance: { backdrops: { layers: [] } }, resources: [] },
    animation: { animation: { execution_graph: { nodes: [{ id: 'authored-node' }] } } },
    assets: new Map()
  });
  for (const base of ['flower-style-03', 'flower-style-38', 'anim-lua-cube', 'anim-studio-time-transform-softglow']) {
    for (const backdrop of [undefined, 'bubble-nine-slice']) {
      if (backdrop && !base.startsWith('flower-style-')) {
        await assert.rejects(composeRecipe({ base, backdrop }, load), /flower-style-03.*flower-style-38/);
        continue;
      }
      const { bundle } = await composeRecipe({ base, backdrop }, load);
      const adapted = backdrop && base.startsWith('flower-style-');
      assert.equal(bundle.animation.animation.execution_graph.nodes.length, adapted ? 0 : 1);
    }
  }
});

test('implicit composition preserves the native four-character glyph material and animation pixels', async () => {
  const fullFrame = (frame) => {
    const data = new Uint8Array(640 * 360 * 4);
    for (let y = 0; y < frame.height; y++)
      data.set(frame.data.subarray(y * frame.rowBytes, (y + 1) * frame.rowBytes),
        ((frame.originY + y) * 640 + frame.originX) * 4);
    return data;
  };
  for (const base of ['flower-style-03', 'flower-style-38']) {
    const samples = [];
    for (const implicit of [false, true]) {
      const { bundle } = await loadRecipe({ base, animation: 'anim-lua-letter-transform' },
        async (url) => new Response(await readFile(url)));
      if (implicit) bundle.animation.animation.execution_graph.nodes = [];
      const r = engine.createRenderer();
      registerFonts(r);
      try {
        await r.loadTemplate(bundle, {
          bindings: { content: '花字创作' }, allowRasterFallback: true,
          fallbackFonts: [{ id: fonts[1].id, family: 'Source Han Sans SC' }]
        });
        samples.push([0, 300000, 800000].map((timeUs) => fullFrame(r.render({ timeUs, width: 640, height: 360 }))));
      } finally { r.dispose(); }
    }
    assert.deepEqual(samples[0], samples[1], `${base}: native glyph-only output must stay pixel-identical`);
  }
});
