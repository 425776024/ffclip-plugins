import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const sdkSource = await readFile(new URL('../src/index.mjs', import.meta.url), 'utf8');
const nativeImport = "import initModule from './videocut-text.mjs';";

/** Supply native heap/result fixtures while executing the complete production JS wrapper. */
async function fixture(t, stride) {
  const pointer = 17,
    width = 2,
    height = 2;
  let pixels = Uint8ClampedArray.from([
    11, 29, 47, 255, 71, 89, 107, 128, 131, 149, 167, 64, 191, 209, 227, 0
  ]);
  const module = { HEAPU8: new Uint8Array(256), _vct_raster_lanes: () => 4 };
  const calls = {
    vct_create: () => 1,
    vct_destroy: () => {},
    vct_register_asset: () => 1,
    vct_load: () => 1,
    vct_pixels: () => pointer,
    vct_render: () => {
      module.HEAPU8.fill(238);
      for (let y = 0; y < height; y++)
        module.HEAPU8.set(
          pixels.subarray(y * width * 4, (y + 1) * width * 4),
          pointer + y * stride
        );
      return 1;
    },
    vct_result: () =>
      JSON.stringify({
        ok: true,
        profile: 'wasm-raster-v1',
        width,
        height,
        rowBytes: stride,
        originX: 11,
        originY: 7,
        controlBounds: { x: 11, y: 7, width: 2, height: 2 },
        logicalBounds: { x: 11, y: 7, width: 2, height: 2 },
        inkBounds: { x: 11, y: 7, width: 2, height: 2 }
      })
  };
  module.cwrap = (name) => calls[name];
  const fixtureKey = `__videocutPixelFixture${stride}`;
  globalThis[fixtureKey] = async () => module;
  t.after(() => {
    delete globalThis[fixtureKey];
  });
  assert.ok(
    sdkSource.includes(nativeImport),
    'SDK native import changed; update only the fixture injection'
  );
  const source = sdkSource.replace(nativeImport, `const initModule = globalThis.${fixtureKey};`);
  const sdk = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
  const engine = await sdk.createTextEngine();
  t.after(() => engine.dispose());
  return {
    module,
    renderer: engine.createRenderer(),
    replacePixels: (value) => {
      pixels = Uint8ClampedArray.from(value);
    },
    original: Array.from(pixels)
  };
}

for (const [name, stride] of [
  ['contiguous', 8],
  ['padded', 12]
]) {
  test(`${name} native RGBA returns tight owned pixels and preserves draw origins/lifetime`, async (t) => {
    const { module, renderer, replacePixels, original } = await fixture(t, stride);
    const performanceDescriptor = Object.getOwnPropertyDescriptor(globalThis, 'performance');
    const imageDescriptor = Object.getOwnPropertyDescriptor(globalThis, 'ImageData');
    let clockReads = 0;
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: { now: () => ++clockReads }
    });
    globalThis.ImageData = class {
      constructor(data, width, height) {
        Object.assign(this, { data, width, height });
      }
    };
    t.after(() => {
      if (performanceDescriptor)
        Object.defineProperty(globalThis, 'performance', performanceDescriptor);
      else delete globalThis.performance;
      if (imageDescriptor) Object.defineProperty(globalThis, 'ImageData', imageDescriptor);
      else delete globalThis.ImageData;
    });
    const first = renderer.render({ width: 24, height: 18, timeUs: 0 });
    assert.deepEqual(Array.from(first.data), original);
    assert.equal(first.rowBytes, 8);
    assert.equal(first.byteLength, 16);
    assert.notEqual(first.data.buffer, module.HEAPU8.buffer);
    assert.equal(first.timings, undefined);
    const next = original.map((value, index) => (index % 4 === 3 ? value : 255 - value));
    replacePixels(next);
    module.HEAPU8 = new Uint8Array(512);
    let cleared, painted;
    const canvas = {
      width: 24,
      height: 18,
      getContext: () => ({
        clearRect: (...args) => {
          cleared = args;
        },
        putImageData: (image, x, y) => {
          painted = { image, x, y, pixels: Array.from(image.data) };
        }
      })
    };
    const second = renderer.draw(canvas, { timeUs: 500000 });
    assert.deepEqual(painted.pixels, next);
    assert.equal(painted.image.data, second.data);
    assert.deepEqual([painted.x, painted.y], [11, 7]);
    assert.deepEqual(cleared, [0, 0, 24, 18]);
    assert.equal(clockReads, 0, 'Profiling disabled must not consult a timing clock');
    assert.notEqual(second.data.buffer, module.HEAPU8.buffer);
    assert.notEqual(second.data.buffer, first.data.buffer);
    assert.deepEqual(Array.from(first.data), original);
    module.HEAPU8.fill(0);
    renderer.dispose();
    assert.deepEqual(Array.from(first.data), original);
    assert.deepEqual(Array.from(second.data), next);
  });
}
