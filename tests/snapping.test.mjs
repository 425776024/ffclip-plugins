import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';

const bundled = await build({
  entryPoints: ['src/editor/snapping.ts'],
  bundle: true,
  platform: 'node',
  format: 'esm',
  write: false,
  logLevel: 'silent'
});
const { MagneticSnap, previewAxisSnap } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundled.outputFiles[0].text).toString('base64')
);
const point = (targets) => new MagneticSnap([{ offset: 0, targets }]);

test('capture uses screen pixels at every timeline zoom, before frame rounding', () => {
  for (const zoom of [15, 80, 220]) {
    const snap = point([{ value: 480000 }]),
      scale = zoom / 120000;
    assert.equal(snap.snap(480000 + 4 / scale, scale).value, 480000);
    snap.clear();
    assert.equal(snap.snap(480000 + 6 / scale, scale).guide, undefined);
  }
});

test('holds one target through noise and releases beyond the wider band', () => {
  const snap = point([{ value: 100 }, { value: 108 }]);
  assert.equal(snap.snap(104, 1).value, 100);
  assert.equal(snap.snap(107, 1).value, 100, 'Must not switch to the closer target while held');
  assert.equal(snap.snap(109, 1).value, 108);
  assert.equal(snap.snap(117, 1).guide, undefined);
});

test('clip trailing edge and every selected group edge are eligible probes', () => {
  const snap = new MagneticSnap(
    [0, 2, 4, 5].map((offset) => ({
      offset,
      targets: [{ value: 10 }]
    }))
  );
  assert.deepEqual(snap.snap(5.04, 80), { value: 5, guide: 10 });
  assert.deepEqual(snap.snap(5.08, 80), { value: 5, guide: 10 });
});

test('playhead attraction is slightly wider', () => {
  const snap = point([{ value: 100, multiplier: 1.25 }, { value: 200 }]);
  assert.equal(snap.snap(106, 1).guide, 100);
  snap.clear();
  assert.equal(snap.snap(206, 1).guide, undefined);
});

test('disabled snapping clears sticky state immediately', () => {
  const snap = point([{ value: 100 }]);
  assert.equal(snap.snap(104, 1).guide, 100);
  assert.deepEqual(snap.snap(107, 1, false), { value: 107 });
  assert.deepEqual(snap.snap(107, 1), { value: 107 });
  assert.deepEqual(snap.snap(100, 0), { value: 100 });
});

test('ineligible source limits never show a guide after clamping', () => {
  const snap = point([{ value: 100 }]);
  assert.deepEqual(snap.snap(102, 1, true, 101, 110), { value: 102 });
  assert.deepEqual(snap.snap(104, 1, true, 0, 105), { value: 100, guide: 100 });
  assert.deepEqual(snap.snap(104, 1, true, 101, 110), { value: 104 });
});

test('preview center and both canvas edges use presented geometry at any viewport scale', () => {
  for (const scale of [0.25, 0.5, 1, 2]) {
    // Presented bounds have a shifted center (e.g. text layout or custom anchor).
    const snap = previewAxisSnap(310, 160, 80, 640, 0);
    assert.equal(snap.snap(10 + 4 / scale, scale).value, 10);
    snap.clear();
    assert.equal(snap.snap(-230 + 4 / scale, scale).value, -230);
    snap.clear();
    assert.equal(snap.snap(250 - 4 / scale, scale).value, 250);
  }
});

test('rotated layers only snap their center except at quarter turns', () => {
  const rotated = previewAxisSnap(310, 160, 80, 640, 45);
  assert.equal(rotated.snap(14, 1).guide, 320);
  rotated.clear();
  assert.equal(rotated.snap(-226, 1).guide, undefined);
  assert.equal(previewAxisSnap(310, 160, 80, 640, 90).snap(-226, 1).guide, 0);
});

test('indexed search agrees with nearest eligible probes over a dense timeline', () => {
  const targets = Array.from({ length: 10000 }, (_, n) => ({ value: n * 1200 }));
  const probes = [0, 48000].map((offset) => ({ offset, targets }));
  for (const position of [238777, 45678, 938421, 133333, 20000000]) {
    const snap = new MagneticSnap(probes);
    const result = snap.snap(position, 80 / 120000);
    const nearest = probes
      .flatMap(({ offset }) =>
        targets.map(({ value }) => ({
          value: value - offset,
          guide: value,
          distance: Math.abs(position + offset - value)
        }))
      )
      .sort((a, b) => a.distance - b.distance)[0];
    assert.deepEqual(
      result,
      (nearest.distance * 80) / 120000 <= 5
        ? { value: nearest.value, guide: nearest.guide }
        : { value: position }
    );
  }
});
