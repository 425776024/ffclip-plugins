import initModule from './videocut-text.mjs';

const MAX_FILE_BYTES = 64 * 1024 * 1024;
const MAX_PACKAGE_BYTES = 128 * 1024 * 1024;
const encoder = new TextEncoder();
const asBytes = (value) => value instanceof Uint8Array ? value : new Uint8Array(value);

export async function sha256(value) {
  const hash = await globalThis.crypto.subtle.digest('SHA-256', asBytes(value));
  return Array.from(new Uint8Array(hash), (b) => b.toString(16).padStart(2, '0')).join('');
}

function relativePath(path) {
  if (typeof path !== 'string' || path.length > 1024 || !path || path.startsWith('/') ||
      /[\\:%?#\u0000-\u001f]/.test(path) || path.split('/').some((p) => !p || p === '.' || p === '..'))
    throw new Error(`Invalid template resource path: ${path}`);
  return path;
}

/** Loads the existing native directory package without rewriting its source files. */
export async function loadTextTemplate(manifestUrl, { fetch: fetcher = globalThis.fetch, signal } = {}) {
  const base = new URL(manifestUrl, globalThis.location?.href);
  const read = async (url, limit = MAX_FILE_BYTES) => {
    const response = await fetcher(url, { signal });
    if (!response.ok) throw new Error(`Template request failed: ${response.status} ${url}`);
    if (Number(response.headers.get('content-length')) > limit) throw new Error('Template file exceeds byte limit');
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (bytes.length > limit) throw new Error('Template file exceeds byte limit');
    return bytes;
  };
  const manifestBytes = await read(base, 1024 * 1024);
  const manifest = JSON.parse(new TextDecoder().decode(manifestBytes));
  if (manifest.format !== 'com.videocut.text-template' || !Array.isArray(manifest.files) || manifest.files.length > 512)
    throw new Error('Invalid VideoCut template manifest');
  const files = new Map();
  let total = manifestBytes.length;
  for (const file of manifest.files) {
    const path = relativePath(file.path);
    if (files.has(path)) throw new Error(`Duplicate template path: ${path}`);
    const bytes = await read(new URL(path, base));
    total += bytes.length;
    if (total > MAX_PACKAGE_BYTES) throw new Error('Template package exceeds byte limit');
    if (file.digest && file.digest !== `sha256:${await sha256(bytes)}`) throw new Error(`Template digest mismatch: ${path}`);
    files.set(path, { bytes, mediaType: file.media_type });
  }
  const entry = (name, expected) => {
    const path = relativePath(manifest.entries?.[name]?.path);
    if (path !== expected || !files.has(path)) throw new Error(`Missing template entry: ${name}`);
    return JSON.parse(new TextDecoder().decode(files.get(path).bytes));
  };
  return {
    manifest,
    composition: entry('composition', 'composition.json'),
    animation: entry('animation', 'animation.ir.json'),
    effectProgram: entry('effect_program', 'effect.program.json'),
    assets: new Map([...files].filter(([path]) => !['composition.json', 'animation.ir.json', 'effect.program.json'].includes(path)))
  };
}

/** One WASM module can own multiple independent renderer instances. */
export async function createTextEngine({ wasmUrl, wasmBinary, printErr = console.warn } = {}) {
  const module = await initModule({
    ...(wasmUrl ? { locateFile: () => String(wasmUrl) } : {}),
    ...(wasmBinary ? { wasmBinary: asBytes(wasmBinary) } : {}),
    printErr
  });
  const call = {
    create: module.cwrap('vct_create', 'number', []),
    destroy: module.cwrap('vct_destroy', null, ['number']),
    register: module.cwrap('vct_register_asset', 'number', ['number', 'string', 'string', 'number', 'number']),
    load: module.cwrap('vct_load', 'number', ['number', 'number', 'number', 'number', 'number', 'number']),
    render: module.cwrap('vct_render', 'number', ['number', 'number', 'number', 'number']),
    pixels: module.cwrap('vct_pixels', 'number', ['number']),
    result: module.cwrap('vct_result', 'string', ['number'])
  };
  const instances = new Set();
  let disposed = false;
  return {
    profile: 'wasm-raster-v1',
    capabilities: Object.freeze({ gpu: false, nativeSdfParity: false, postEffects: false, animatedMedia: false,
      rasterPipelineLanes: module._vct_raster_lanes?.() ?? 1 }),
    createRenderer() {
      if (disposed) throw new Error('Engine is disposed');
      const handle = call.create();
      if (!handle) throw new Error('Renderer instance limit reached');
      instances.add(handle);
      let source = null;
      let bindings = {};
      let loadInfo = null;
      let allowRasterFallback = false;
      const check = () => { if (!instances.has(handle)) throw new Error('Renderer is disposed'); };
      const result = (ok) => {
        const value = JSON.parse(call.result(handle));
        if (!ok || !value.ok) throw new Error(value.error || 'Native text operation failed');
        return value;
      };
      const install = (next) => {
        check();
        if (!source) throw new Error('Load a template before editing text');
        if (!next || typeof next !== 'object' || Array.isArray(next) ||
            Object.entries(next).some(([id, text]) => !id || typeof text !== 'string' || !text || encoder.encode(text).length > 10000))
          throw new Error('Bindings must map names to 1–10000 UTF-8 bytes of text');
        const pointers = [];
        try {
          for (const value of [source.composition, source.animation, source.effectProgram, next]) {
            const bytes = encoder.encode(JSON.stringify(value));
            if (bytes.length > 8 * 1024 * 1024) throw new Error('Template JSON exceeds 8 MiB');
            const pointer = module._malloc(bytes.length + 1);
            if (!pointer) throw new Error('WASM allocation failed');
            pointers.push(pointer);
            module.HEAPU8.set(bytes, pointer); module.HEAPU8[pointer + bytes.length] = 0;
          }
          const nextInfo = result(call.load(handle, ...pointers, Number(allowRasterFallback)));
          bindings = { ...next }; loadInfo = nextInfo;
          return nextInfo;
        } finally { for (const pointer of pointers) module._free(pointer); }
      };
      const registerAsset = (id, mediaType, value) => {
        check();
        if (typeof id !== 'string' || !id || id.length > 1024 || id.includes('\0') ||
            typeof mediaType !== 'string' || !mediaType || mediaType.length > 256 || mediaType.includes('\0'))
          throw new Error('Invalid asset identifier or media type');
        const bytes = asBytes(value);
        if (!bytes.length || bytes.length > MAX_FILE_BYTES) throw new Error('Invalid asset size');
        const pointer = module._malloc(bytes.length);
        if (!pointer) throw new Error('WASM allocation failed');
        try {
          module.HEAPU8.set(bytes, pointer);
          return result(call.register(handle, id, mediaType, pointer, bytes.length));
        } finally { module._free(pointer); }
      };
      return {
        registerAsset,
        prepareSdfMesh({ width = 960, height = 540, range = 30 } = {}) {
          check();
          if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 ||
              width > 2048 || height > 2048 || !Number.isFinite(range) || range < 1 || range > 128)
            throw new Error('Invalid SDF mesh dimensions or range');
          const info = result(module._vct_sdf_mesh(handle, width, height, range));
          const copy = (pointer, count, stride) => new Float32Array(module.HEAPU8.slice(pointer, pointer + count * stride).buffer);
          const distanceVertices = copy(info.distancePointer, info.distanceCount, 32);
          const shapeVertices = copy(info.shapePointer, info.shapeCount, 16);
          const {distancePointer, shapePointer, ...metadata} = info;
          return {...metadata, distanceVertices, shapeVertices};
        },
        async loadTemplate(bundle, options = {}) {
          check();
          if (source) throw new Error('Create a new renderer when switching template packages');
          const mapping = new Map();
          for (const [path, asset] of bundle.assets || []) {
            relativePath(path);
            const digest = await sha256(asset.bytes);
            check();
            const id = `wasm.asset.${digest}`;
            mapping.set(`asset://${path}`, { id, digest: `sha256:${digest}` });
            registerAsset(id, asset.mediaType, asset.bytes);
          }
          const normalize = (value) => {
            if (Array.isArray(value)) return value.map(normalize);
            if (value && typeof value === 'object') {
              const out = Object.fromEntries(Object.entries(value).map(([k, v]) => [k, normalize(v)]));
              if (value.primary && Array.isArray(value.fallbacks)) {
                for (const font of options.fallbackFonts || []) {
                  if (!font.id || !font.family) throw new Error('Fallback fonts require registered id and family');
                  if (!out.fallbacks.some((entry) => entry.asset_id === font.id)) out.fallbacks.push({
                    kind: 'builtin', asset_id: font.id, family: font.family, postscript_name: '',
                    weight: 400, width: 5, slant: 'upright', face_index: 0, variation_axes: [],
                    platform: '', face_fingerprint: '', allow_system_glyph_fallback: false
                  });
                }
              }
              if (typeof value.asset_id === 'string' && value.asset_id.startsWith('asset://')) {
                const asset = mapping.get(value.asset_id);
                if (!asset) throw new Error(`Missing package resource: ${value.asset_id}`);
                out.asset_id = asset.id;
                // Animation decoration bindings refer to an asset; unlike
                // resource descriptors their schema has no digest field.
                if (!('id' in value)) out.digest = asset.digest;
              }
              return out;
            }
            return value;
          };
          source = normalize({ composition: bundle.composition, animation: bundle.animation, effectProgram: bundle.effectProgram });
          allowRasterFallback = Number(options.allowRasterFallback === true) | (options.experimentalComposition === true ? 2 : 0);
          try { return install(options.bindings || {}); }
          catch (error) { source = null; throw error; }
        },
        setText(text, bindingId = 'content') {
          if (typeof text !== 'string' || !text || encoder.encode(text).length > 10000)
            throw new Error('Text must contain 1–10000 UTF-8 bytes');
          return install({ ...bindings, [bindingId]: text });
        },
        setBindings(next) { return install(next); },
        render({ timeUs = 0, width = 960, height = 540, profile = false } = {}) {
          check();
          if (!Number.isSafeInteger(timeUs) || timeUs < 0 || !Number.isInteger(width) || !Number.isInteger(height) ||
              width < 1 || height < 1 || width > 4096 || height > 4096 || width * height > 8388608)
            throw new Error('Invalid render dimensions or time');
          const renderStarted = profile ? performance.now() : 0;
          const rendered = call.render(handle, timeUs, width, height);
          const nativeFinished = profile ? performance.now() : 0;
          const info = result(rendered);
          const resultFinished = profile ? performance.now() : 0;
          const pointer = call.pixels(handle);
          const pointerFinished = profile ? performance.now() : 0;
          const data = new Uint8ClampedArray(info.width * info.height * 4);
          const allocationFinished = profile ? performance.now() : 0;
          const heap = module.HEAPU8;
          if (info.rowBytes === info.width * 4) {
            data.set(heap.subarray(pointer, pointer + data.byteLength));
          } else {
            for (let y = 0; y < info.height; y++) {
              const start = pointer + y * info.rowBytes;
              data.set(heap.subarray(start, start + info.width * 4), y * info.width * 4);
            }
          }
          const copyFinished = profile ? performance.now() : 0;
          return { ...info, rowBytes: info.width * 4, byteLength: data.byteLength, data,
            ...(profile ? { timings: {
              nativeRenderMs: nativeFinished - renderStarted,
              resultReadMs: resultFinished - nativeFinished,
              pixelPointerMs: pointerFinished - resultFinished,
              pixelAllocationMs: allocationFinished - pointerFinished,
              rowCopyMs: copyFinished - allocationFinished,
              renderTotalMs: copyFinished - renderStarted,
              ...(info.timings ? { nativeTimings: info.timings } : {})
            } } : {})
          };
        },
        draw(canvas, options = {}) {
          if (loadInfo?.requiresBrowserComposition) throw new Error('Use createBrowserTextComposition for this template; draw() cannot omit its decoration and post passes');
          const drawStarted = options.profile ? performance.now() : 0;
          const frame = this.render({ width: canvas.width, height: canvas.height, ...options });
          const context = canvas.getContext('2d');
          if (!context) throw new Error('A 2D canvas is required');
          const presentationStarted = options.profile ? performance.now() : 0;
          context.clearRect(0, 0, canvas.width, canvas.height);
          const clearFinished = options.profile ? performance.now() : 0;
          const image = new ImageData(frame.data, frame.width, frame.height);
          const imageFinished = options.profile ? performance.now() : 0;
          context.putImageData(image, frame.originX, frame.originY);
          if (options.profile) {
            const drawFinished = performance.now();
            Object.assign(frame.timings, {
              imageDataMs: imageFinished - clearFinished,
              clearRectMs: clearFinished - presentationStarted,
              putImageDataMs: drawFinished - imageFinished,
              drawTotalMs: drawFinished - drawStarted
            });
          }
          return frame;
        },
        get info() { return loadInfo; },
        dispose() {
          if (instances.delete(handle)) call.destroy(handle);
          source = null; loadInfo = null;
        }
      };
    },
    dispose() {
      for (const handle of instances) call.destroy(handle);
      instances.clear(); disposed = true;
    }
  };
}
