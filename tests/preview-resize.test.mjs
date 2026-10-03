import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { layerGeometry } from '../packages/render/plan.mjs';
import { createProject, addText, evaluateVisual, ticks } from '../packages/core/project.mjs';

async function load(path) {
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
}
const { cornerResize } = await load('src/editor/preview-resize.ts');
const { presentationProject } = await load('packages/render/projection.ts');
const project = { canvas: { width: 1920, height: 1080 } };
const rect = { x: 460, y: 200, width: 750, height: 280 };
function geometry(visual) {
  return layerGeometry(project, visual, 1920, 1080, 1920, 1080, true);
}
function bounds(visual) {
  const m = geometry(visual).matrix;
  const points = [
    [rect.x, rect.y],
    [rect.x + rect.width, rect.y],
    [rect.x, rect.y + rect.height],
    [rect.x + rect.width, rect.y + rect.height]
  ].map(([x, y]) => [
    (m[0] * x) / 1920 + (m[2] * y) / 1080 + m[4],
    (m[1] * x) / 1920 + (m[3] * y) / 1080 + m[5]
  ]);
  const x = Math.min(...points.map((p) => p[0])),
    y = Math.min(...points.map((p) => p[1]));
  return {
    x,
    y,
    width: Math.max(...points.map((p) => p[0])) - x,
    height: Math.max(...points.map((p) => p[1])) - y
  };
}
const near = (a, b) => assert.ok(Math.abs(a - b) < 1e-7, `${a} differs from ${b}`);

test('all four text corners keep their opposite point fixed through rotation, flips and noncentral anchors', () => {
  for (const rotationDegrees of [0, 35, 90, 145])
    for (const flipHorizontal of [false, true])
      for (const corner of ['tl', 'tr', 'bl', 'br']) {
        const visual = {
          positionX: 70,
          positionY: -40,
          scaleX: 1.4,
          scaleY: 0.6,
          rotationDegrees,
          anchorX: 0.2,
          anchorY: 0.85,
          flipHorizontal
        };
        const original = bounds(visual),
          m = geometry(visual).matrix;
        const pivot = {
          x: m[4] + m[0] * visual.anchorX + m[2] * visual.anchorY,
          y: m[5] + m[1] * visual.anchorX + m[3] * visual.anchorY
        };
        const right = corner.endsWith('r'),
          bottom = corner.startsWith('b');
        const sample = cornerResize(original, visual, pivot, corner);
        const result = sample(
          (right ? 1 : -1) * original.width * 0.5,
          (bottom ? 1 : -1) * original.height * 0.5
        );
        near(result.scaleX, visual.scaleX * 1.5);
        near(result.scaleY, visual.scaleY * 1.5);
        const resized = bounds({ ...visual, ...result });
        near(original.x + (right ? 0 : original.width), resized.x + (right ? 0 : resized.width));
        near(
          original.y + (bottom ? 0 : original.height),
          resized.y + (bottom ? 0 : resized.height)
        );
        near(resized.width, original.width * 1.5);
        near(resized.height, original.height * 1.5);
      }
});

test('corner scaling clamps to the authored range and does not flip when crossing the opposite corner', () => {
  const visual = { positionX: 0, positionY: 0, scaleX: 2, scaleY: 0.5, rotationDegrees: 0 };
  const sample = cornerResize(bounds(visual), visual, { x: 960, y: 540 }, 'br');
  const tiny = sample(-100000, -100000),
    large = sample(100000, 100000);
  near(tiny.scaleY, 0.01);
  near(tiny.scaleX, 0.04);
  near(large.scaleX, 20);
  near(large.scaleY, 5);
  assert.ok(tiny.scaleX > 0 && tiny.scaleY > 0);
  // Division followed by multiplication can round beyond the strict project
  // limits even when the ratio itself was clamped.
  for (const scale of [0.148005148005148, 1.3754330167810667]) {
    const edge = { ...visual, scaleX: scale, scaleY: scale };
    const clamped = cornerResize(bounds(edge), edge, { x: 960, y: 540 }, 'br');
    for (const delta of [-100000, 100000]) {
      const value = clamped(delta, delta);
      assert.ok(value.scaleX >= 0.01 && value.scaleX <= 20);
      assert.ok(value.scaleY >= 0.01 && value.scaleY <= 20);
      const initial = createProject(),
        item = addText(initial, { content: '边界' });
      assert.doesNotThrow(() =>
        presentationProject(initial, {
          id: item.id,
          x: value.positionX,
          y: value.positionY,
          scaleX: value.scaleX,
          scaleY: value.scaleY
        })
      );
    }
  }
});

test('live resize overrides sampled position and scale without modifying authored values or keyframes', () => {
  const initial = createProject(),
    item = addText(initial, { content: '控制点' });
  item.clip.automation = Object.fromEntries(
    ['positionX', 'positionY', 'scaleX', 'scaleY', 'opacity'].map((key) => [
      `visual.${key}`,
      {
        timeDomain: 'itemLocal',
        keyframes: [
          {
            id: `kf-${key}`,
            time: 0,
            value: key.startsWith('scale') ? 2 : 0.5,
            interpolation: 'linear'
          }
        ]
      }
    ])
  );
  const original = JSON.stringify(initial);
  const resized = presentationProject(initial, { id: item.id, x: 12, y: 34, scaleX: 3, scaleY: 4 });
  const edited = resized.timeline.tracks[0].items[0],
    visual = evaluateVisual(edited, ticks(1));
  assert.equal(visual.positionX, 12);
  assert.equal(visual.positionY, 34);
  assert.equal(visual.scaleX, 3);
  assert.equal(visual.scaleY, 4);
  assert.equal(visual.opacity, 0.5);
  assert.equal(JSON.stringify(initial), original);
  assert.equal(edited.clip.text, item.clip.text);
  const moved = presentationProject(initial, { id: item.id, x: 12, y: 34 });
  assert.equal(evaluateVisual(moved.timeline.tracks[0].items[0], 0).scaleX, 2);
  assert.throws(
    () => presentationProject(initial, { id: item.id, x: 0, y: 0, scaleX: 0 }),
    /scale/
  );
});
