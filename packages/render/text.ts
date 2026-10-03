import wasmUrl from '../text-wasm/dist/videocut-text.wasm?url';
import { recipes, composeRecipe, resolveRecipe } from '../text-wasm/src/recipes.mjs';
import type { TextContent } from '../core/types';
import { applyTemplateStyle } from '../core/template-style.mjs';
import {
  applyTemplateLayout,
  templateLayoutInfo,
  templateControlBounds,
  type TextLayoutInfo
} from '../core/text-layout.mjs';
import {
  chooseTemplateFonts,
  choosePlainTextFont,
  rewriteTemplateFonts,
  type SystemTemplateFont,
  type SystemTemplateFontCatalog
} from '../text-wasm/src/system-fonts.mjs';
import type {
  TextEngine,
  TemplateBundle,
  Rect,
  TextFrame,
  TextFrameTimings
} from '../text-wasm/dist/index.mjs';
import { sharedGpu } from './gpu.mjs';
import { sharedMediaEngine } from '../media/browser';

const base = '/text-templates/';
let resources: ReturnType<typeof loadResources> | undefined;
const canvasFonts = new Map<string, Promise<string>>();
const fontBytes = new Map<
  string,
  Promise<SystemTemplateFont & { mediaType: string; bytes: Uint8Array }>
>();
let fontCatalog: Promise<SystemTemplateFontCatalog> | undefined;
let systemFontResources: ReturnType<typeof loadSystemFonts> | undefined;
function installedFontCatalog() {
  return (fontCatalog ||= (async () => {
    const response = await fetch('/api/fonts');
    if (!response.ok) throw new Error('无法读取本机系统字体');
    return response.json() as Promise<SystemTemplateFontCatalog>;
  })().catch((error) => {
    fontCatalog = undefined;
    throw error;
  }));
}
function readInstalledFont(font: SystemTemplateFont) {
  if (!fontBytes.has(font.id))
    fontBytes.set(
      font.id,
      (async () => {
        const resource = await fetch(`/api/fonts/${encodeURIComponent(font.id)}`);
        if (!resource.ok) throw new Error(`系统字体读取失败：${font.family}`);
        return {
          ...font,
          mediaType: resource.headers.get('Content-Type') || 'font/ttf',
          bytes: new Uint8Array(await resource.arrayBuffer())
        };
      })().catch((error) => {
        fontBytes.delete(font.id);
        throw error;
      })
    );
  return fontBytes.get(font.id)!;
}
async function loadSystemFonts() {
  const catalog = await installedFontCatalog();
  const { sans, cjk } = chooseTemplateFonts(catalog);
  const fonts = await Promise.all(
    [...new Map([sans, cjk].map((f) => [f.id, f])).values()].map(readInstalledFont)
  );
  return { catalog, fonts, sans, cjk };
}
function installedFonts() {
  return (systemFontResources ||= loadSystemFonts().catch((error) => {
    systemFontResources = undefined;
    throw error;
  }));
}
export async function ensureCanvasFonts(content: Parameters<typeof choosePlainTextFont>[1]) {
  const descriptor = choosePlainTextFont(await installedFontCatalog(), content);
  if (!canvasFonts.has(descriptor.id))
    canvasFonts.set(
      descriptor.id,
      (async () => {
        const { bytes } = await readInstalledFont(descriptor);
        const fonts =
          (globalThis as unknown as { fonts?: { add(face: FontFace): void }; document?: Document })
            .fonts || globalThis.document?.fonts;
        if (!fonts) throw new Error('浏览器不支持在当前渲染环境注册系统字体');
        const alias = `VideoCut System ${descriptor.id}`;
        const face = new FontFace(alias, Uint8Array.from(bytes).buffer);
        await face.load();
        fonts.add(face);
        return alias;
      })().catch((error) => {
        canvasFonts.delete(descriptor.id);
        throw error;
      })
    );
  return canvasFonts.get(descriptor.id)!;
}
async function loadResources() {
  const sdk = await import('../text-wasm/dist/index.mjs');
  const { fonts, catalog } = await installedFonts();
  const engine: TextEngine = await sdk.createTextEngine({
    wasmUrl: new URL(wasmUrl, location.origin)
  });
  return { sdk, fonts, catalog, engine };
}
function shared() {
  return (resources ||= loadResources().catch((error) => {
    resources = undefined;
    throw error;
  }));
}
const bundles = new Map<string, Promise<TemplateBundle>>();
async function bundleFor(template: string | NonNullable<TextContent['template']>) {
  const value = typeof template === 'string' ? { id: template, version: 1 as const } : template;
  const recipe = resolveRecipe(value),
    id = JSON.stringify([value.id, value.recipe || null, value.resourceBase || base]);
  if (!bundles.has(id))
    bundles.set(
      id,
      (async () => {
        const { sdk, catalog } = await shared();
        const bundle = (
          await composeRecipe(recipe, (part) =>
            sdk.loadTextTemplate(
              new URL(
                `${value.resourceBase || base}templates/com.videocut.text.qt-type.${part}/manifest.json`,
                location.origin
              )
            )
          )
        ).bundle;
        return rewriteTemplateFonts(bundle, catalog);
      })().catch((error) => {
        bundles.delete(id);
        throw error;
      })
    );
  return { recipe, bundle: await bundles.get(id)! };
}
export interface TemplateRenderResult {
  ms: number;
  controlBounds: Rect;
  textLayout: TextLayoutInfo;
  /** Native output crop including its raster halo, in unchanged source-canvas pixels. */
  rasterBounds?: Rect;
  /** Native RGBA and metadata are retained only for explicitly profiled samples. */
  frame?: TextFrame;
  timings?: TextFrameTimings;
}
export interface TemplatePlayer {
  durationUs: number;
  setText(text: string): void;
  resize(width: number, height: number): boolean;
  render(
    timeUs: number,
    options?: { profile?: boolean; present?: boolean }
  ): Promise<TemplateRenderResult>;
  pixels(): Promise<{ width: number; height: number; data: Uint8Array | Uint8ClampedArray }>;
  premultiplied: boolean;
  dispose(): void;
}
export async function createTemplatePlayer(
  canvas: HTMLCanvasElement | OffscreenCanvas,
  id: string | NonNullable<TextContent['template']>,
  text: string,
  width = 640,
  height = 360,
  layout?: { layoutWidth?: number; projectWidth: number; projectHeight: number }
): Promise<TemplatePlayer> {
  const [{ engine, fonts }, { recipe, bundle: original }] = await Promise.all([
    shared(),
    bundleFor(id)
  ]);
  const projectCanvas = {
    width: layout?.projectWidth || width,
    height: layout?.projectHeight || height
  };
  const bundle = applyTemplateLayout(
    applyTemplateStyle(original, typeof id === 'string' ? undefined : id.style),
    layout?.layoutWidth,
    projectCanvas
  );
  const textLayout = templateLayoutInfo(bundle, projectCanvas);
  canvas.width = width;
  canvas.height = height;
  if (recipe.external) {
    if (!('gpu' in navigator)) throw new Error('此图案模板需要支持 WebGPU 的浏览器');
    const { createBrowserTextComposition } =
      await import('../text-wasm/dist/browser-composition.mjs');
    const player = await createBrowserTextComposition(engine, canvas, bundle, {
      width,
      height,
      bindings: { content: text },
      fonts,
      experimentalComposition: true,
      gpuContext: await sharedGpu(),
      loadVideo: async (asset: { bytes: Uint8Array }, metadata: any) => {
        if (
          metadata.packing !== 'alpha-left-color-right' ||
          !(metadata.fps > 0) ||
          !Number.isInteger(metadata.frame_count) ||
          metadata.frame_count < 1
        )
          throw new Error('无效的文字模板视频元数据');
        const url = URL.createObjectURL(
          new Blob([Uint8Array.from(asset.bytes)], { type: 'video/mp4' })
        );
        let reader: Awaited<ReturnType<typeof sharedMediaEngine.createVideoReader>>;
        try {
          reader = await sharedMediaEngine.createVideoReader(url);
        } catch (error) {
          URL.revokeObjectURL(url);
          sharedMediaEngine.release(url);
          throw error;
        }
        let previous: { close(): void } | undefined;
        return {
          async sample(timeUs: number) {
            previous?.close();
            const index = Math.max(
              0,
              Math.min(metadata.frame_count - 1, Math.floor((timeUs * metadata.fps) / 1e6))
            );
            const frame = await reader.frameAt((index + 0.25) / metadata.fps);
            if (frame.width !== metadata.packed_width || frame.height !== metadata.render_size) {
              frame.close();
              throw new Error('模板视频尺寸与清单不一致');
            }
            previous = frame;
            return { image: frame.frame, index };
          },
          dispose() {
            previous?.close();
            void reader
              .close()
              .finally(() => sharedMediaEngine.release(url))
              .catch(() => {});
            URL.revokeObjectURL(url);
          }
        };
      }
    });
    return {
      durationUs: player.info.durationUs,
      setText: (t) => {
        player.setText(t);
      },
      // The external compositor owns fixed-size GPU targets. Its caller can
      // recreate this uncommon path when the preview extent changes.
      resize: () => false,
      render: async (timeUs) => {
        const frame = await player.render({ timeUs });
        return {
          ms: frame.totalMs,
          controlBounds: templateControlBounds(bundle, frame.controlBounds, width, height),
          textLayout
        };
      },
      pixels: () => player.readPixels(),
      premultiplied: true,
      dispose: () => player.dispose()
    };
  }
  const renderer = engine.createRenderer();
  try {
    for (const font of fonts) renderer.registerAsset(font.id, font.mediaType, font.bytes);
    const info = await renderer.loadTemplate(bundle, {
      bindings: { content: text },
      allowRasterFallback: true
    });
    return {
      durationUs: info.durationUs,
      setText: (t) => {
        renderer.setText(t);
      },
      resize: (w, h) => {
        if (
          !Number.isInteger(w) ||
          !Number.isInteger(h) ||
          w < 1 ||
          h < 1 ||
          w > 4096 ||
          h > 4096 ||
          w * h > 8388608
        )
          throw new Error('文字模板渲染尺寸无效');
        // Native draw samples its output extent from the canvas. Retain the
        // installed document, fonts and immutable texture assets while scrubbing.
        width = canvas.width = w;
        height = canvas.height = h;
        return true;
      },
      render: async (timeUs, options) => {
        const start = performance.now();
        const frame =
          options?.present === false
            ? renderer.render({
                timeUs,
                width: canvas.width,
                height: canvas.height,
                profile: options.profile
              })
            : renderer.draw(canvas, { timeUs, profile: options?.profile });
        return {
          ms: performance.now() - start,
          controlBounds: templateControlBounds(
            bundle,
            frame.controlBounds,
            canvas.width,
            canvas.height
          ),
          textLayout,
          rasterBounds: {
            x: frame.originX,
            y: frame.originY,
            width: frame.width,
            height: frame.height
          },
          ...(options?.profile ? { frame, timings: frame.timings } : {})
        };
      },
      pixels: async () =>
        (
          canvas.getContext('2d') as CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D
        ).getImageData(0, 0, width, height),
      premultiplied: false,
      dispose: () => renderer.dispose()
    };
  } catch (e) {
    renderer.dispose();
    throw e;
  }
}
// Thumbnails share one queue and retain PNGs rather than live renderers.
const posters = new Map<string, Promise<string>>();
let queue = Promise.resolve();
export function templatePoster(id: string, sampleText?: string) {
  const key = JSON.stringify([id, sampleText]);
  if (!posters.has(key)) {
    const task = queue.then(async () => {
      const recipe = recipes.find((r) => r.id === id)!;
      const canvas = document.createElement('canvas');
      const player = await createTemplatePlayer(canvas, id, sampleText ?? recipe.text, 480, 270);
      try {
        await player.render(recipe.timeUs);
        const frame = await player.pixels();
        const pixels = new Uint8ClampedArray(frame.data);
        if (player.premultiplied)
          for (let i = 0; i < pixels.length; i += 4) {
            const a = pixels[i + 3];
            if (a)
              for (let c = 0; c < 3; c++)
                pixels[i + c] = Math.min(255, Math.round((pixels[i + c] * 255) / a));
          }
        const image = document.createElement('canvas');
        image.width = frame.width;
        image.height = frame.height;
        image
          .getContext('2d')!
          .putImageData(new ImageData(pixels, frame.width, frame.height), 0, 0);
        // Fit the actual artwork to its card. The project canvas and control
        // bounds stay intact in preview/export; only the library PNG is cropped.
        let left = frame.width,
          top = frame.height,
          right = -1,
          bottom = -1;
        for (let y = 0; y < frame.height; y++)
          for (let x = 0; x < frame.width; x++)
            if (pixels[(y * frame.width + x) * 4 + 3] > 8) {
              left = Math.min(left, x);
              top = Math.min(top, y);
              right = Math.max(right, x);
              bottom = Math.max(bottom, y);
            }
        if (right < left) throw new Error('预览加载失败');
        const padding = Math.max(10, Math.round((right - left) * 0.08));
        const poster = document.createElement('canvas');
        poster.width = right - left + 1 + padding * 2;
        poster.height = bottom - top + 1 + padding * 2;
        poster
          .getContext('2d')!
          .drawImage(
            image,
            left,
            top,
            right - left + 1,
            bottom - top + 1,
            padding,
            padding,
            right - left + 1,
            bottom - top + 1
          );
        return poster.toDataURL('image/png');
      } finally {
        player.dispose();
      }
    });
    posters.set(
      key,
      task.catch((e) => {
        posters.delete(key);
        throw e;
      })
    );
    queue = task.then(
      () => undefined,
      () => undefined
    );
  }
  return posters.get(key)!;
}
