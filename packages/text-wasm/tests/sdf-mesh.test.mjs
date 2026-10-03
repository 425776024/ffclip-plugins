import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createTextEngine } from '../dist/index.mjs';
import { fixture, registerFonts, fonts } from './helpers.mjs';
const hash = (data) => createHash('sha256').update(new Uint8Array(data.buffer)).digest('hex');
test('native SDF mesh is finite, deterministic and owns copied storage; editing changes geometry', async () => {
  const e = await createTextEngine(),
    r = e.createRenderer();
  try {
    registerFonts(r);
    await r.loadTemplate(await fixture('com.videocut.text.qt-type.flower-style-09'), {
      allowRasterFallback: true,
      bindings: { content: '花字' },
      fallbackFonts: [{ id: fonts[1].id, family: 'Source Han Sans SC' }]
    });
    const a = r.prepareSdfMesh({ width: 640, height: 360 });
    const original = hash(a.distanceVertices);
    assert.equal(a.glyphCount, 2);
    assert.equal(a.scope, 'base-layout-outline-only');
    assert.ok(a.distanceCount > 0);
    assert.equal(a.distanceVertices.length, a.distanceCount * 8);
    assert.equal(a.shapeVertices.length, a.shapeCount * 4);
    assert.ok(a.distanceVertices.every(Number.isFinite));
    assert.ok(a.shapeVertices.every(Number.isFinite));
    assert.equal(hash(r.prepareSdfMesh({ width: 640, height: 360 }).distanceVertices), original);
    r.setText('你好');
    const b = r.prepareSdfMesh({ width: 640, height: 360 });
    assert.notEqual(hash(b.distanceVertices), original);
    assert.equal(hash(a.distanceVertices), original);
    assert.throws(() => r.prepareSdfMesh({ width: 4096 }), /Invalid SDF/);
    assert.throws(() => r.prepareSdfMesh({ range: NaN }), /Invalid SDF/);
    r.dispose();
    assert.equal(hash(a.distanceVertices), original);
    assert.throws(() => r.prepareSdfMesh(), /disposed/);
  } finally {
    e.dispose();
  }
});
