import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import { recipes, composeRecipe } from '../packages/text-wasm/src/recipes.mjs';
import { createTextEngine, loadTextTemplate } from '../packages/text-wasm/dist/index.mjs';
import { registerFonts, nontransparent, fixture } from '../packages/text-wasm/tests/helpers.mjs';
import { english } from '../src/editor/i18n/messages.mjs';
import { textAssetFiles } from '../scripts/text-assets.mjs';
import { presetDesigns } from '../packages/text-wasm/src/preset-designs.mjs';
import { rewriteTemplateFonts } from '../packages/text-wasm/src/system-fonts.mjs';
// Exercise actual Han glyphs through the same font rewrite as the application.
const testFonts = {
  defaults: {
    sans: 'builtin.font.inter.variable.v1',
    cjk: 'builtin.font.reference-cjk.source-han-sans-sc-medium.v1'
  },
  fonts: [
    { id: 'builtin.font.inter.variable.v1', family: 'Inter', postscriptName: 'Inter-Regular' },
    {
      id: 'builtin.font.reference-cjk.source-han-sans-sc-medium.v1',
      family: 'Source Han Sans SC',
      postscriptName: 'SourceHanSansSC-Medium'
    }
  ]
};
const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');
const load = (id) =>
  loadTextTemplate(
    `http://localhost/text-templates/templates/com.videocut.text.qt-type.${id}/manifest.json`,
    {
      fetch: async (url) => {
        const path = new URL(url).pathname.slice(1);
        assert.ok(textAssetFiles.has(path), path);
        return new Response(await readFile(join('dist/web', path)));
      }
    }
  );

test('authored library has exactly ten per category, translated labels and compatible legacy IDs', () => {
  assert.equal(new Set(recipes.map((r) => r.id)).size, 30);
  for (const category of ['flower', 'bubble', 'animation'])
    assert.equal(recipes.filter((r) => r.category === category).length, 10);
  for (const id of [
    'layered-flower',
    'pattern-flower',
    'studio-glow',
    'studio-radial',
    'cube',
    'printer'
  ])
    assert.ok(recipes.some((r) => r.id === id));
  for (const preset of recipes)
    for (const label of [preset.name, preset.text, preset.tag]) assert.ok(english[label], label);
  assert.ok(
    [...textAssetFiles.values()]
      .filter((p) => p.endsWith('composition.json'))
      .every((p) => p.includes('/presets/'))
  );
  assert.ok(![...textAssetFiles.keys()].some((p) => /\.(ttf|otf)$/.test(p)));
});

test('every shipped preset renders editable Chinese and English; static categories have distinct pixels', async () => {
  const appearances = { flower: new Set(), bubble: new Set() };
  for (const recipe of recipes.filter((r) => !r.external)) {
    // Isolate native font caches as well as the document between quality samples.
    const engine = await createTextEngine();
    const renderer = engine.createRenderer();
    registerFonts(renderer);
    const { bundle } = await composeRecipe(recipe, load);
    try {
      await renderer.loadTemplate(rewriteTemplateFonts(bundle, testFonts), {
        bindings: { content: '灵感' },
        allowRasterFallback: true
      });
      const chinese = renderer.render({ timeUs: recipe.timeUs, width: 640, height: 360 });
      assert.ok(nontransparent(chinese) > 400, recipe.id);
      if (appearances[recipe.category]) appearances[recipe.category].add(hash(chinese.data));
      renderer.setText('Hello');
      const englishFrame = renderer.render({ timeUs: recipe.timeUs, width: 640, height: 360 });
      assert.ok(nontransparent(englishFrame) > 400, recipe.id);
      assert.notEqual(hash(chinese.data), hash(englishFrame.data), recipe.id);
      assert.ok(englishFrame.controlBounds.width > 0, recipe.id);
    } catch (error) {
      throw new Error(`${recipe.id}: ${error.message}`, { cause: error });
    } finally {
      engine.dispose();
    }
  }
  assert.equal(appearances.flower.size, 10);
  assert.equal(appearances.bubble.size, 10);
});

test('eight raster animation presets change frames and reproduce earlier pixels after seeking backwards', async () => {
  const engine = await createTextEngine();
  const actions = new Set();
  try {
    for (const recipe of recipes.filter((r) => r.category === 'animation' && !r.external)) {
      const { bundle } = await composeRecipe(recipe, load);
      actions.add(JSON.stringify([bundle.animation, bundle.effectProgram]));
      const renderer = engine.createRenderer();
      try {
        registerFonts(renderer);
        await renderer.loadTemplate(rewriteTemplateFonts(bundle, testFonts), {
          bindings: { content: '动画测试' },
          allowRasterFallback: true
        });
        const sample = (timeUs) => {
          const frame = renderer.render({ timeUs, width: 640, height: 360 });
          return hash(
            Buffer.concat([
              Buffer.from(
                JSON.stringify([frame.width, frame.height, frame.originX, frame.originY])
              ),
              Buffer.from(frame.data)
            ])
          );
        };
        const early = sample(300000),
          middle = sample(850000),
          late = sample(1500000);
        assert.ok(new Set([early, middle, late]).size >= 2, recipe.id);
        sample(recipe.id === 'cube' ? 2400000 : 4200000);
        assert.equal(sample(300000), early, `${recipe.id}: backwards seek`);
        if (['anim-wave', 'anim-breathe', 'anim-swing'].includes(recipe.id))
          assert.equal(sample(3300000), early, `${recipe.id}: native looping clock`);
      } finally {
        renderer.dispose();
      }
    }
    assert.equal(actions.size, 8);
  } finally {
    engine.dispose();
  }
});

test('redesigned legacy backdrop components retain namespaced composition and no reference pixels', async () => {
  for (const backdrop of ['bubble-nine-slice', 'bubble-tile']) {
    const { bundle } = await composeRecipe(
      { base: 'flower-style-03', backdrop, animation: 'anim-lua-letter-transform' },
      load
    );
    assert.ok(bundle.assets.has('backdrop/assets/asset-000.png'));
    assert.ok(
      bundle.composition.resources.some((r) => r.resource_id === 'backdrop/assets/asset-000.png')
    );
    const original = await readFile(
      new URL(
        `../packages/text-wasm/fixtures/templates/com.videocut.text.qt-type.${backdrop}/assets/asset-000.png`,
        import.meta.url
      )
    );
    assert.notEqual(hash(bundle.assets.get('backdrop/assets/asset-000.png').bytes), hash(original));
  }
});

const withoutPaint = (value) =>
  Array.isArray(value)
    ? value.map(withoutPaint)
    : value && typeof value === 'object'
      ? Object.fromEntries(
          Object.entries(value)
            .filter(([key]) => !['color', 'fallback_name', 'name_key', 'default'].includes(key))
            .map(([key, v]) => [key, withoutPaint(v)])
        )
      : value;
const relabel = (value, from, to) =>
  JSON.parse(JSON.stringify(value).replaceAll(`qt-type.${from}`, `qt-type.${to}`));
test('all presets preserve the source layout, material stack and background resource topology', async () => {
  for (const [i, design] of presetDesigns.entries()) {
    const recipe = recipes[i];
    const reference = (
      await composeRecipe(
        design.category === 'flower' && design.backdrop
          ? {
              base: design.source,
              backdrop: design.backdrop,
              animation: 'anim-lua-letter-transform'
            }
          : { base: design.source },
        (id) => fixture(`com.videocut.text.qt-type.${id}`)
      )
    ).bundle;
    const actual = (await composeRecipe(recipe, load)).bundle;
    const expected = relabel(reference.composition, design.source, recipe.base);
    assert.deepEqual(
      withoutPaint(actual.composition.document),
      withoutPaint(expected.document),
      `${design.id}: original typography/layout`
    );
    assert.deepEqual(
      withoutPaint(actual.composition.appearance),
      withoutPaint(expected.appearance),
      `${design.id}: backdrop and SDF layout`
    );
    assert.deepEqual(
      actual.composition.resources,
      expected.resources,
      `${design.id}: resource IDs/types/closure`
    );
    assert.deepEqual(
      actual.composition.decorations,
      expected.decorations,
      `${design.id}: decoration placement`
    );
    assert.deepEqual(
      actual.animation.animation.execution_graph,
      relabel(reference.animation.animation.execution_graph, design.source, recipe.base),
      `${design.id}: execution topology`
    );
    if (design.base || design.category !== 'animation')
      assert.deepEqual(
        actual.animation,
        relabel(reference.animation, design.source, recipe.base),
        `${design.id}: inherited animation`
      );
    for (const [path, asset] of actual.assets) {
      const original = reference.assets.get(path);
      assert.ok(original, `${design.id}: ${path}`);
      assert.notEqual(
        hash(asset.bytes),
        hash(original.bytes),
        `${design.id}: replaced visual resource ${path}`
      );
      if (path.endsWith('.png')) {
        assert.equal(
          Buffer.from(asset.bytes).readUInt32BE(16),
          Buffer.from(original.bytes).readUInt32BE(16),
          `${design.id}: image width`
        );
        assert.equal(
          Buffer.from(asset.bytes).readUInt32BE(20),
          Buffer.from(original.bytes).readUInt32BE(20),
          `${design.id}: image height`
        );
      }
    }
  }
});
test('each flower has a distinct inherited material structure', () => {
  const sources = presetDesigns.filter((d) => d.category === 'flower').map((d) => d.source);
  assert.equal(new Set(sources).size, 10);
});
test('flower textures and bubble backgrounds contribute visible pixels', async () => {
  for (const recipe of recipes.filter((r) => ['flower', 'bubble'].includes(r.category))) {
    const { bundle } = await composeRecipe(recipe, load);
    assert.ok(bundle.assets.size >= 1, recipe.id);
    if (recipe.category === 'bubble')
      assert.equal(bundle.composition.appearance.backdrops.layers.length, 1, recipe.id);
    const engine = await createTextEngine();
    const render = async (enabled) => {
      const renderer = engine.createRenderer();
      registerFonts(renderer);
      try {
        const composition = structuredClone(bundle.composition);
        composition.appearance.backdrops.layers.forEach(
          (l) => (l.transform.opacity = enabled ? 1 : 0)
        );
        if (recipe.category === 'flower') {
          const disableTextures = (value) => {
            if (Array.isArray(value)) value.forEach(disableTextures);
            else if (value && typeof value === 'object') {
              if (value.kind === 'texture') value.texture_opacity = enabled ? 1 : 0;
              Object.values(value).forEach(disableTextures);
            }
          };
          disableTextures(composition.document);
        }
        await renderer.loadTemplate(rewriteTemplateFonts({ ...bundle, composition }, testFonts), {
          bindings: { content: '花字' },
          allowRasterFallback: true
        });
        return hash(renderer.render({ timeUs: 350000, width: 640, height: 360 }).data);
      } finally {
        renderer.dispose();
      }
    };
    try {
      assert.notEqual(await render(true), await render(false), recipe.id);
    } finally {
      engine.dispose();
    }
  }
});
