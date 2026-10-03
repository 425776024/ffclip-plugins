import test from 'node:test';
import assert from 'node:assert/strict';
import { createProject, addAsset, ticks, quantizeTime } from '../packages/core/project.mjs';
import {
  buildRenderPlan,
  layerGeometry,
  previewExtent,
  previewSampleTime,
  frameTime,
  makeLut
} from '../packages/render/plan.mjs';

const close = (actual, expected) =>
  assert.ok(Math.abs(actual - expected) < 1e-8, `${actual} != ${expected}`);
const visual = { positionX: 0, positionY: 0, scaleX: 1, scaleY: 1, rotationDegrees: 0, opacity: 1 };

test('playback holds authored frames including fractional tick rates; paused seeks keep exact ticks', () => {
  for (const rate of [
    { numerator: 30, denominator: 1 },
    { numerator: 30000, denominator: 1001 },
    { numerator: 31, denominator: 1 }
  ]) {
    for (const index of [0, 1, 15, 16, 3000]) {
      const start = frameTime(index, rate),
        end = frameTime(index + 1, rate);
      assert.equal(previewSampleTime(start, rate, true), start);
      assert.equal(previewSampleTime(end - 1, rate, true), start);
      assert.equal(previewSampleTime(end, rate, true), end);
      assert.equal(previewSampleTime(end - 1, rate, false), end - 1);
    }
  }
});

test('canvas geometry keeps aspect, native crop proportions and transform anchor at every output resolution', () => {
  const project = createProject();
  const full = layerGeometry(project, visual, 1920, 1080, 960, 540);
  assert.deepEqual(full.matrix, [960, 0, -0, 540, 0, 0]);
  assert.deepEqual(full.crop, [0, 0, 1, 1]);
  const cropped = layerGeometry(
    project,
    { ...visual, crop: { left: 0.25, right: 0.25, top: 0, bottom: 0 } },
    1920,
    1080,
    960,
    540
  );
  assert.deepEqual(cropped.crop, [0.25, 0, 0.5, 1]);
  assert.deepEqual(cropped.bounds, { x: 480, y: 0, width: 960, height: 1080 });
  const zoom = layerGeometry(
    project,
    { ...visual, anchorX: 0, anchorY: 0, scaleX: 2, scaleY: 2, positionX: 10, positionY: 20 },
    1920,
    1080,
    1920,
    1080
  );
  assert.deepEqual(zoom.bounds, { x: 10, y: 20, width: 3840, height: 2160 });
  const flip = layerGeometry(project, { ...visual, flipHorizontal: true }, 1920, 1080, 1920, 1080);
  const [a, b, c, d, e, f] = flip.inverse;
  close(a * 0 + b * 0.5 + c, 1);
  close(d * 0 + e * 0.5 + f, 0.5);
  close(a * 1 + b * 0.5 + c, 0);
});

test('inverse geometry roundtrips a rotated, nonuniform layer independently of preview scale', () => {
  const p = createProject(),
    v = {
      ...visual,
      positionX: 211,
      positionY: -74,
      rotationDegrees: 32,
      scaleX: 0.8,
      scaleY: 1.3
    };
  const g = layerGeometry(p, v, 640, 480, 800, 450);
  const [a, b, c, d, e, f] = g.inverse,
    m = g.matrix;
  for (const [u, v] of [
    [0, 0],
    [0.25, 0.7],
    [1, 1]
  ]) {
    const x = (m[0] * u + m[2] * v + m[4]) / 800,
      y = (m[1] * u + m[3] * v + m[5]) / 450;
    close(a * x + b * y + c, u);
    close(d * x + e * y + f, v);
  }
});

test('DPR preview sizing is capped for playback while export frame mapping stays rational', () => {
  const canvas = { width: 3840, height: 2160 };
  assert.deepEqual(previewExtent(canvas, 960, 540, 2, false), { width: 1920, height: 1080 });
  assert.deepEqual(previewExtent(canvas, 960, 540, 2, true), { width: 1280, height: 720 });
  assert.deepEqual(previewExtent(canvas, 8000, 4500, 2, false), canvas);
  assert.equal(frameTime(30000, { numerator: 30000, denominator: 1001 }), 120120000);
});

test('59.94 frame boundaries and nearest-frame snapping stay exact after one hour', () => {
  const rate = { numerator: 60000, denominator: 1001 };
  assert.equal(frameTime(1, rate), 2002);
  assert.equal(frameTime(60000, rate), 120120000);
  assert.equal(frameTime(215784, rate), 431999568);
  assert.equal(quantizeTime(ticks(3600), rate), 431999568);
  assert.equal(quantizeTime(432000568, rate), 431999568);
  assert.equal(quantizeTime(432000569, rate), 432001570);
  assert.equal(frameTime(215785, rate) - frameTime(215784, rate), 2002);
});

test('transition plan uses both handles once, honors hidden tracks and evaluates animated properties', () => {
  const p = createProject(),
    asset = {
      id: 'asset-1',
      name: 'a.mp4',
      path: '/a.mp4',
      kind: 'video',
      duration: ticks(10),
      size: 1,
      width: 1920,
      height: 1080,
      hasAudio: false
    };
  const a = addAsset(p, asset);
  a.placement = { begin: 0, end: ticks(2) };
  a.clip.source = { begin: ticks(2), end: ticks(4) };
  const b = addAsset(p, asset, { trackId: p.timeline.tracks[0].id, start: ticks(2) });
  b.placement.end = ticks(4);
  b.clip.source = { begin: ticks(4), end: ticks(6) };
  a.clip.automation = {
    'visual.opacity': {
      timeDomain: 'itemLocal',
      keyframes: [
        { id: 'k1', time: 0, value: 0, interpolation: 'linear' },
        { id: 'k2', time: ticks(2), value: 1, interpolation: 'linear' }
      ]
    }
  };
  p.timeline.transitions = [
    {
      id: 'transition-1',
      fromItemId: a.id,
      toItemId: b.id,
      templateId: 'dissolve',
      duration: ticks(1),
      parameters: {}
    }
  ];
  const before = buildRenderPlan(p, ticks(1));
  assert.equal(before.layers.length, 1);
  assert.equal(before.layers[0].layer.visual.opacity, 0.5);
  const midpoint = buildRenderPlan(p, ticks(2));
  assert.equal(midpoint.layers.length, 1);
  const entry = midpoint.layers[0];
  assert.equal(entry.kind, 'transition');
  assert.equal(entry.transition.progress, 0.5);
  assert.equal(entry.transition.style, 'cross_dissolve');
  assert.equal(entry.from.sourceTime, ticks(4));
  assert.equal(entry.to.sourceTime, ticks(4));
  p.timeline.tracks[0].visible = false;
  assert.deepEqual(buildRenderPlan(p, ticks(2)).layers, []);
});

test('LUT strip stores RGB cube axes without changing alpha', () => {
  const { data, size } = makeLut('identity', 4);
  const pixel = (r, g, b) => [
    ...data.slice((g * size * size + b * size + r) * 4, (g * size * size + b * size + r) * 4 + 4)
  ];
  assert.deepEqual(pixel(0, 0, 0), [0, 0, 0, 255]);
  assert.deepEqual(pixel(3, 0, 0), [255, 0, 0, 255]);
  assert.deepEqual(pixel(0, 3, 0), [0, 255, 0, 255]);
  assert.deepEqual(pixel(0, 0, 3), [0, 0, 255, 255]);
  assert.throws(() => makeLut('missing'), /未知LUT/);
});

test('LUT automation is evaluated into each frame plan without mutating authored effects', () => {
  const p = createProject(),
    item = addAsset(p, {
      id: 'animated-image',
      name: 'image.png',
      path: '/image.png',
      kind: 'image',
      size: 1,
      width: 640,
      height: 360,
      duration: ticks(5),
      hasAudio: false
    });
  item.clip.effects = [
    { id: 'look.1', templateId: 'lut', enabled: true, parameters: { preset: 'warm', amount: 0.5 } }
  ];
  item.clip.automation = {
    'effects.look.1.amount': {
      timeDomain: 'itemLocal',
      keyframes: [
        { id: 'ka', time: 0, value: 0, interpolation: 'linear' },
        { id: 'kb', time: ticks(1), value: 1, interpolation: 'linear' }
      ]
    }
  };
  assert.equal(buildRenderPlan(p, 0).layers[0].layer.effects[0].parameters.amount, 0);
  assert.equal(buildRenderPlan(p, ticks(0.25)).layers[0].layer.effects[0].parameters.amount, 0.25);
  assert.equal(item.clip.effects[0].parameters.amount, 0.5);
});

test('automatic encoding prefers the browser and gives WebM guidance when optional FFmpeg is absent', async () => {
  const { selectEncoding } = await import('../packages/render/capabilities.mjs');
  const supported = {
    format: 'mp4',
    localEncoder: false,
    videoSupported: true,
    audioSupported: true
  };
  assert.equal(selectEncoding(supported), 'browser');
  assert.equal(selectEncoding({ ...supported, localEncoder: true }), 'browser');
  assert.throws(() => selectEncoding({ ...supported, audioSupported: false }), /AAC.*WebM/);
  assert.equal(
    selectEncoding({ ...supported, localEncoder: true, audioSupported: false }),
    'video-with-pcm'
  );
  assert.equal(
    selectEncoding({ ...supported, localEncoder: true, videoSupported: false }),
    'frames-with-pcm'
  );
  for (const preferredEncoding of ['video-with-pcm', 'frames-with-pcm'])
    assert.throws(() => selectEncoding({ ...supported, preferredEncoding }), /未提供本地编码器/);
});
