import { fullscreen, sdf, effects } from './gpu-shaders.mjs';
const textureUsage = 0x04 | 0x10 | 0x01 | 0x02; // TEXTURE_BINDING, RENDER_ATTACHMENT, COPY_SRC/DST
const finite = (x, min, max, name) => {
  if (!Number.isFinite(x) || x < min || x > max) throw new Error(`Invalid ${name}`);
  return x;
};
export async function createTextGpuEffects(canvas) {
  if (!globalThis.navigator?.gpu)
    throw new Error('WebGPU is unavailable; no CPU fallback is used by this GPU experiment');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('No WebGPU adapter');
  const device = await adapter.requestDevice();
  const context = canvas.getContext('webgpu');
  if (!context) {
    device.destroy();
    throw new Error('A fresh WebGPU canvas is required');
  }
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: 'premultiplied' });
  let disposed = false,
    lost = null,
    resources = null,
    currentMesh = null,
    generation = 0;
  const failures = [];
  device.addEventListener('uncapturederror', (event) => failures.push(event.error.message));
  device.lost.then((info) => {
    if (!disposed) lost = info.message || info.reason;
  });
  const check = () => {
    if (disposed) throw new Error('GPU renderer is disposed');
    if (lost) throw new Error(`WebGPU device lost: ${lost}`);
    if (failures.length) throw new Error(`WebGPU validation: ${failures.shift()}`);
  };
  const shader = async (code, label) => {
    const module = device.createShaderModule({ code, label });
    const info = await module.getCompilationInfo();
    const errors = info.messages.filter((m) => m.type === 'error');
    if (errors.length)
      throw new Error(
        `${label}: ${errors.map((e) => `${e.lineNum}:${e.linePos} ${e.message}`).join('\n')}`
      );
    return module;
  };
  try {
    const [sdfModule, effectModule] = await Promise.all([
      shader(fullscreen + sdf, 'VideoCut native SDF port'),
      shader(effects, 'VideoCut Metal effects port')
    ]);
    const depthFormat = 'depth24plus-stencil8';
    const baseDepth = { format: depthFormat, depthWriteEnabled: false, depthCompare: 'always' };
    const keep = { compare: 'always', failOp: 'keep', depthFailOp: 'keep', passOp: 'keep' };
    const vertexBuffers = (stride, types) => [
      {
        arrayStride: stride,
        attributes: types.map(([format, offset], shaderLocation) => ({
          format,
          offset,
          shaderLocation
        }))
      }
    ];
    const distancePipeline = await device.createRenderPipelineAsync({
      layout: 'auto',
      vertex: {
        module: sdfModule,
        entryPoint: 'distanceVertex',
        buffers: vertexBuffers(32, [
          ['float32x2', 0],
          ['float32x2', 8],
          ['float32x2', 16],
          ['float32', 24],
          ['float32', 28]
        ])
      },
      fragment: {
        module: sdfModule,
        entryPoint: 'distanceFragment',
        targets: [{ format: 'rgba8unorm' }]
      },
      primitive: { topology: 'triangle-list' },
      depthStencil: {
        ...baseDepth,
        depthWriteEnabled: true,
        depthCompare: 'less-equal',
        stencilFront: keep,
        stencilBack: keep
      }
    });
    const shapePipeline = await device.createRenderPipelineAsync({
      layout: 'auto',
      vertex: {
        module: sdfModule,
        entryPoint: 'shapeVertex',
        buffers: vertexBuffers(16, [
          ['float32x2', 0],
          ['float32x2', 8]
        ])
      },
      fragment: {
        module: sdfModule,
        entryPoint: 'shapeFragment',
        targets: [{ format: 'rgba8unorm', writeMask: 0 }]
      },
      primitive: { topology: 'triangle-list' },
      depthStencil: {
        ...baseDepth,
        stencilFront: { ...keep, passOp: 'increment-wrap' },
        stencilBack: { ...keep, passOp: 'decrement-wrap' }
      }
    });
    const inversePipeline = await device.createRenderPipelineAsync({
      layout: 'auto',
      vertex: { module: sdfModule, entryPoint: 'quad' },
      fragment: {
        module: sdfModule,
        entryPoint: 'inverseFragment',
        targets: [
          {
            format: 'rgba8unorm',
            blend: {
              color: { operation: 'add', srcFactor: 'one-minus-dst', dstFactor: 'zero' },
              alpha: { operation: 'add', srcFactor: 'one-minus-dst-alpha', dstFactor: 'zero' }
            }
          }
        ]
      },
      primitive: { topology: 'triangle-list' },
      depthStencil: {
        ...baseDepth,
        stencilFront: { compare: 'not-equal', passOp: 'zero', failOp: 'zero', depthFailOp: 'zero' },
        stencilBack: { compare: 'not-equal', passOp: 'zero', failOp: 'zero', depthFailOp: 'zero' }
      }
    });
    const bindLayout = device.createBindGroupLayout({
      entries: [
        { binding: 0, visibility: 2, texture: { sampleType: 'float' } },
        { binding: 1, visibility: 2, sampler: { type: 'filtering' } },
        { binding: 2, visibility: 2, buffer: { type: 'uniform', minBindingSize: 64 } },
        { binding: 3, visibility: 2, texture: { sampleType: 'float' } }
      ]
    });
    const pipelineLayout = device.createPipelineLayout({ bindGroupLayouts: [bindLayout] });
    const pipelines = {};
    for (const name of ['material', 'gaussian', 'threshold', 'glowY', 'glowBlend', 'copy']) {
      pipelines[name] = await device.createRenderPipelineAsync({
        layout: pipelineLayout,
        vertex: { module: effectModule, entryPoint: 'quad' },
        fragment: { module: effectModule, entryPoint: name, targets: [{ format: 'rgba8unorm' }] }
      });
    }
    pipelines.present = await device.createRenderPipelineAsync({
      layout: pipelineLayout,
      vertex: { module: effectModule, entryPoint: 'quad' },
      fragment: { module: effectModule, entryPoint: 'copy', targets: [{ format }] }
    });
    const sampler = device.createSampler({
      minFilter: 'linear',
      magFilter: 'linear',
      addressModeU: 'clamp-to-edge',
      addressModeV: 'clamp-to-edge'
    });
    const uniforms = Array.from({ length: 8 }, () =>
      device.createBuffer({ size: 64, usage: 0x40 | 0x08 })
    );
    const release = () => {
      if (resources) {
        for (const t of resources.textures) t.destroy();
        resources.depth.destroy();
        resources.distance?.destroy();
        resources.shape?.destroy();
        resources = null;
      }
    };
    const allocate = (width, height) => {
      release();
      canvas.width = width;
      canvas.height = height;
      const textures = Array.from({ length: 6 }, (_, i) =>
        device.createTexture({
          label: `VideoCut pass ${i}`,
          size: [width, height],
          format: 'rgba8unorm',
          usage: textureUsage
        })
      );
      resources = {
        width,
        height,
        textures,
        depth: device.createTexture({ size: [width, height], format: depthFormat, usage: 0x10 }),
        distance: null,
        shape: null,
        sdfReady: false
      };
    };
    const pass = (encoder, name, source, target, params, index, auxiliary = source) => {
      const values = new Float32Array(16);
      values.set(params);
      device.queue.writeBuffer(uniforms[index], 0, values);
      const group = device.createBindGroup({
        layout: bindLayout,
        entries: [
          { binding: 0, resource: source.createView() },
          { binding: 1, resource: sampler },
          { binding: 2, resource: { buffer: uniforms[index] } },
          { binding: 3, resource: auxiliary.createView() }
        ]
      });
      const p = encoder.beginRenderPass({
        colorAttachments: [
          { view: target.createView(), clearValue: [0, 0, 0, 0], loadOp: 'clear', storeOp: 'store' }
        ]
      });
      p.setPipeline(pipelines[name]);
      p.setBindGroup(0, group);
      p.draw(3);
      p.end();
    };
    const sdfPass = (encoder) => {
      const p = encoder.beginRenderPass({
        colorAttachments: [
          {
            view: resources.textures[0].createView(),
            clearValue: [0, 0, 0, 0],
            loadOp: 'clear',
            storeOp: 'store'
          }
        ],
        depthStencilAttachment: {
          view: resources.depth.createView(),
          depthClearValue: 1,
          depthLoadOp: 'clear',
          depthStoreOp: 'discard',
          stencilClearValue: 0,
          stencilLoadOp: 'clear',
          stencilStoreOp: 'discard'
        }
      });
      p.setPipeline(distancePipeline);
      p.setVertexBuffer(0, resources.distance);
      p.draw(currentMesh.distanceVertices.length / 8);
      p.setPipeline(shapePipeline);
      p.setVertexBuffer(0, resources.shape);
      p.setStencilReference(0);
      p.draw(currentMesh.shapeVertices.length / 4);
      p.setPipeline(inversePipeline);
      p.draw(3);
      p.end();
      resources.sdfReady = true;
    };
    const api = {
      profile: 'webgpu-effects-experimental-v1',
      adapterInfo: {
        vendor: adapter.info?.vendor || '',
        architecture: adapter.info?.architecture || '',
        description: adapter.info?.description || '',
        isFallbackAdapter: adapter.info?.isFallbackAdapter ?? adapter.isFallbackAdapter ?? null
      },
      setMesh(mesh) {
        check();
        finite(mesh.width, 1, 2048, 'width');
        finite(mesh.height, 1, 2048, 'height');
        finite(mesh.range, 1, 128, 'range');
        if (
          !Number.isInteger(mesh.width) ||
          !Number.isInteger(mesh.height) ||
          !(mesh.distanceVertices instanceof Float32Array) ||
          !(mesh.shapeVertices instanceof Float32Array) ||
          mesh.distanceVertices.length % 24 ||
          mesh.shapeVertices.length % 12 ||
          !mesh.distanceVertices.length ||
          !mesh.shapeVertices.length ||
          mesh.distanceVertices.byteLength + mesh.shapeVertices.byteLength > 64 * 1024 * 1024 ||
          !mesh.distanceVertices.every(Number.isFinite) ||
          !mesh.shapeVertices.every(Number.isFinite)
        )
          throw new Error('Invalid SDF vertices');
        allocate(mesh.width, mesh.height);
        currentMesh = { ...mesh };
        generation++;
        for (const [key, vertices] of [
          ['distance', mesh.distanceVertices],
          ['shape', mesh.shapeVertices]
        ]) {
          resources[key] = device.createBuffer({ size: vertices.byteLength, usage: 0x20 | 0x08 });
          device.queue.writeBuffer(resources[key], 0, vertices);
        }
      },
      // Post-effect probes can upload the exact same premultiplied RGBA input
      // used by Metal. This does not silently replace the SDF geometry path.
      setSource({ width, height, data }) {
        check();
        finite(width, 1, 2048, 'width');
        finite(height, 1, 2048, 'height');
        if (
          !Number.isInteger(width) ||
          !Number.isInteger(height) ||
          !(data instanceof Uint8Array) ||
          data.length !== width * height * 4
        )
          throw new Error('Invalid RGBA source');
        allocate(width, height);
        currentMesh = null;
        generation++;
        device.queue.writeTexture(
          { texture: resources.textures[1] },
          data,
          { bytesPerRow: width * 4 },
          { width, height }
        );
      },
      render({
        effect = 'sdf',
        fill = [0.92, 0.08, 0.22, 1],
        stroke = [1, 1, 1, 1],
        strokeWidth = 18,
        radius = 12,
        sigma = 6,
        exposure = 1.5,
        threshold = 0.15,
        glowColor = [1, 0.65, 0.3],
        displayGlow = false
      } = {}) {
        check();
        if (!resources) throw new Error('Set an SDF mesh or RGBA source first');
        if (!['sdf', 'gaussian', 'soft-glow'].includes(effect))
          throw new Error('Unsupported experimental effect');
        finite(strokeWidth, 0, 128, 'stroke width');
        finite(radius, 0, 128, 'radius');
        finite(sigma, 0.01, 128, 'sigma');
        finite(exposure, 0, 8, 'exposure');
        finite(threshold, 0, 0.99, 'threshold');
        for (const [name, color, size] of [
          ['fill', fill, 4],
          ['stroke', stroke, 4],
          ['glow color', glowColor, 3]
        ]) {
          if (!Array.isArray(color) || color.length !== size) throw new Error(`Invalid ${name}`);
          color.forEach((c) => finite(c, 0, 1, name));
        }
        const start = performance.now();
        const encoder = device.createCommandEncoder();
        const t = resources.textures;
        if (currentMesh) {
          if (!resources.sdfReady) sdfPass(encoder);
          pass(
            encoder,
            'material',
            t[0],
            t[1],
            [currentMesh.range, strokeWidth, 0.6, 0, ...fill, ...stroke],
            0
          );
        }
        let result = t[1];
        const gaussianParams = (axis, glow = false) => [
          radius,
          sigma / (axis ? resources.height : resources.width),
          1 / (axis ? resources.height : resources.width),
          axis,
          0,
          2.2,
          1,
          glow ? exposure : 1
        ];
        if (effect === 'gaussian') {
          pass(encoder, 'gaussian', t[1], t[2], gaussianParams(0), 1);
          pass(encoder, 'gaussian', t[2], t[5], gaussianParams(1), 2);
          result = t[5];
        } else if (effect === 'soft-glow') {
          pass(encoder, 'threshold', t[1], t[2], [0, threshold, 1, 1, 0], 1);
          pass(encoder, 'gaussian', t[2], t[3], gaussianParams(0), 2);
          pass(encoder, 'glowY', t[3], t[4], gaussianParams(1, true), 3);
          pass(
            encoder,
            'glowBlend',
            t[1],
            t[5],
            [exposure, Number(displayGlow), 0, 0, ...glowColor, 0],
            4,
            t[4]
          );
          result = t[5];
        }
        pass(encoder, 'present', result, context.getCurrentTexture(), [], 6);
        device.queue.submit([encoder.finish()]);
        resources.result = result;
        return {
          profile: api.profile,
          effect,
          width: resources.width,
          height: resources.height,
          submissionMs: performance.now() - start,
          generation,
          source: currentMesh ? 'sdk-sdf-mesh' : 'uploaded-premultiplied-rgba',
          note: 'CPU submission time only; await completed() for queue completion.'
        };
      },
      async completed() {
        check();
        await device.queue.onSubmittedWorkDone();
        check();
      },
      async readPixels({ distanceField = false } = {}) {
        check();
        if (!resources?.result) throw new Error('Render before readback');
        if (distanceField && !currentMesh) throw new Error('A distance field requires an SDF mesh');
        const { width, height } = resources;
        const stride = Math.ceil((width * 4) / 256) * 256;
        const buffer = device.createBuffer({ size: stride * height, usage: 0x01 | 0x08 });
        try {
          const encoder = device.createCommandEncoder();
          encoder.copyTextureToBuffer(
            { texture: distanceField ? resources.textures[0] : resources.result },
            { buffer, bytesPerRow: stride },
            { width, height }
          );
          device.queue.submit([encoder.finish()]);
          await buffer.mapAsync(1);
          check();
          const mapped = new Uint8Array(buffer.getMappedRange());
          const data = new Uint8Array(width * height * 4);
          for (let y = 0; y < height; y++)
            data.set(mapped.subarray(y * stride, y * stride + width * 4), y * width * 4);
          return { width, height, data, alphaMode: 'premultiplied' };
        } finally {
          buffer.destroy();
        }
      },
      dispose() {
        if (disposed) return;
        disposed = true;
        release();
        uniforms.forEach((b) => b.destroy());
        context.unconfigure();
        device.destroy();
        currentMesh = null;
      }
    };
    return api;
  } catch (error) {
    context.unconfigure();
    device.destroy();
    throw error;
  }
}
