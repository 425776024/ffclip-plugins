import { sha256 } from './index.mjs';
import { createCompositionGpu } from './composition-gpu.mjs';

async function loadPackedVideo(asset, metadata) {
  if (
    metadata.packing !== 'alpha-left-color-right' ||
    !Number.isFinite(metadata.fps) ||
    metadata.fps <= 0 ||
    !Number.isInteger(metadata.frame_count) ||
    metadata.frame_count < 1 ||
    metadata.frame_count > 1000
  )
    throw new Error('Unsupported packed-alpha video metadata');
  const video = document.createElement('video');
  video.muted = true;
  video.playsInline = true;
  video.preload = 'auto';
  const url = URL.createObjectURL(new Blob([asset.bytes], { type: 'video/mp4' }));
  const wait = (event, action) =>
    new Promise((resolve, reject) => {
      const done = () => {
          cleanup();
          resolve();
        },
        error = () => {
          cleanup();
          reject(new Error(`Video decode failed: ${video.error?.message || event}`));
        };
      const timer = setTimeout(error, 10000);
      const cleanup = () => {
        clearTimeout(timer);
        video.removeEventListener(event, done);
        video.removeEventListener('error', error);
      };
      video.addEventListener(event, done, { once: true });
      video.addEventListener('error', error, { once: true });
      try {
        action();
      } catch (e) {
        cleanup();
        reject(e);
      }
    });
  const cache = new Map();
  let current = -1;
  const dispose = () => {
    for (const image of cache.values()) image.close();
    cache.clear();
    video.pause();
    video.removeAttribute('src');
    video.load();
    URL.revokeObjectURL(url);
  };
  try {
    await wait('loadeddata', () => {
      video.src = url;
      video.load();
    });
    if (video.videoWidth !== metadata.packed_width || video.videoHeight !== metadata.render_size)
      throw new Error('Packed video dimensions differ from its template');
    return {
      async sample(timeUs) {
        const index = Math.max(
          0,
          Math.min(metadata.frame_count - 1, Math.floor((timeUs * metadata.fps) / 1e6))
        );
        if (cache.has(index)) {
          const image = cache.get(index);
          cache.delete(index);
          cache.set(index, image);
          return { image, index };
        }
        if (index !== current) {
          await wait('seeked', () => {
            video.currentTime = Math.min((index + 0.25) / metadata.fps, video.duration - 0.001);
          });
          current = index;
        }
        const image = await createImageBitmap(video);
        cache.set(index, image);
        // Bounded by both frame count and bytes (two 1280x640 streams stay small).
        while (
          cache.size >
          Math.max(
            1,
            Math.min(8, Math.floor((32 * 1024 * 1024) / (image.width * image.height * 4)))
          )
        ) {
          const key = cache.keys().next().value;
          cache.get(key).close();
          cache.delete(key);
        }
        return { image, index };
      },
      dispose
    };
  } catch (e) {
    dispose();
    throw e;
  }
}

/** Experimental independent browser composition; no native process is used. */
export async function createBrowserTextComposition(
  engine,
  canvas,
  bundle,
  {
    width = 640,
    height = 360,
    bindings = {},
    fonts = [],
    fallbackFonts = [],
    experimentalComposition = false,
    gpuContext = null,
    loadVideo = null
  } = {}
) {
  if (
    !Number.isInteger(width) ||
    !Number.isInteger(height) ||
    width < 1 ||
    height < 1 ||
    width > 4096 ||
    height > 4096 ||
    width * height > 8388608
  )
    throw new Error('Invalid composition dimensions');
  const renderer = engine.createRenderer();
  const videos = new Map();
  let gpu = null,
    disposed = false,
    busy = false;
  try {
    for (const font of fonts) renderer.registerAsset(font.id, font.mediaType, font.bytes);
    const info = await renderer.loadTemplate(bundle, {
      bindings,
      fallbackFonts,
      allowRasterFallback: true,
      experimentalComposition
    });
    if (experimentalComposition)
      for (const decoration of bundle.composition.decorations) {
        const asset = bundle.assets.get(decoration.asset.asset_id.replace(/^asset:\/\//, ''));
        if (!asset) throw new Error('Missing decoration wrapper');
        const wrapper = JSON.parse(new TextDecoder().decode(asset.bytes));
        const meta = wrapper.meta?.videocut_packed_alpha;
        if (
          !meta ||
          wrapper.layers?.length !== 1 ||
          wrapper.layers[0].ty !== 2 ||
          wrapper.assets?.length !== 1
        )
          throw new Error('Only the native direct packed-alpha decoration wrapper is supported');
        const videoAsset = bundle.assets.get(wrapper.assets[0].p.replace(/^asset:\/\//, ''));
        if (!videoAsset) throw new Error('Missing packed-alpha video');
        const key = `wasm.asset.${await sha256(asset.bytes)}`;
        videos.set(
          key,
          await (loadVideo ? loadVideo(videoAsset, meta) : loadPackedVideo(videoAsset, meta))
        );
      }
    gpu = await createCompositionGpu(canvas, width, height, gpuContext);
    const check = () => {
      if (disposed) throw new Error('Composition is disposed');
      if (busy) throw new Error('Await the previous composition render before editing/rendering');
    };
    return {
      profile: 'browser-composition-experimental-v1',
      info,
      adapter: gpu.adapter,
      setText(text) {
        check();
        return renderer.setText(text);
      },
      async render({ timeUs = 1500000, background = true, postEffects = true } = {}) {
        check();
        busy = true;
        const start = performance.now();
        try {
          const frame = renderer.render({ timeUs, width, height });
          const textMs = performance.now() - start;
          const decorations = [],
            mediaFrames = [];
          for (const d of frame.compositionPlan?.decorations || []) {
            if (!background || d.opacity <= 0) continue;
            const video = videos.get(d.assetId);
            if (!video) throw new Error(`Missing decoded decoration: ${d.assetId}`);
            const { image, index } = await video.sample(d.timeUs);
            decorations.push({ ...d, source: image });
            mediaFrames.push(index);
          }
          gpu.render(frame, decorations, { background, postEffects });
          await gpu.completed();
          return {
            timeUs,
            totalMs: performance.now() - start,
            textMs,
            controlBounds: frame.controlBounds,
            visualExtent: frame.visualExtent,
            mediaFrames,
            compositionPlan: frame.compositionPlan || null,
            warnings: info.warnings
          };
        } finally {
          busy = false;
        }
      },
      async readPixels() {
        check();
        return gpu.readPixels();
      },
      dispose() {
        if (disposed) return;
        if (busy) throw new Error('Await the active render before disposal');
        disposed = true;
        renderer.dispose();
        for (const v of videos.values()) v.dispose();
        gpu.dispose();
      }
    };
  } catch (e) {
    renderer.dispose();
    for (const v of videos.values()) v.dispose();
    gpu?.dispose();
    throw e;
  }
}
