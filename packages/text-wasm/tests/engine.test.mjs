import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createTextEngine } from '../dist/index.mjs';
import { fixture, fonts, registerFonts, nontransparent } from './helpers.mjs';
const engine = await createTextEngine();
test('WASM uses the four-lane SIMD raster pipeline', () => {
  assert.equal(engine.capabilities.rasterPipelineLanes, 4);
});
after(() => engine.dispose());
const options = { allowRasterFallback: true, bindings: { content: '你好' }, fallbackFonts: [{ id: fonts[1].id, family: 'Source Han Sans SC' }] };
const hash = (frame) => createHash('sha256').update(frame.data).digest('hex');
async function renderer(id = 'flower-style-09') {
  const r = engine.createRenderer(); registerFonts(r);
  await r.loadTemplate(await fixture(`com.videocut.text.qt-type.${id}`), options);
  return r;
}
test('native C++ flower template renders transparent RGBA and edits Chinese without mutating prior frames', async () => {
  const r = await renderer();
  try {
    const frame = r.render({ timeUs: 1500000, width: 640, height: 360 });
    const oldHash = hash(frame);
    assert.ok(nontransparent(frame) > 1000);
    assert.ok(frame.data.some((v, i) => i % 4 === 3 && v === 0));
    assert.equal(frame.data.length, frame.width * frame.height * 4);
    assert.equal(hash(r.render({ timeUs: 1500000, width: 640, height: 360 })), oldHash);
    r.setText('花字');
    assert.notEqual(hash(r.render({ timeUs: 1500000, width: 640, height: 360 })), oldHash);
    assert.equal(hash(frame), oldHash);
    assert.throws(() => r.setBindings({ content: '' }), /Bindings/);
    r.dispose(); assert.equal(hash(frame), oldHash);
    assert.throws(() => r.render(), /disposed/);
  } finally { r.dispose(); }
});
test('native animation changes pixels with time and seeking is deterministic', async () => {
  const r = await renderer('anim-lua-opacity');
  try {
    const a = r.render({ timeUs: 1500000, width: 640, height: 360 });
    const b = r.render({ timeUs: 3000000, width: 640, height: 360 });
    assert.ok(nontransparent(a) > 0); assert.ok(nontransparent(b) > 0);
    assert.notEqual(hash(a), hash(b));
    assert.equal(hash(r.render({ timeUs: 1500000, width: 640, height: 360 })), hash(a));
  } finally { r.dispose(); }
});
test('requires explicit raster fallback and registered fonts', async () => {
  const bundle = await fixture('com.videocut.text.qt-type.flower-style-09');
  const r = engine.createRenderer(); registerFonts(r);
  try { await assert.rejects(r.loadTemplate(bundle), /SDF\/Metal fidelity/); }
  finally { r.dispose(); }
  const missing = engine.createRenderer();
  try {
    await assert.rejects(async () => {
      await missing.loadTemplate(bundle, options);
      missing.render();
    }, /font|asset/i);
  } finally { missing.dispose(); }
});
test('reports unsupported native post effects without silently dropping them', async () => {
  const r = engine.createRenderer(); registerFonts(r);
  try {
    await assert.rejects(r.loadTemplate(await fixture('com.videocut.text.qt-type.anim-studio-alpha-plugin-wipe'), options), /post-effects/);
  } finally { r.dispose(); }
});
test('renderer lifetime and input bounds', async () => {
  const r = await renderer();
  try {
    for (const opts of [{ width: 9999 }, { height: 0 }, { timeUs: -1 }, { timeUs: 1.1 }]) assert.throws(() => r.render(opts), /Invalid render/);
    assert.throws(() => r.registerAsset('late', 'font/ttf', fonts[0].bytes), /before loading/);
    await assert.rejects(r.loadTemplate(await fixture('com.videocut.text.qt-type.flower-style-09')), /new renderer/);
  } finally { r.dispose(); }
  const independent = await createTextEngine(); const owned = independent.createRenderer();
  independent.dispose(); independent.dispose(); owned.dispose();
  assert.throws(() => owned.render(), /disposed/);
  assert.throws(() => independent.createRenderer(), /disposed/);
});
