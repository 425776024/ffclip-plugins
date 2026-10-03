import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';

const bundle = await build({
  entryPoints: ['packages/render/gpu.mjs'],
  bundle: true,
  platform: 'node',
  format: 'esm',
  write: false,
  logLevel: 'silent'
});
let fixtureId = 0;
async function fixture(t) {
  let scopeError;
  const state = { rejectNextCopy: false, rejectNextScope: false, copies: [] };
  const texture = (width, height) => ({
    width,
    height,
    destroyed: false,
    destroy() {
      this.destroyed = true;
    },
    createView() {
      return {};
    }
  });
  const device = {
    lost: new Promise(() => {}),
    limits: { maxTextureDimension2D: 4096 },
    queue: {
      copyExternalImageToTexture(source, destination, extent) {
        state.copies.push({ source, destination, extent });
        if (state.rejectNextCopy) {
          state.rejectNextCopy = false;
          scopeError = { message: 'External source upload was rejected' };
        }
      },
      submit() {},
      async onSubmittedWorkDone() {},
      writeBuffer() {}
    },
    createSampler() {
      return {};
    },
    createShaderModule() {
      return {
        async getCompilationInfo() {
          return { messages: [] };
        }
      };
    },
    async createRenderPipelineAsync() {
      return {
        getBindGroupLayout() {
          return {};
        }
      };
    },
    createTexture({ size }) {
      return texture(size[0], size[1]);
    },
    createBuffer({ size }) {
      return { size, destroy() {} };
    },
    createBindGroup() {
      return {};
    },
    createCommandEncoder() {
      return {
        finish() {
          return {};
        },
        beginRenderPass() {
          return { end() {}, setPipeline() {}, setBindGroup() {}, draw() {} };
        }
      };
    },
    pushErrorScope() {},
    async popErrorScope() {
      const error =
        scopeError ||
        (state.rejectNextScope ? { message: 'Unrelated graph validation failed' } : undefined);
      scopeError = undefined;
      state.rejectNextScope = false;
      return error;
    }
  };
  const descriptor = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    value: {
      gpu: {
        async requestAdapter() {
          return {
            async requestDevice() {
              return device;
            }
          };
        },
        getPreferredCanvasFormat() {
          return 'bgra8unorm';
        }
      }
    }
  });
  t.after(() =>
    descriptor
      ? Object.defineProperty(globalThis, 'navigator', descriptor)
      : delete globalThis.navigator
  );
  const source = bundle.outputFiles[0].text + `\n// fixture ${++fixtureId}`;
  const { createGpuCompositor } = await import(
    'data:text/javascript;base64,' + Buffer.from(source).toString('base64')
  );
  const canvas = {
    width: 64,
    height: 64,
    getContext() {
      return {
        configure() {},
        unconfigure() {},
        getCurrentTexture() {
          return texture(canvas.width, canvas.height);
        }
      };
    }
  };
  const gpu = await createGpuCompositor(canvas);
  t.after(() => gpu.dispose());
  const meta = (id, rasterBounds) => ({
    inputKey: `input:${id}`,
    uploadIdentity: id,
    width: 64,
    height: 64,
    rasterBounds
  });
  const upload = (id, rasterBounds) =>
    gpu.upload({ width: 64, height: 64 }, meta(id).inputKey, id, rasterBounds, 1);
  const resident = (id, rasterBounds) => {
    const found = gpu.retainInputs([meta(id, rasterBounds)], true).has(meta(id).inputKey);
    return found;
  };
  return { gpu, state, upload, resident };
}

test('Failed upload validation invalidates only this frame uploads, preserving previously validated inputs', async (t) => {
  const f = await fixture(t);
  let base = f.gpu.begin();
  f.upload('previous');
  await f.gpu.finish(base);
  await f.gpu.abort();
  base = f.gpu.begin();
  f.state.rejectNextCopy = true;
  f.upload('failed');
  await assert.rejects(f.gpu.finish(base), /External source upload was rejected/);
  await f.gpu.abort();
  assert.equal(
    f.resident('failed'),
    false,
    'A rejected external copy cannot skip the next source render'
  );
  await f.gpu.abort();
  assert.equal(
    f.resident('previous'),
    true,
    'A new failure does not discard existing validated sources'
  );
  await f.gpu.abort();
});

test('ROI uploads copy only native pixels while residency validates logical size and exact origin', async (t) => {
  const f = await fixture(t),
    bounds = { x: 7, y: 11, width: 10, height: 20 };
  let base = f.gpu.begin();
  const input = f.upload('native', bounds);
  assert.equal(input.width, 10);
  assert.equal(input.height, 20);
  assert.deepEqual(f.state.copies.at(-1).source.origin, [7, 11]);
  assert.deepEqual(f.state.copies.at(-1).extent, [10, 20]);
  assert.equal(f.state.copies.at(-1).destination.premultipliedAlpha, true);
  await f.gpu.finish(base);
  assert.equal(f.resident('native', bounds), true);
  await f.gpu.abort();
  assert.equal(
    f.resident('native', { ...bounds, x: 8 }),
    false,
    'Same-sized crops at different origins cannot reuse pixels'
  );
  await f.gpu.abort();
  base = f.gpu.begin();
  f.state.rejectNextCopy = true;
  f.upload('failed', bounds);
  await assert.rejects(f.gpu.finish(base), /External source upload was rejected/);
  await f.gpu.abort();
  assert.equal(f.resident('failed', bounds), false);
  await f.gpu.abort();
  assert.equal(f.resident('native', bounds), true);
  await f.gpu.abort();
});

test('Pre-submit abort checks queued copy validation, while ordinary cancellation preserves valid uploads', async (t) => {
  const f = await fixture(t);
  f.gpu.begin();
  f.state.rejectNextCopy = true;
  f.upload('failed');
  await f.gpu.abort();
  assert.equal(f.resident('failed'), false);
  await f.gpu.abort();
  f.gpu.begin();
  f.upload('cancelled');
  await f.gpu.abort();
  assert.equal(
    f.resident('cancelled'),
    true,
    'Cancelling graph presentation does not erase a valid immutable copy'
  );
  await f.gpu.abort();
});

test('Cancellation after successful validation and later graph errors preserve reused source pixels', async (t) => {
  const f = await fixture(t);
  const base = f.gpu.begin();
  f.upload('valid');
  const cancel = new AbortController();
  cancel.abort();
  await assert.rejects(f.gpu.finish(base, cancel.signal), { name: 'AbortError' });
  await f.gpu.abort();
  assert.equal(f.resident('valid'), true);
  const next = f.gpu.begin();
  f.state.rejectNextScope = true;
  await assert.rejects(f.gpu.finish(next), /Unrelated graph validation failed/);
  await f.gpu.abort();
  assert.equal(
    f.resident('valid'),
    true,
    'An untouched cached source stays valid when another graph operation fails'
  );
  await f.gpu.abort();
});
