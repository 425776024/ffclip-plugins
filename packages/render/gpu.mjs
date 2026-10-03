import { RenderResourceCache, RenderMemoryPool, RenderBudgetError } from './resource-cache';
import { localEffectGeometry, renderIdentity } from './graph';
// Shared per execution context. Preview/export/template players borrow this device;
// disposing a composition releases only its own textures and buffers.
let shared;
export function sharedGpu() {
  return (shared ||= (async () => {
    if (!globalThis.navigator?.gpu) return null;
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) return null;
    const device = await adapter.requestDevice();
    device.lost.then(() => {
      shared = undefined;
    });
    return { device, adapter, pipelines: new Map(), memory: new RenderMemoryPool() };
  })().catch((error) => {
    shared = undefined;
    throw error;
  }));
}

const shader = `
struct Params { a: vec4f, b: vec4f, c: vec4f, d: vec4f, e: vec4f, f: vec4f, g: vec4f, h: vec4f };
@group(0) @binding(0) var first: texture_2d<f32>;
@group(0) @binding(1) var second: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;
@group(0) @binding(3) var<uniform> p: Params;
struct Out { @builtin(position) position: vec4f, @location(0) uv: vec2f };
@vertex fn vertex(@builtin(vertex_index) i: u32) -> Out {
  let pos = array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));
  var o:Out; o.position=vec4f(pos[i],0,1); o.uv=vec2f((pos[i].x+1)*.5,(1-pos[i].y)*.5); return o;
}
fn sample1(uv:vec2f)->vec4f { return textureSampleLevel(first,samp,uv,0); }
fn sample2(uv:vec2f)->vec4f { return textureSampleLevel(second,samp,uv,0); }
fn inside(uv:vec2f)->bool { return all(uv>=vec2f(0)) && all(uv<=vec2f(1)); }
@fragment fn fragment(o:Out)->@location(0) vec4f {
 let uv=o.uv; let mode=u32(p.a.x);
 if(mode==0u){return sample1(uv);}
 if(mode==1u){ // inverse affine from project canvas to cropped source UV
   let q=vec2f(dot(p.b.xyz,vec3f(uv,1)),dot(p.c.xyz,vec3f(uv,1)));
   if(!inside(q)){return vec4f(0);}
   return sample1(p.d.xy+q*p.d.zw)*p.a.y;
 }
 if(mode==2u){ // Porter-Duff source-over with optional blend function
   let b=sample1(uv); let s=sample2(uv);
   let cb=b.rgb/max(b.a,0.000001); let cs=s.rgb/max(s.a,0.000001);
   var blend=cs;
   if(p.a.y==1){blend=cb*cs;}
   if(p.a.y==2){blend=1-(1-cb)*(1-cs);}
   if(p.a.y==3){blend=select(2*cb*cs,1-2*(1-cb)*(1-cs),cb>vec3f(.5));}
   if(p.a.y==4){blend=min(cb,cs);}
   if(p.a.y==5){blend=max(cb,cs);}
   return vec4f((1-s.a)*b.rgb+(1-b.a)*s.rgb+b.a*s.a*blend,s.a+b.a*(1-s.a));
 }
 if(mode==3u){ // bounded separable Gaussian, including alpha
   let sigma=max(p.a.y,.01); let radius=min(i32(ceil(p.a.z)),64);
   var sum=vec4f(0); var weight=0.;
   for(var i:i32=-64;i<=64;i++){if(abs(i)<=radius){let w=exp(-f32(i*i)/(2*sigma*sigma));sum+=sample1(uv+p.b.xy*f32(i))*w;weight+=w;}}
   return sum/max(weight,.000001);
 }
 if(mode==4u){let c=sample1(uv);let rgb=c.rgb/max(c.a,.000001);let l=max(rgb.r,max(rgb.g,rgb.b));return c*smoothstep(p.a.y,min(1.,p.a.y+.15),l);}
 if(mode==5u){let b=sample1(uv);let g=clamp(sample2(uv)*p.a.y,vec4f(0),vec4f(1));let rgb=1-(1-b.rgb)*(1-g.rgb);return vec4f(rgb,b.a+g.a*(1-b.a));}
 if(mode==6u){ // 3D LUT packed in a 2D strip; trilinear blue-axis sampling
   let c=sample1(uv);let rgb=clamp(c.rgb/max(c.a,.000001),vec3f(0),vec3f(1));
   let n=p.a.z;let blue=rgb.b*(n-1);let low=floor(blue);let high=min(n-1,low+1);
   let y=(rgb.g*(n-1)+.5)/n;
   let a=sample2(vec2f((low*n+rgb.r*(n-1)+.5)/(n*n),y)).rgb;
   let b=sample2(vec2f((high*n+rgb.r*(n-1)+.5)/(n*n),y)).rgb;
   return vec4f(mix(rgb,mix(a,b,fract(blue)),p.a.y)*c.a,c.a);
 }
 if(mode==7u){
   let t=clamp(p.a.y,0.,1.);if(t<=0){return sample1(uv);}if(t>=1){return sample2(uv);}
   let style=u32(p.a.z);let dir=u32(p.a.w);
   if(style==1u){let color=vec4f(p.b.rgb*p.b.a,p.b.a);return select(mix(sample1(uv),color,smoothstep(0.,.5,t)),mix(color,sample2(uv),smoothstep(.5,1.,t)),t>=.5);}
   var v=uv.x;if(dir==1u){v=1-uv.x;}if(dir==2u){v=uv.y;}if(dir==3u){v=1-uv.y;}
   if(style==2u){return mix(sample1(uv),sample2(uv),1-smoothstep(t-p.c.x,t+p.c.x,v));}
   if(style==3u){var axis=vec2f(1,0);if(dir==1u){axis=vec2f(-1,0);}if(dir==2u){axis=vec2f(0,1);}if(dir==3u){axis=vec2f(0,-1);}let b=uv+axis*(1-t);let a=uv-axis*t;if(inside(b)){return sample2(b);}if(inside(a)){return sample1(a);}return mix(sample1(uv),sample2(uv),t);}
   return mix(sample1(uv),sample2(uv),t);
 }
 return vec4f(1,0,1,1);
}`;

export async function createGpuCompositor(canvas, options = {}) {
  const gpu = await sharedGpu();
  if (!gpu) return null;
  const { device, memory } = gpu;
  const context = canvas.getContext('webgpu');
  if (!context) return null;
  let renderWidth = canvas.width,
    renderHeight = canvas.height;
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: 'premultiplied' });
  let failed = '',
    disposed = false,
    textureBytes = 0,
    revision = 0;
  device.lost.then((info) => {
    failed = info.message || 'GPU device lost';
  });
  const contents = new Map(),
    pending = new Map(),
    touchedUploads = new Set(),
    keys = new WeakMap();
  const resources = new RenderResourceCache(
    options.textureBudgetBytes || 512 * 1048576,
    (value, key) => {
      if (!key.startsWith('buffer:')) textureBytes -= value.width * value.height * 4;
      value.destroy();
      contents.delete(key);
      pending.delete(key);
    }
  );
  memory.register(resources, resources.budgetBytes);
  const sampler = device.createSampler({ minFilter: 'linear', magFilter: 'linear' });
  const check = () => {
    if (disposed || failed) throw new Error(failed || 'Compositor disposed');
  };
  async function pipeline(targetFormat) {
    const key = 'scene-v2:' + targetFormat;
    if (!gpu.pipelines.has(key)) {
      const promise = (async () => {
        const module = device.createShaderModule({ code: shader });
        const info = await module.getCompilationInfo();
        const errors = info.messages.filter((m) => m.type === 'error');
        if (errors.length) throw new Error(errors.map((m) => m.message).join('\n'));
        return device.createRenderPipelineAsync({
          layout: 'auto',
          vertex: { module, entryPoint: 'vertex' },
          fragment: { module, entryPoint: 'fragment', targets: [{ format: targetFormat }] },
          primitive: { topology: 'triangle-list' }
        });
      })().catch((error) => {
        gpu.pipelines.delete(key);
        throw error;
      });
      gpu.pipelines.set(key, promise);
    }
    const value = gpu.pipelines.get(key);
    gpu.pipelines.delete(key);
    gpu.pipelines.set(key, value);
    while (gpu.pipelines.size > 8) gpu.pipelines.delete(gpu.pipelines.keys().next().value);
    return value;
  }
  let renderPipeline, presentPipeline;
  try {
    [renderPipeline, presentPipeline] = await Promise.all([
      pipeline('rgba8unorm'),
      pipeline(format)
    ]);
  } catch (error) {
    memory.unregister(resources);
    context.unconfigure();
    throw error;
  }
  let encoder,
    passIndex = 0,
    frameScope = false,
    inputsProtected = false,
    submitted = false,
    resultTexture,
    resultIdentity = '',
    workingSetBytes = 0;
  let uploadCount = 0,
    uploadCacheHits = 0,
    effectHits = 0,
    layerHits = 0,
    compositeHits = 0,
    transitionHits = 0;
  function texture(key, width = renderWidth, height = renderHeight) {
    let value = resources.get(key);
    if (value && (value.width !== width || value.height !== height)) {
      resources.delete(key);
      value = undefined;
    }
    if (!value) {
      const bytes = width * height * 4;
      if (
        width > device.limits.maxTextureDimension2D ||
        height > device.limits.maxTextureDimension2D
      )
        throw new RenderBudgetError(Math.max(bytes, memory.bytes), memory.budgetBytes);
      memory.reserve(bytes);
      resources.reserve(bytes);
      value = device.createTexture({ size: [width, height], format: 'rgba8unorm', usage: 0x17 });
      resources.set(key, value, bytes);
      memory.recordUsage();
      keys.set(value, key);
      textureBytes += bytes;
    }
    return value;
  }
  function buffer(key, size, usage) {
    let value = resources.get(key);
    if (value && value.size !== size) {
      resources.delete(key);
      value = undefined;
    }
    if (!value) {
      memory.reserve(size);
      resources.reserve(size);
      value = device.createBuffer({ size, usage });
      resources.set(key, value, size);
      memory.recordUsage();
      keys.set(value, key);
    }
    return value;
  }
  const identity = (value) => pending.get(keys.get(value)) || contents.get(keys.get(value)) || '';
  const signature = renderIdentity;
  const uploadSignature = (id, bounds) => (bounds ? signature(id, bounds) : signature(id));
  async function validateFrameScope() {
    const invalidateUploads = () => {
      // A failed external copy must never prove source residency next frame.
      // Previously validated inputs were not touched and remain reusable.
      for (const key of touchedUploads) resources.delete(key);
    };
    try {
      const error = await device.popErrorScope();
      if (error) invalidateUploads();
      return error;
    } catch (error) {
      invalidateUploads();
      throw error;
    } finally {
      touchedUploads.clear();
    }
  }
  function residentInput(source, pin = true) {
    const value = resources.get(source.inputKey, false);
    if (
      !value ||
      value.width !== (source.rasterBounds?.width || source.width) ||
      value.height !== (source.rasterBounds?.height || source.height) ||
      contents.get(source.inputKey) !== uploadSignature(source.uploadIdentity, source.rasterBounds)
    )
      return;
    return pin ? resources.get(source.inputKey) : value;
  }
  function hit(category) {
    if (category === 'effect') effectHits++;
    else if (category === 'layer') layerHits++;
    else if (category === 'transition') transitionHits++;
    else compositeHits++;
  }
  function node(category, key, id, width, height, draw) {
    const current = resources.get(key);
    if (
      current &&
      current.width === width &&
      current.height === height &&
      identity(current) === id
    ) {
      hit(category);
      return current;
    }
    const target = texture(key, width, height);
    draw(target);
    pending.set(key, id);
    return target;
  }
  function pass(mode, source, auxiliary, target, values = [], present = false) {
    const pLine = present ? presentPipeline : renderPipeline;
    const uniform = buffer('buffer:uniform:' + passIndex++, 128, 0x48),
      data = new Float32Array(32);
    data[0] = mode;
    data.set(values, 1);
    device.queue.writeBuffer(uniform, 0, data);
    const bind = device.createBindGroup({
      layout: pLine.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: source.createView() },
        { binding: 1, resource: (auxiliary || source).createView() },
        { binding: 2, resource: sampler },
        { binding: 3, resource: { buffer: uniform } }
      ]
    });
    const p = encoder.beginRenderPass({
      colorAttachments: [
        {
          view: target.createView(),
          loadOp: 'clear',
          storeOp: 'store',
          clearValue: { r: 0, g: 0, b: 0, a: 0 }
        }
      ]
    });
    p.setPipeline(pLine);
    p.setBindGroup(0, bind);
    p.draw(3);
    p.end();
  }
  function cachedPass(
    category,
    key,
    mode,
    source,
    auxiliary,
    values = [],
    width = renderWidth,
    height = renderHeight
  ) {
    const id = signature(
      mode,
      identity(source),
      auxiliary ? identity(auxiliary) : null,
      values,
      width,
      height
    );
    return node(category, key, id, width, height, (target) =>
      pass(mode, source, auxiliary, target, values)
    );
  }
  function gaussian(source, sigma, key, radius = Math.ceil(sigma * 3)) {
    const width = source.width,
      height = source.height,
      factor = Math.max(1, Math.ceil(radius / 64));
    const w = Math.max(1, Math.ceil(width / factor)),
      h = Math.max(1, Math.ceil(height / factor));
    let input = source;
    if (factor > 1) input = cachedPass('effect', key + ':down', 0, source, null, [], w, h);
    const x = cachedPass(
      'effect',
      key + ':x',
      3,
      input,
      null,
      [Math.max(0.01, (sigma * w) / width), Math.ceil((radius * w) / width), 0, 1 / w, 0],
      w,
      h
    );
    const y = cachedPass(
      'effect',
      key + ':y',
      3,
      x,
      null,
      [Math.max(0.01, (sigma * h) / height), Math.ceil((radius * h) / height), 0, 0, 1 / h],
      w,
      h
    );
    return factor === 1 ? y : cachedPass('effect', key + ':up', 0, y, null, [], width, height);
  }
  const resetStats = () => {
    uploadCount =
      uploadCacheHits =
      effectHits =
      layerHits =
      compositeHits =
      transitionHits =
      passIndex =
        0;
  };
  return {
    backend: 'webgpu',
    device,
    retainInputs(sources, preferNative = false) {
      check();
      if (!preferNative) resources.resetPriorities();
      // Protect every reused input before other source renderers allocate from
      // the shared budget, and before the background or new layer targets exist.
      resources.beginFrame();
      inputsProtected = true;
      const retained = new Set();
      for (const source of sources) if (residentInput(source)) retained.add(source.inputKey);
      return retained;
    },
    begin(background = [0, 0, 0, 1], width = canvas.width, height = canvas.height) {
      renderWidth = width;
      renderHeight = height;
      check();
      if (!inputsProtected) resources.beginFrame();
      inputsProtected = false;
      pending.clear();
      resetStats();
      submitted = false;
      device.pushErrorScope('validation');
      frameScope = true;
      encoder = device.createCommandEncoder();
      return node(
        'composite',
        'background',
        signature(background, renderWidth, renderHeight),
        renderWidth,
        renderHeight,
        (target) => {
          const pass = encoder.beginRenderPass({
            colorAttachments: [
              {
                view: target.createView(),
                loadOp: 'clear',
                storeOp: 'store',
                clearValue: {
                  r: background[0] * background[3],
                  g: background[1] * background[3],
                  b: background[2] * background[3],
                  a: background[3]
                }
              }
            ]
          });
          pass.end();
        }
      );
    },
    upload(source, key = 'input', sourceIdentity, rasterBounds, priority = 0) {
      check();
      const w = rasterBounds?.width || source.displayWidth || source.videoWidth || source.width,
        h = rasterBounds?.height || source.displayHeight || source.videoHeight || source.height;
      const t = texture(key, w, h),
        id =
          sourceIdentity === undefined
            ? 'uncached:' + ++revision
            : uploadSignature(sourceIdentity, rasterBounds);
      resources.setPriority(key, priority);
      if (contents.get(key) === id) {
        uploadCacheHits++;
        return t;
      }
      device.queue.copyExternalImageToTexture(
        { source, ...(rasterBounds ? { origin: [rasterBounds.x, rasterBounds.y] } : {}) },
        { texture: t, premultipliedAlpha: true },
        [w, h]
      );
      touchedUploads.add(key);
      uploadCount++;
      contents.set(key, id);
      return t;
    },
    reuseInput(source) {
      check();
      const value = residentInput(source);
      if (!value) throw new Error('Cached source texture is no longer resident');
      uploadCacheHits++;
      return value;
    },
    layer(source, geometry, effects, key) {
      const roi = geometry.rasterBounds;
      if (roi) {
        // Keep historical inputs compact, but restore their original texel grid
        // before filtering. Direct UV remapping can change a byte under affine
        // transforms because sampler subtexel rounding depends on texture size.
        source = node(
          'layer',
          key + ':source',
          signature(
            'raster-restore',
            identity(source),
            roi,
            geometry.sourceWidth,
            geometry.sourceHeight
          ),
          geometry.sourceWidth,
          geometry.sourceHeight,
          (target) => {
            const clear = encoder.beginRenderPass({
              colorAttachments: [
                {
                  view: target.createView(),
                  loadOp: 'clear',
                  storeOp: 'store',
                  clearValue: { r: 0, g: 0, b: 0, a: 0 }
                }
              ]
            });
            clear.end();
            passIndex++;
            encoder.copyTextureToTexture(
              { texture: source },
              { texture: target, origin: [roi.x, roi.y] },
              [roi.width, roi.height]
            );
          }
        );
      }
      const enabled = effects.filter((e) => e.enabled !== false);
      const local = enabled.length ? localEffectGeometry(geometry, enabled) : null;
      const targetGeometry = local?.geometry || geometry,
        w = local?.width || renderWidth,
        h = local?.height || renderHeight;
      const [a, b, c, d, e, f] = targetGeometry.inverse;
      let t = cachedPass(
        'layer',
        key + ':geometry',
        1,
        source,
        null,
        [geometry.opacity, 0, 0, a, b, c, 0, d, e, f, 0, ...geometry.crop],
        w,
        h
      );
      for (let i = 0; i < enabled.length; i++) {
        const effect = enabled[i],
          p = effect.parameters || {},
          id = effect.templateId,
          fx = key + ':fx:' + (effect.id || i);
        if (id === 'blur') {
          const radius = Math.max(0, Number(p.radius ?? 8));
          if (radius)
            t = gaussian(
              t,
              ((radius - 1) * 0.3 + 0.8) * geometry.renderScale,
              fx,
              radius * geometry.renderScale
            );
        } else if (id === 'glow') {
          const x = cachedPass(
            'effect',
            fx + ':threshold',
            4,
            t,
            null,
            [Number(p.threshold ?? 0.5)],
            w,
            h
          );
          const z = gaussian(x, Math.max(0.1, Number(p.radius ?? 12) * geometry.renderScale), fx);
          t = cachedPass(
            'effect',
            fx + ':out',
            5,
            t,
            z,
            [Number(p.strength ?? p.intensity ?? 0.6)],
            w,
            h
          );
        } else if (id === 'lut') {
          if (!effect.lut) throw new Error('LUT资源未准备');
          t = cachedPass(
            'effect',
            fx,
            6,
            t,
            effect.lut.texture,
            [Number(p.amount ?? p.intensity ?? 1), effect.lut.size],
            w,
            h
          );
        } else throw new Error('不支持的特效：' + id);
      }
      if (local)
        t = cachedPass('layer', key + ':placement', 1, t, null, [
          1,
          0,
          0,
          renderWidth / w,
          0,
          -local.x / w,
          0,
          0,
          renderHeight / h,
          -local.y / h,
          0,
          0,
          0,
          1,
          1
        ]);
      return t;
    },
    lut(data, size, key) {
      const name = 'lut:' + key,
        t = texture(name, size * size, size),
        id = signature(key, size);
      if (contents.get(name) !== id) {
        device.queue.writeTexture({ texture: t }, data, { bytesPerRow: size * size * 4 }, [
          size * size,
          size
        ]);
        contents.set(name, id);
      }
      return t;
    },
    transition(from, to, tr, key) {
      const index = ['cross_dissolve', 'fade_color', 'wipe', 'slide'].indexOf(
        tr.style || tr.kind || 'cross_dissolve'
      );
      if (index < 0) throw new Error('不支持的转场');
      return cachedPass('transition', key, 7, from, to, [
        tr.progress,
        index,
        Math.max(0, ['right', 'left', 'down', 'up'].indexOf(tr.direction || 'right')),
        ...(tr.color || [0, 0, 0, 1]),
        1.5 / Math.max(renderWidth, renderHeight)
      ]);
    },
    blend(base, layer, mode, index) {
      return cachedPass('composite', 'composite:' + index, 2, base, layer, [
        ['normal', 'multiply', 'screen', 'overlay', 'darken', 'lighten'].indexOf(mode)
      ]);
    },
    async finish(base, signal) {
      const nextIdentity = identity(base),
        unchanged = resultTexture === base && resultIdentity === nextIdentity;
      device.queue.submit([encoder.finish()]);
      submitted = true;
      frameScope = false;
      const error = await validateFrameScope();
      await device.queue.onSubmittedWorkDone();
      if (error) throw new Error(error.message);
      check();
      signal?.throwIfAborted();
      // Keep the displayed frame intact through decode, budget failures and
      // cancellation. Resize/present only after the new offscreen graph is valid.
      if (!unchanged || canvas.width !== renderWidth || canvas.height !== renderHeight) {
        buffer('buffer:uniform:' + passIndex, 128, 0x48);
        if (canvas.width !== renderWidth) canvas.width = renderWidth;
        if (canvas.height !== renderHeight) canvas.height = renderHeight;
        device.pushErrorScope('validation');
        frameScope = true;
        encoder = device.createCommandEncoder();
        pass(0, base, null, context.getCurrentTexture(), [], true);
        device.queue.submit([encoder.finish()]);
        frameScope = false;
        const presentError = await validateFrameScope();
        await device.queue.onSubmittedWorkDone();
        if (presentError) throw new Error(presentError.message);
        check();
      }
      for (const [key, value] of pending) contents.set(key, value);
      pending.clear();
      resultTexture = base;
      resultIdentity = nextIdentity;
      workingSetBytes = resources.workingSetBytes + memory.externalBytes;
      resources.releasePins();
    },
    reuseFinal() {
      check();
      if (!resultTexture || resources.get(keys.get(resultTexture), false) !== resultTexture)
        return false;
      resources.beginFrame();
      resetStats();
      compositeHits = 1;
      return true;
    },
    setBudget(bytes) {
      if (!Number.isSafeInteger(bytes) || bytes < 1024)
        throw new RangeError('Invalid render resource budget');
      resources.releasePins();
      resources.budgetBytes = bytes;
      memory.setBudget(resources, bytes);
    },
    stats() {
      return {
        uploadCount,
        uploadCacheHits,
        effectHits,
        layerHits,
        compositeHits,
        transitionHits,
        gpuPasses: passIndex,
        evictions: resources.evictions,
        textureBytes,
        resourceBytes: memory.bytes,
        peakResourceBytes: memory.peakBytes,
        externalTextureBytes: memory.externalBytes,
        workingSetBytes,
        textureBudget: memory.budgetBytes,
        pipelineCount: gpu.pipelines.size
      };
    },
    async readPixels() {
      check();
      if (!resultTexture || resources.get(keys.get(resultTexture)) !== resultTexture)
        throw new Error('Render before readback');
      const width = canvas.width,
        height = canvas.height,
        stride = Math.ceil((width * 4) / 256) * 256;
      const readback = buffer('buffer:readback', stride * height, 0x09);
      const e = device.createCommandEncoder();
      e.copyTextureToBuffer({ texture: resultTexture }, { buffer: readback, bytesPerRow: stride }, [
        width,
        height
      ]);
      device.queue.submit([e.finish()]);
      await readback.mapAsync(1);
      try {
        const source = new Uint8Array(readback.getMappedRange()),
          pixels = new Uint8Array(width * height * 4);
        for (let y = 0; y < height; y++)
          pixels.set(source.subarray(y * stride, y * stride + width * 4), y * width * 4);
        return pixels;
      } finally {
        readback.unmap();
        resources.releasePins();
      }
    },
    async abort() {
      inputsProtected = false;
      if (frameScope) {
        frameScope = false;
        await validateFrameScope();
      }
      if (submitted) for (const key of pending.keys()) resources.delete(key);
      pending.clear();
      resources.releasePins();
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      resources.clear();
      memory.unregister(resources);
      context.unconfigure();
    }
  };
}
