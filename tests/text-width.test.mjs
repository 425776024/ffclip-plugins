import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import {
  createProject,
  addText,
  editTimeline,
  CommandHistory,
  validateProject
} from '../packages/core/project.mjs';
import {
  applyTemplateLayout,
  templateLayoutInfo,
  wrapText
} from '../packages/core/text-layout.mjs';
import { applyTemplateStyle } from '../packages/core/template-style.mjs';
import { createTextEngine } from '../packages/text-wasm/dist/index.mjs';
import { fixture, registerFonts } from '../packages/text-wasm/tests/helpers.mjs';
const load = async (path) => {
  const result = await build({
    entryPoints: [path],
    bundle: true,
    platform: 'node',
    format: 'esm',
    write: false,
    logLevel: 'silent'
  });
  return import(
    'data:text/javascript;base64,' + Buffer.from(result.outputFiles[0].text).toString('base64')
  );
};
const { textWidthResize } = await load('src/editor/preview-resize.ts');
const { presentationProject } = await load('packages/render/projection.ts');

test('width reflow anchors either opposite edge with rotations, flips and existing scale; Alt retains center', () => {
  for (const rotation of [0, 35, 90, 180])
    for (const flip of [1, -1])
      for (const edge of ['left', 'right']) {
        const radians = (rotation * Math.PI) / 180;
        const axis = { x: Math.cos(radians) * flip, y: Math.sin(radians) * flip };
        const visual = { positionX: 50, positionY: -30, scaleX: 1.5, scaleY: 0.6 };
        const layout = {
          width: 200,
          minimumWidth: 1,
          left: { x: 320 - axis.x * 150, y: 180 - axis.y * 150 },
          right: { x: 320 + axis.x * 150, y: 180 + axis.y * 150 }
        };
        const resize = textWidthResize(layout, visual, edge);
        const direction = edge === 'right' ? 1 : -1;
        const result = resize(direction * axis.x * 60, direction * axis.y * 60);
        assert.ok(Math.abs(result.layoutWidth - 240) < 1e-8);
        const shift = {
          x: result.positionX - visual.positionX,
          y: result.positionY - visual.positionY
        };
        assert.ok(Math.abs(shift.x - axis.x * direction * 30) < 1e-8);
        assert.ok(Math.abs(shift.y - axis.y * direction * 30) < 1e-8);
        assert.equal(result.scaleX, undefined);
        const centered = resize(direction * axis.x * 60, direction * axis.y * 60, true);
        assert.ok(Math.abs(centered.layoutWidth - 280) < 1e-8);
        assert.equal(centered.positionX, visual.positionX);
        assert.equal(centered.positionY, visual.positionY);
      }
});

test('text width is a reversible atomic edit, persists across presets and resets without changing font or scale', () => {
  const p = createProject(),
    item = addText(p, { content: '宽度验收', fontSize: 48 });
  const original = JSON.stringify(p),
    history = new CommandHistory();
  const result = history.prepare(p, [
    { action: 'set_text', itemId: item.id, layoutWidth: 180 },
    { action: 'set_transform', itemId: item.id, positionX: 40 }
  ]);
  history.accept(result);
  const text = result.project.timeline.tracks[0].items[0].clip.text;
  assert.equal(text.fontSize, 48);
  assert.equal(text.layoutWidth, 180);
  const live = presentationProject(p, { id: item.id, x: 40, y: 0, layoutWidth: 180 });
  assert.equal(live.timeline.tracks[0].items[0].clip.text.layoutWidth, 180);
  assert.equal(JSON.stringify(p), original);
  const undone = history.prepare(result.project, [{ action: 'undo' }]);
  history.accept(undone);
  assert.deepEqual(undone.project, p);
  const switched = editTimeline(result.project, [
    { action: 'set_text', itemId: item.id, template: { id: 'layered-flower', version: 1 } }
  ]).project;
  assert.equal(
    validateProject(JSON.parse(JSON.stringify(switched))).timeline.tracks[0].items[0].clip.text
      .layoutWidth,
    180
  );
  const reset = editTimeline(switched, [
    { action: 'set_text', itemId: item.id, layoutWidth: null }
  ]).project;
  assert.equal(reset.timeline.tracks[0].items[0].clip.text.layoutWidth, undefined);
  for (const width of [0, -1, Infinity, NaN, 65537, '200'])
    assert.throws(() =>
      editTimeline(p, [
        { action: 'set_text', itemId: item.id, content: '不应提交', layoutWidth: width }
      ])
    );
  assert.equal(JSON.stringify(p), original);
});

test('basic wrapping preserves explicit newlines, words and grapheme clusters', () => {
  const measure = (text) =>
    [...new Intl.Segmenter(undefined, { granularity: 'grapheme' }).segment(text)].length;
  assert.deepEqual(wrapText('hello world', 6, measure), ['hello', 'world']);
  assert.deepEqual(wrapText('宽度调节\n👩‍💻👩‍💻', 2, measure), ['宽度', '调节', '👩‍💻👩‍💻']);
  assert.deepEqual(wrapText('hello\n\nworld', undefined, measure), ['hello', '', 'world']);
});

test('actual WASM reflow changes line layout while keeping authored font sizes and cached templates intact', async () => {
  const original = await fixture('com.videocut.text.qt-type.flower-style-03');
  const bundle = applyTemplateStyle(original, { fontSize: 96 });
  const before = JSON.stringify(bundle.composition),
    canvas = { width: 640, height: 360 };
  const wide = applyTemplateLayout(bundle, 360, canvas),
    narrow = applyTemplateLayout(bundle, 120, canvas);
  assert.equal(templateLayoutInfo(narrow, canvas).width, 120);
  assert.equal(narrow.composition.document.paragraphs[0].runs[0].style.font_size, 96);
  assert.equal(JSON.stringify(bundle.composition), before);
  const engine = await createTextEngine(),
    renderers = [];
  try {
    const render = async (value) => {
      const renderer = engine.createRenderer();
      renderers.push(renderer);
      registerFonts(renderer);
      await renderer.loadTemplate(value, {
        bindings: { content: '文字宽度调节不会改变字号' },
        allowRasterFallback: true
      });
      return renderer.render({ timeUs: 0, ...canvas });
    };
    const a = await render(wide),
      b = await render(narrow);
    assert.ok(
      b.logicalBounds.height > a.logicalBounds.height,
      `${b.logicalBounds.height} <= ${a.logicalBounds.height}`
    );
    assert.ok(b.logicalBounds.width < a.logicalBounds.width);
    assert.ok(b.data.some((byte) => byte !== 0));
  } finally {
    renderers.forEach((renderer) => renderer.dispose());
    engine.dispose();
  }
});
