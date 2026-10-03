import { effects } from './gpu-shaders.mjs';
import { radial, sprite } from './composition-shaders.mjs';

/** Output-canvas compositor for the explicitly admitted studio templates. */
export async function createCompositionGpu(canvas, width, height, shared = null) {
  if (!navigator.gpu) throw new Error('These complex templates require WebGPU');
  const adapter = shared?.adapter || (await navigator.gpu.requestAdapter());
  if (!adapter) throw new Error('No WebGPU adapter');
  const device = shared?.device || (await adapter.requestDevice());
  const context = canvas.getContext('webgpu');
  if (!context) {
    if (!shared) device.destroy();
    throw new Error('Use a fresh WebGPU canvas');
  }
  const format = navigator.gpu.getPreferredCanvasFormat();
  canvas.width = width;
  canvas.height = height;
  context.configure({ device, format, alphaMode: 'premultiplied' });
  let disposed = false,
    failure = null,
    textures = new Map(),
    buffers = [];
  const allocations = new Map();
  const allocate = (bytes, create) => {
    const token = {};
    shared?.memory?.retainExternal(token, bytes);
    try {
      const value = create();
      allocations.set(value, token);
      return value;
    } catch (error) {
      shared?.memory?.releaseExternal(token);
      throw error;
    }
  };
  const release = (value) => {
    if (!value) return;
    value.destroy();
    const token = allocations.get(value);
    if (token) shared?.memory?.releaseExternal(token);
    allocations.delete(value);
  };
  const gpuError = (e) => {
    failure = e.error.message;
  };
  device.addEventListener('uncapturederror', gpuError);
  device.lost.then((info) => {
    if (!disposed) failure = info.message || 'GPU device lost';
  });
  const check = () => {
    if (disposed || failure) throw new Error(failure || 'Composition is disposed');
  };
  const module = async (code) => {
    const m = device.createShaderModule({ code });
    const c = await m.getCompilationInfo();
    const errors = c.messages.filter((x) => x.type === 'error');
    if (errors.length) throw new Error(errors.map((x) => x.message).join('\n'));
    return m;
  };
  try {
    const buildPipelines = async () => {
      const [fx, sp] = await Promise.all([module(effects + radial), module(sprite)]);
      const fxLayout = device.createBindGroupLayout({
        entries: [
          { binding: 0, visibility: 2, texture: {} },
          { binding: 1, visibility: 2, sampler: {} },
          { binding: 2, visibility: 2, buffer: { minBindingSize: 64 } },
          { binding: 3, visibility: 2, texture: {} }
        ]
      });
      const pipelineLayout = device.createPipelineLayout({ bindGroupLayouts: [fxLayout] });
      const pipelines = {};
      for (const name of [
        'copy',
        'threshold',
        'gaussian',
        'glowY',
        'glowBlend',
        'radialBlur',
        'present'
      ])
        pipelines[name] = await device.createRenderPipelineAsync({
          layout: pipelineLayout,
          vertex: { module: fx, entryPoint: 'quad' },
          fragment: {
            module: fx,
            entryPoint: name === 'present' ? 'copy' : name,
            targets: [{ format: name === 'present' ? format : 'rgba8unorm' }]
          }
        });
      const spriteLayout = device.createBindGroupLayout({
        entries: [
          { binding: 0, visibility: 2, texture: {} },
          { binding: 1, visibility: 2, sampler: {} },
          { binding: 2, visibility: 3, buffer: { minBindingSize: 48 } }
        ]
      });
      const spritePipeline = await device.createRenderPipelineAsync({
        layout: device.createPipelineLayout({ bindGroupLayouts: [spriteLayout] }),
        vertex: { module: sp, entryPoint: 'vertex' },
        fragment: {
          module: sp,
          entryPoint: 'fragment',
          targets: [
            {
              format: 'rgba8unorm',
              blend: {
                color: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' },
                alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' }
              }
            }
          ]
        }
      });
      return { fxLayout, pipelines, spriteLayout, spritePipeline };
    };
    const prepared = shared
      ? (shared.compositionPipelines ||= buildPipelines().catch((error) => {
          shared.compositionPipelines = undefined;
          throw error;
        }))
      : buildPipelines();
    const { fxLayout, pipelines, spriteLayout, spritePipeline } = await prepared;
    const sampler = device.createSampler({ minFilter: 'linear', magFilter: 'linear' });
    const texture = (key, w = width, h = height) => {
      let t = textures.get(key);
      if (!t || t.width !== w || t.height !== h) {
        release(t);
        t = allocate(w * h * 4, () =>
          device.createTexture({ size: [w, h], format: 'rgba8unorm', usage: 0x17 })
        );
        textures.set(key, t);
      }
      return t;
    };
    const uniform = (index, values) => {
      const b = (buffers[index] ??= allocate(64, () =>
        device.createBuffer({ size: 64, usage: 0x48 })
      ));
      const data = new Float32Array(16);
      data.set(values);
      device.queue.writeBuffer(b, 0, data);
      return b;
    };
    const pass = (encoder, name, source, target, values, index, auxiliary = source) => {
      const group = device.createBindGroup({
        layout: fxLayout,
        entries: [
          { binding: 0, resource: source.createView() },
          { binding: 1, resource: sampler },
          { binding: 2, resource: { buffer: uniform(index, values) } },
          { binding: 3, resource: auxiliary.createView() }
        ]
      });
      const p = encoder.beginRenderPass({
        colorAttachments: [
          { view: target.createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] }
        ]
      });
      p.setPipeline(pipelines[name]);
      p.setBindGroup(0, group);
      p.draw(3);
      p.end();
    };
    const api = {
      adapter: {
        vendor: adapter.info?.vendor,
        architecture: adapter.info?.architecture,
        isFallbackAdapter: adapter.info?.isFallbackAdapter ?? false
      },
      render(frame, decorations, { background = true, postEffects = true } = {}) {
        check();
        const encoder = device.createCommandEncoder();
        const text = texture('text', frame.width, frame.height);
        device.queue.writeTexture(
          { texture: text },
          frame.data,
          { bytesPerRow: frame.width * 4 },
          { width: frame.width, height: frame.height }
        );
        const sprites = [];
        if (background)
          for (const [i, d] of decorations.entries()) {
            if (d.opacity <= 0) continue;
            const source = texture('video' + i, d.source.width, d.source.height);
            device.queue.copyExternalImageToTexture({ source: d.source }, { texture: source }, [
              d.source.width,
              d.source.height
            ]);
            // Native affine maps texel centers, while this quad maps texel edges.
            const [a, b, c, e, x, y] = d.affine;
            sprites.push({
              source,
              values: [
                a,
                b,
                c,
                e,
                x + 0.5 - (a + c) * 0.5,
                y + 0.5 - (b + e) * 0.5,
                d.width,
                d.height,
                width,
                height,
                d.opacity,
                1
              ]
            });
          }
        sprites.push({
          source: text,
          values: [
            1,
            0,
            0,
            1,
            frame.originX,
            frame.originY,
            frame.width,
            frame.height,
            width,
            height,
            1,
            0
          ]
        });
        const scene = texture('scene');
        const p = encoder.beginRenderPass({
          colorAttachments: [
            {
              view: scene.createView(),
              loadOp: 'clear',
              storeOp: 'store',
              clearValue: [0, 0, 0, 0]
            }
          ]
        });
        p.setPipeline(spritePipeline);
        for (const [i, s] of sprites.entries()) {
          p.setBindGroup(
            0,
            device.createBindGroup({
              layout: spriteLayout,
              entries: [
                { binding: 0, resource: s.source.createView() },
                { binding: 1, resource: sampler },
                { binding: 2, resource: { buffer: uniform(16 + i, s.values) } }
              ]
            })
          );
          p.draw(6);
        }
        p.end();
        let result = scene;
        const effect = postEffects ? frame.compositionPlan?.effects[0] : null;
        if (effect?.kind === 'soft-glow') {
          const c = effect;
          const threshold = texture('threshold');
          const w = c.glowWidth,
            h = c.glowHeight;
          const down = texture('down', w, h),
            x = texture('blurX', w, h),
            y = texture('blurY', w, h),
            out = texture('output');
          pass(
            encoder,
            'threshold',
            scene,
            threshold,
            [c.thresholdType, c.thresholdLow, c.thresholdHigh, c.thresholdSmooth, c.grayScale],
            0
          );
          pass(encoder, 'copy', threshold, down, [], 1);
          pass(
            encoder,
            'gaussian',
            down,
            x,
            [c.sampleCount, c.sigmaX, c.stepX, 0, 0, 2.2, 1, 1],
            2
          );
          pass(
            encoder,
            'glowY',
            x,
            y,
            [c.sampleCount, c.sigmaY, c.stepY, 1, 0, 2.2, 1, c.exposure],
            3
          );
          pass(
            encoder,
            'glowBlend',
            scene,
            out,
            [c.exposure, c.displayGlow, 0, 0, ...c.glowColor, 0],
            4,
            y
          );
          result = out;
        } else if (effect?.kind === 'radial-blur') {
          const c = effect;
          result = texture('output');
          pass(
            encoder,
            'radialBlur',
            scene,
            result,
            [
              c.intensity,
              c.blurType,
              c.quality,
              c.weightDecay,
              ...c.center,
              c.dither,
              c.borderType,
              c.blurAlpha,
              c.inverseGammaCorrection,
              c.gamma,
              c.lightIntensity,
              c.lightTransferMode
            ],
            0
          );
        } else if (effect) throw new Error('Unsupported composition effect');
        pass(encoder, 'present', result, context.getCurrentTexture(), [], 7);
        device.queue.submit([encoder.finish()]);
        api.result = result;
      },
      async completed() {
        check();
        await device.queue.onSubmittedWorkDone();
        check();
      },
      async readPixels() {
        check();
        if (!api.result) throw new Error('Render before readback');
        const stride = Math.ceil((width * 4) / 256) * 256,
          b = allocate(stride * height, () =>
            device.createBuffer({ size: stride * height, usage: 0x09 })
          );
        try {
          const e = device.createCommandEncoder();
          e.copyTextureToBuffer(
            { texture: api.result },
            { buffer: b, bytesPerRow: stride },
            { width, height }
          );
          device.queue.submit([e.finish()]);
          await b.mapAsync(1);
          check();
          const source = new Uint8Array(b.getMappedRange()),
            data = new Uint8Array(width * height * 4);
          for (let y = 0; y < height; y++)
            data.set(source.subarray(y * stride, y * stride + width * 4), y * width * 4);
          return { width, height, data };
        } finally {
          release(b);
        }
      },
      dispose() {
        if (disposed) return;
        disposed = true;
        for (const t of textures.values()) release(t);
        for (const b of buffers) release(b);
        device.removeEventListener('uncapturederror', gpuError);
        context.unconfigure();
        if (!shared) device.destroy();
      }
    };
    return api;
  } catch (error) {
    for (const resource of [...allocations.keys()]) release(resource);
    device.removeEventListener('uncapturederror', gpuError);
    context.unconfigure();
    if (!shared) device.destroy();
    throw error;
  }
}
