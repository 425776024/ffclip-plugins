import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import {
  createProject,
  addText,
  editTimeline,
  validateProject,
  splitItem,
  ticks,
  CommandHistory
} from '../packages/core/project.mjs';
import { applyTemplateStyle } from '../packages/core/template-style.mjs';
import { createTextEngine } from '../packages/text-wasm/dist/index.mjs';
import { fixture, registerFonts } from '../packages/text-wasm/tests/helpers.mjs';
import { composeRecipe, recipes } from '../packages/text-wasm/src/recipes.mjs';

test('switching the selected template preserves content, identity, timing and transforms, with reversible history', () => {
  const initial = createProject();
  const item = addText(initial, {
    content: '保留内容',
    start: ticks(2),
    color: '#ff5c70',
    fontSize: 96
  });
  item.clip.visual.positionX = 123;
  const history = new CommandHistory();
  const template = { id: 'layered-flower', version: 1, style: { color: '#ff5c70', fontSize: 240 } };
  const result = history.prepare(initial, [{ action: 'set_text', itemId: item.id, template }]);
  history.accept(result);
  const updated = result.project.timeline.tracks[0].items[0];
  assert.equal(updated.id, item.id);
  assert.equal(updated.clip.text.content, item.clip.text.content);
  assert.deepEqual(updated.clip.source, item.clip.source);
  assert.deepEqual(updated.placement, item.placement);
  assert.deepEqual(updated.clip.visual, item.clip.visual);
  assert.deepEqual(updated.clip.text.template, template);
  assert.equal(updated.clip.text.color, '#ffffff');
  assert.equal(updated.clip.text.fontSize, 64);
  const restored = validateProject(JSON.parse(JSON.stringify(result.project)));
  const right = splitItem(restored, item.id, ticks(4));
  assert.deepEqual(right.clip.text.template, template);
  assert.equal(right.clip.source.begin, ticks(2));
  const undone = history.prepare(result.project, [{ action: 'undo' }]);
  history.accept(undone);
  assert.deepEqual(undone.project, initial);
  const redone = history.prepare(undone.project, [{ action: 'redo' }]);
  history.accept(redone);
  assert.deepEqual(redone.project, result.project);
  const basic = editTimeline(redone.project, [
    { action: 'set_text', itemId: item.id, template: null, color: '#55d6a5', fontSize: 96 }
  ]).project;
  assert.equal(basic.timeline.tracks[0].items[0].clip.text.template, undefined);
  assert.equal(basic.timeline.tracks[0].items[0].clip.text.color, '#55d6a5');
});

test('template style and animation validation rejects an invalid batch without publishing partial edits', () => {
  const initial = createProject(),
    item = addText(initial, { content: '测试' });
  const before = JSON.stringify(initial);
  for (const style of [{ color: 'red' }, { fontSize: 0 }, { fontSize: 1001 }, { opacity: 0.5 }])
    assert.throws(() =>
      editTimeline(initial, [
        { action: 'set_text', itemId: item.id, content: '不应提交' },
        { action: 'set_text', itemId: item.id, template: { id: 'cube', version: 1, style } }
      ])
    );
  assert.throws(
    () =>
      editTimeline(initial, [
        {
          action: 'set_text',
          itemId: item.id,
          template: {
            id: 'invalid-overlay',
            version: 1,
            recipe: { base: 'anim-lua-cube', animation: 'anim-lua-letter-transform' }
          }
        }
      ]),
    /只支持/
  );
  assert.equal(JSON.stringify(initial), before);
  const recipe = { base: 'flower-style-03', backdrop: 'bubble-nine-slice' };
  const edited = editTimeline(initial, [
    { action: 'set_text', itemId: item.id, template: { id: 'static-flower', version: 1, recipe } }
  ]).project;
  assert.deepEqual(edited.timeline.tracks[0].items[0].clip.text.template.recipe, recipe);
});

test('queued color, size and template edits merge at publication and reset only the requested override', () => {
  const initial = createProject(),
    item = addText(initial, { content: '样式', template: { id: 'layered-flower', version: 1 } });
  const result = editTimeline(initial, [
    { action: 'set_text', itemId: item.id, templateStyle: { color: '#69a7ff' } },
    { action: 'set_text', itemId: item.id, templateStyle: { fontSize: 180 } },
    { action: 'set_text', itemId: item.id, template: { id: 'pattern-flower', version: 1 } }
  ]).project;
  assert.deepEqual(result.timeline.tracks[0].items[0].clip.text.template.style, {
    color: '#69a7ff',
    fontSize: 180
  });
  const reset = editTimeline(result, [
    { action: 'set_text', itemId: item.id, templateStyle: { color: null } }
  ]).project;
  assert.deepEqual(reset.timeline.tracks[0].items[0].clip.text.template.style, { fontSize: 180 });
  const basic = editTimeline(reset, [
    { action: 'set_text', itemId: item.id, template: null }
  ]).project;
  assert.equal(basic.timeline.tracks[0].items[0].clip.text.fontSize, 180);
  assert.equal(basic.timeline.tracks[0].items[0].clip.text.template, undefined);
});

test('WASM color edits change actual glyph pixels and size edits change geometry without leaking to another clip', async () => {
  const recipe = recipes.find((entry) => entry.id === 'layered-flower');
  const { bundle } = await composeRecipe(recipe, (id) =>
    fixture(`com.videocut.text.qt-type.${id}`)
  );
  const original = JSON.stringify(bundle.composition);
  const engine = await createTextEngine();
  const renderers = [];
  const render = async (style) => {
    const renderer = engine.createRenderer();
    renderers.push(renderer);
    registerFonts(renderer);
    await renderer.loadTemplate(applyTemplateStyle(bundle, style), {
      bindings: { content: '颜色' },
      allowRasterFallback: true
    });
    return renderer.render({ timeUs: 350000, width: 640, height: 360 });
  };
  try {
    const a = await render(),
      blue = await render({ color: '#0000ff' }),
      small = await render({ fontSize: 180 });
    const hash = (frame) => createHash('sha256').update(frame.data).digest('hex');
    assert.notEqual(hash(blue), hash(a));
    assert.deepEqual(blue.controlBounds, a.controlBounds);
    let bluePixels = 0;
    for (let i = 0; i < blue.data.length; i += 4)
      if (
        blue.data[i + 3] > 200 &&
        blue.data[i + 2] > 200 &&
        blue.data[i] < 30 &&
        blue.data[i + 1] < 30
      )
        bluePixels++;
    assert.ok(bluePixels > 100, `Expected blue glyph fill, found ${bluePixels} pixels`);
    assert.ok(small.inkBounds.width < a.inkBounds.width);
    assert.ok(small.inkBounds.height < a.inkBounds.height);
    assert.equal(JSON.stringify(bundle.composition), original);
    assert.equal(hash(await render()), hash(a));
  } finally {
    for (const renderer of renderers) renderer.dispose();
    engine.dispose();
  }
});
