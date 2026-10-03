import { duration, type Project } from '../core/project.mjs';
import { sharedMediaEngine } from '../media/browser';
import { createTemplatePlayer, ensureCanvasFonts, type TemplatePlayer } from './text';
import { frameTime, layerGeometry, makeLut } from './plan.mjs';
import { createGpuCompositor } from './gpu.mjs';
import { SceneGraph, visualFrameIdentity, renderIdentity } from './graph';
import { HtmlFrameClient, type HtmlFrameRequest } from './html';
import { ResidentSourceCache, sourceFrameKey, sourceRasterBounds } from './source-cache';
import { resolveRecipe } from '../text-wasm/src/recipes.mjs';
import { wrapText, type TextLayoutInfo } from '../core/text-layout.mjs';
import {
  TextPrefetchPool,
  TextWorkerUnavailableError,
  type TextFrameRequest,
  type TextPrefetchStats
} from './text-prefetch';

type Surface = HTMLCanvasElement | OffscreenCanvas;
export interface RenderReport {
  backend: string;
  time: number;
  width: number;
  height: number;
  frameMs: number;
  uploadCount: number;
  uploadCacheHits: number;
  textureBytes: number;
  textureBudget: number;
  resourceBytes: number;
  peakResourceBytes: number;
  workingSetBytes: number;
  externalTextureBytes: number;
  effectHits: number;
  layerHits: number;
  compositeHits: number;
  transitionHits: number;
  gpuPasses: number;
  evictions: number;
  graphBuilds: number;
  textLayoutCount: number;
  textPrefetch?: TextPrefetchStats;
  bounds: Array<{
    id: string;
    locked: boolean;
    x: number;
    y: number;
    width: number;
    height: number;
    textLayout?: TextLayoutInfo & {
      left: { x: number; y: number };
      right: { x: number; y: number };
    };
  }>;
  media: Array<Record<string, unknown>>;
}
const surface = (w: number, h: number): Surface => {
  if (typeof OffscreenCanvas !== 'undefined') return new OffscreenCanvas(w, h);
  const c = document.createElement('canvas');
  c.width = w;
  c.height = h;
  return c;
};
const aborted = (signal?: AbortSignal) => {
  if (signal?.aborted) throw new DOMException('操作已取消', 'AbortError');
};

/** One evaluated scene and one compositor for both interactive frames and export. */
export class SceneRenderer {
  private gpu: any;
  private ctx: CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D | null = null;
  private text = new Map<
    string,
    {
      key: string;
      canvas: Surface;
      player?: TemplatePlayer;
      content: string;
      bounds?: any;
      textLayout?: TextLayoutInfo;
      rasterBounds?: any;
      frameKey?: string;
    }
  >();
  private luts = new Map<string, any>();
  private readers = new Map<
    string,
    {
      url: string;
      signal?: AbortSignal;
      reader: Awaited<ReturnType<typeof sharedMediaEngine.createVideoReader>>;
    }
  >();
  private disposed = false;
  private frames = 0;
  private graph = new SceneGraph();
  private html: HtmlFrameClient;
  private sourceCache = new ResidentSourceCache();
  private lastIdentity = '';
  private lastReport?: RenderReport;
  private textLayoutCount = 0;
  private textPool?: TextPrefetchPool;
  private textWorkers: boolean;
  private nativeExtent = '';
  readonly ready: Promise<void>;
  constructor(
    readonly canvas: Surface,
    public mediaUrl: (id: string) => string,
    options: {
      textureBudgetBytes?: number;
      htmlFrameUrl?: string;
      /** Exact native RGBA preparation; foreground composition stays ordered. */
      textWorkers?: boolean;
    } = {}
  ) {
    this.html = new HtmlFrameClient(options.htmlFrameUrl);
    this.textWorkers = options.textWorkers === true;
    this.ready = (async () => {
      this.gpu = await createGpuCompositor(canvas, options);
      if (this.disposed) {
        this.gpu?.dispose();
        throw new Error('渲染器已关闭');
      }
      if (!this.gpu) this.ctx = canvas.getContext('2d') as typeof this.ctx;
      if (!this.gpu && !this.ctx) throw new Error('浏览器无法创建画布渲染器');
    })();
    this.ready.catch(() => {});
  }
  private textRequest(layer: any, w: number, h: number): TextFrameRequest | undefined {
    const content = layer.item.clip.text;
    if (
      !this.textWorkers ||
      !content?.template ||
      !content.content ||
      w > 4096 ||
      h > 4096 ||
      w * h > 8388608
    )
      return;
    const recipe = resolveRecipe(content.template);
    if (recipe.external || !['flower-style-03', 'flower-style-38'].includes(recipe.base)) return;
    const timeUs = Math.max(0, Math.round((layer.sourceTime / 120000) * 1e6));
    return {
      key: JSON.stringify([
        content,
        layer.projectWidth ?? null,
        layer.projectHeight ?? null,
        w,
        h,
        timeUs
      ]),
      template: content.template,
      text: content.content,
      width: w,
      height: h,
      timeUs,
      layout: {
        layoutWidth: content.layoutWidth,
        projectWidth: layer.projectWidth,
        projectHeight: layer.projectHeight
      }
    };
  }
  /** Prepare only exact native source pixels, never a lower-quality scene. */
  prepareAhead(project: Project, time: number, w: number, h: number) {
    if (!this.textWorkers || this.disposed) return;
    const requests = new Map<string, TextFrameRequest>();
    const frame = Math.floor(
        ((time + 0.5) * project.frameRate.numerator) / (120000 * project.frameRate.denominator)
      ),
      end = duration(project);
    for (let i = 1; i <= 6 && requests.size < 6; i++) {
      const next = frameTime(frame + i, project.frameRate);
      if (next >= end) break;
      const plan = this.graph.evaluate(project, next, w, h);
      for (const entry of plan.layers)
        for (const layer of entry.kind === 'layer' ? [entry.layer] : [entry.from, entry.to]) {
          let input: TextFrameRequest | undefined;
          try {
            input = this.textRequest(
              {
                ...layer,
                projectWidth: project.canvas.width,
                projectHeight: project.canvas.height
              },
              w,
              h
            );
          } catch {
            // Predictive work must not surface a future clip's input error at
            // the current playhead; its foreground render still validates it.
            continue;
          }
          if (input && requests.size < 6) requests.set(input.key, input);
        }
    }
    if (requests.size) (this.textPool ||= new TextPrefetchPool()).prefetch([...requests.values()]);
    else this.textPool?.prefetch([]);
  }
  invalidatePrefetch() {
    this.textPool?.invalidate();
  }
  private prepareHtmlAhead(
    project: Project,
    time: number,
    width: number,
    height: number,
    activeIds: ReadonlySet<string>,
    signal?: AbortSignal
  ) {
    const inputs: HtmlFrameRequest[] = [],
      frame = Math.floor(
        ((time + 0.5) * project.frameRate.numerator) / (120000 * project.frameRate.denominator)
      ),
      end = duration(project);
    for (let index = 1; index <= 2 && inputs.length < 2; index++) {
      const next = frameTime(frame + index, project.frameRate);
      if (next >= end) break;
      const plan = this.graph.evaluate(project, next, width, height);
      for (const entry of plan.layers)
        for (const layer of entry.kind === 'layer' ? [entry.layer] : [entry.from, entry.to])
          if (layer.item.clip.html && activeIds.has(layer.item.id) && inputs.length < 2)
            inputs.push({
              itemId: layer.item.id,
              html: layer.item.clip.html,
              sourceTime: layer.sourceTime
            });
    }
    this.html.prefetch(inputs, signal);
  }
  private async textSource(layer: any, w: number, h: number, signal?: AbortSignal) {
    const item = layer.item,
      content = item.clip.text;
    const key = JSON.stringify([
      content.template,
      content.template ? null : [w, h],
      layer.projectWidth,
      layer.projectHeight,
      content.layoutWidth,
      content.template ? null : content
    ]);
    const pooled = this.textRequest(layer, w, h);
    let cached = this.text.get(item.id);
    if (cached?.key === key && (cached.canvas.width !== w || cached.canvas.height !== h)) {
      if (cached.player?.resize(w, h)) {
        cached.frameKey = undefined;
        cached.bounds = undefined;
        cached.rasterBounds = undefined;
      } else {
        cached.player?.dispose();
        this.text.delete(item.id);
        cached = undefined;
      }
    }
    if (!cached || cached.key !== key) {
      cached?.player?.dispose();
      this.text.delete(item.id);
      const c = surface(w, h);
      cached = { key, canvas: c, content: content.content };
      if (content.template && !pooled)
        cached.player = await createTemplatePlayer(c, content.template, content.content, w, h, {
          layoutWidth: content.layoutWidth,
          projectWidth: layer.projectWidth,
          projectHeight: layer.projectHeight
        });
      else if (!content.template) {
        const fontFamily = await ensureCanvasFonts(content);
        const ctx = c.getContext('2d') as
          CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D;
        ctx.clearRect(0, 0, w, h);
        ctx.fillStyle = content.color;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        const fontSize = (content.fontSize * w) / layer.projectWidth;
        ctx.font = `${fontSize}px "${fontFamily}"`;
        const wrapWidth =
          content.layoutWidth === undefined
            ? undefined
            : (content.layoutWidth * w) / layer.projectWidth;
        const lines = wrapText(content.content, wrapWidth, (value) => ctx.measureText(value).width),
          lineHeight = fontSize * 1.25;
        lines.forEach((line: string, i: number) =>
          ctx.fillText(line, w / 2, h / 2 + (i - (lines.length - 1) / 2) * lineHeight)
        );
        const tw =
          wrapWidth ?? Math.max(1, ...lines.map((line: string) => ctx.measureText(line).width));
        cached.textLayout = { width: (tw * layer.projectWidth) / w, minimumWidth: 1 };
        cached.bounds = {
          x: (w - tw) / 2,
          y: (h - lines.length * lineHeight) / 2,
          width: tw,
          height: lines.length * lineHeight
        };
      }
      if (this.disposed) {
        cached.player?.dispose();
        throw new Error('渲染器已关闭');
      }
      this.text.set(item.id, cached);
      this.textLayoutCount++;
    }
    let nativeWaitMs: number | undefined, nativeRasterMs: number | undefined;
    if (pooled && !cached.player && cached.frameKey !== pooled.key) {
      const started = performance.now();
      try {
        const frame = await (this.textPool ||= new TextPrefetchPool()).request(pooled, signal);
        aborted(signal);
        if (this.disposed) throw new Error('渲染器已关闭');
        const ctx = cached.canvas.getContext('2d') as
          CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D;
        ctx.clearRect(0, 0, w, h);
        ctx.putImageData(
          new ImageData(frame.data as Uint8ClampedArray<ArrayBuffer>, frame.width, frame.height),
          frame.originX,
          frame.originY
        );
        cached.content = content.content;
        cached.bounds = frame.controlBounds;
        cached.textLayout = (frame as typeof frame & { textLayout?: TextLayoutInfo }).textLayout;
        cached.rasterBounds = sourceRasterBounds(w, h, {
          x: frame.originX,
          y: frame.originY,
          width: frame.width,
          height: frame.height
        });
        cached.frameKey = pooled.key;
        nativeWaitMs = performance.now() - started;
        nativeRasterMs = frame.timings?.nativeRenderMs;
      } catch (error) {
        const transportFallback =
          !this.textWorkers &&
          !signal?.aborted &&
          !this.disposed &&
          error instanceof Error &&
          error.name === 'AbortError';
        if (!(error instanceof TextWorkerUnavailableError) && !transportFallback) throw error;
        aborted(signal);
        // Lack of nested workers alone may fall back. Native input failures surface.
        this.textWorkers = false;
        this.textPool?.dispose();
        this.textPool = undefined;
      }
    }
    if (content.template && !this.textWorkers && !cached.player) {
      const player = await createTemplatePlayer(
        cached.canvas,
        content.template,
        content.content,
        w,
        h,
        {
          layoutWidth: content.layoutWidth,
          projectWidth: layer.projectWidth,
          projectHeight: layer.projectHeight
        }
      );
      if (this.disposed || signal?.aborted) {
        player.dispose();
        aborted(signal);
        throw new Error('渲染器已关闭');
      }
      cached.player = player;
      cached.content = content.content;
      cached.frameKey = undefined;
    }
    if (cached.player) {
      if (cached.content !== content.content) {
        cached.player.setText(content.content);
        cached.content = content.content;
      }
      const sampleTime = Math.max(
        0,
        Math.min(Math.round((layer.sourceTime / 120000) * 1e6), cached.player.durationUs - 1)
      );
      const frameKey = JSON.stringify([key, w, h, content.content, sampleTime]);
      if (cached.frameKey !== frameKey) {
        const result = await cached.player.render(sampleTime);
        cached.bounds = result.controlBounds;
        cached.textLayout = result.textLayout;
        cached.rasterBounds = sourceRasterBounds(w, h, result.rasterBounds);
        cached.frameKey = frameKey;
      }
    }
    return {
      frame: cached.canvas,
      width: w,
      height: h,
      close: () => {},
      controlBounds: cached.bounds,
      textLayout: cached.textLayout,
      rasterBounds: cached.rasterBounds,
      identity: cached.frameKey || key,
      nativeWaitMs,
      nativeRasterMs
    };
  }
  async render(
    project: Project,
    time: number,
    width: number,
    height: number,
    signal?: AbortSignal,
    options?: { prefetch?: boolean; htmlPrefetch?: boolean }
  ): Promise<RenderReport> {
    await this.ready;
    aborted(signal);
    if (this.disposed) throw new Error('渲染器已关闭');
    const extent = `${width}x${height}`;
    if (extent !== this.nativeExtent) {
      this.invalidatePrefetch();
      this.nativeExtent = extent;
    }
    const started = performance.now();
    const plan = this.graph.evaluate(project, time, width, height),
      bounds: any[] = [],
      media: any[] = [],
      used = new Set<string>();
    const frameIdentity = visualFrameIdentity(plan, this.mediaUrl);
    if (
      frameIdentity === this.lastIdentity &&
      this.lastReport &&
      (!this.gpu || this.gpu.reuseFinal())
    ) {
      const locked = new Map(
        project.timeline.tracks.flatMap((t) => t.items.map((i) => [i.id, t.locked] as const))
      );
      return {
        ...this.lastReport,
        time,
        frameMs: performance.now() - started,
        graphBuilds: this.graph.builds,
        textLayoutCount: 0,
        textPrefetch: this.textPool?.stats(),
        ...(this.gpu?.stats() || { gpuPasses: 0, compositeHits: 1 }),
        bounds: this.lastReport.bounds.map((b) => ({ ...b, locked: locked.get(b.id) ?? b.locked }))
      };
    }
    this.textLayoutCount = 0;
    const activeIds = new Set<string>(
      plan.layers.flatMap((entry) =>
        entry.kind === 'layer' ? [entry.layer.item.id] : [entry.from.item.id, entry.to.item.id]
      )
    );
    this.html.retain(activeIds);
    for (const [id, text] of this.text)
      if (!activeIds.has(id)) {
        text.player?.dispose();
        this.text.delete(id);
      }
    for (const [id, entry] of this.readers)
      if (!activeIds.has(id)) {
        await entry.reader.close();
        this.readers.delete(id);
      }
    // Decode/render inputs before beginning the GPU command buffer. A single
    // frame owns its borrowed image bitmaps until all drawing has finished.
    const owned: Array<{ close: () => void }> = [];
    const sources = new Map<string, any>();
    for (const entry of plan.layers)
      for (const layer of entry.kind === 'layer' ? [entry.layer] : [entry.from, entry.to])
        sources.set(layer.item.id, layer);
    const sourceKeys = new Map(
      [...sources.values()].map((layer) => [
        layer.item.id,
        sourceFrameKey(
          layer,
          width,
          height,
          project.canvas.width,
          project.canvas.height,
          this.mediaUrl
        )
      ])
    );
    const flights = new Map<string, Promise<any>>();
    let retainedInputs = new Set<string>();
    const prepare = async (layer: any) => {
      aborted(signal);
      const sourceStarted = performance.now();
      used.add(layer.item.id);
      const sourceKey = sourceKeys.get(layer.item.id)!;
      const cached =
        this.gpu &&
        this.sourceCache.get(sourceKey, (source) => retainedInputs.has(source.inputKey));
      let data: any = cached;
      if (!data) {
        let flight = flights.get(sourceKey);
        if (!flight) {
          flight = (async () => {
            let data: any;
            if (layer.item.clip.html)
              data = await this.html.frame(
                layer.item.id,
                layer.item.clip.html,
                layer.sourceTime,
                signal
              );
            else if (layer.item.clip.text) {
              // Let already-started capture/decoder promises dispatch their IO before
              // native text rasterization occupies this worker synchronously.
              await Promise.resolve();
              data = await this.textSource(
                {
                  ...layer,
                  projectWidth: project.canvas.width,
                  projectHeight: project.canvas.height
                },
                width,
                height,
                signal
              );
            } else if (layer.asset) {
              const url = this.mediaUrl(layer.asset.id);
              if (layer.asset.kind === 'image')
                data = await sharedMediaEngine.image(url, { signal });
              else {
                let entry = this.readers.get(layer.item.id);
                if (entry?.url !== url || entry.signal?.aborted) {
                  await entry?.reader.close();
                  entry = {
                    url,
                    signal,
                    reader: await sharedMediaEngine.createVideoReader(url, { signal })
                  };
                  if (this.disposed) {
                    await entry.reader.close();
                    throw new Error('渲染器已关闭');
                  }
                  this.readers.set(layer.item.id, entry);
                }
                data = await entry.reader.frameAt(Math.max(0, layer.sourceTime / 120000));
              }
            } else throw new Error('找不到素材：' + layer.item.clip.assetId);
            owned.push(data);
            return data;
          })();
          flights.set(sourceKey, flight);
        }
        data = await flight;
      }
      aborted(signal);
      const geometry = layerGeometry(
        project,
        layer.visual,
        layer.item.clip.html?.width || layer.asset?.width || data.width,
        layer.item.clip.html?.height || layer.asset?.height || data.height,
        width,
        height,
        !!layer.item.clip.text
      );
      if (data.rasterBounds)
        Object.assign(geometry, {
          rasterBounds: data.rasterBounds,
          sourceWidth: data.width,
          sourceHeight: data.height
        });
      let bound;
      if (data.controlBounds) {
        const b = data.controlBounds,
          m = geometry.matrix;
        const pts = [
          [b.x / width, b.y / height],
          [(b.x + b.width) / width, b.y / height],
          [b.x / width, (b.y + b.height) / height],
          [(b.x + b.width) / width, (b.y + b.height) / height]
        ].map(([x, y]) => [
          ((m[0] * x + m[2] * y + m[4]) * project.canvas.width) / width,
          ((m[1] * x + m[3] * y + m[5]) * project.canvas.height) / height
        ]);
        const x = Math.min(...pts.map((p) => p[0])),
          y = Math.min(...pts.map((p) => p[1]));
        bound = {
          id: layer.item.id,
          locked: layer.track.locked,
          textLayout: data.textLayout
            ? {
                ...data.textLayout,
                left: { x: (pts[0][0] + pts[2][0]) / 2, y: (pts[0][1] + pts[2][1]) / 2 },
                right: { x: (pts[1][0] + pts[3][0]) / 2, y: (pts[1][1] + pts[3][1]) / 2 }
              }
            : undefined,
          x,
          y,
          width: Math.max(...pts.map((p) => p[0])) - x,
          height: Math.max(...pts.map((p) => p[1])) - y
        };
      } else bound = { id: layer.item.id, locked: layer.track.locked, ...geometry.bounds };
      const mediaReport = {
        itemId: layer.item.id,
        kind: layer.item.clip.type,
        currentTime: layer.sourceTime / 120000,
        decodedTimestamp: data.timestamp ?? null,
        readyState: 4,
        decodedFrames: this.frames + 1,
        sourceMs: performance.now() - sourceStarted,
        sourceCacheHit: !!cached,
        ...(data.nativeWaitMs !== undefined
          ? {
              nativeWaitMs: data.nativeWaitMs,
              nativeRasterMs: data.nativeRasterMs
            }
          : {}),
        ...(layer.item.clip.html
          ? {
              tickTime: data.tickTime,
              htmlWidth: data.width,
              htmlHeight: data.height,
              authoredWidth: layer.item.clip.html.width,
              authoredHeight: layer.item.clip.html.height,
              transparent: data.transparent
            }
          : {}),
        error: null
      };
      const uploadIdentity =
        cached?.uploadIdentity ||
        (layer.item.clip.html ? renderIdentity(data.identity) : data.identity) ||
        JSON.stringify([
          layer.asset.sourceIdentity || this.mediaUrl(layer.asset.id),
          layer.asset.kind === 'image' ? 'static' : data.timestamp,
          data.width,
          data.height
        ]);
      return {
        ...layer,
        data,
        geometry,
        uploadIdentity,
        bound,
        mediaReport,
        sourceKey,
        residentSource: !!cached
      };
    };
    try {
      if (this.gpu)
        retainedInputs = this.gpu.retainInputs(
          [...sourceKeys.values()].flatMap((key) => {
            const entry = this.sourceCache.peek(key);
            return entry ? [entry] : [];
          }),
          [...sources.values()].some((layer) => !!layer.item.clip.text?.template)
        );
      // Source decoding/capture is independent across items. Start HTML and
      // media IO before synchronous WASM text work, then await every source even
      // on failure so no late bitmap can escape this frame's ownership cleanup.
      const pending = new Map<string, Promise<any>>();
      for (const layer of [...sources.values()].sort(
        (a, b) => Number(!!a.item.clip.text) - Number(!!b.item.clip.text)
      ))
        pending.set(layer.item.id, prepare(layer));
      if (options?.htmlPrefetch && [...sources.values()].some((layer) => !!layer.item.clip.html))
        this.prepareHtmlAhead(project, time, width, height, activeIds, signal);
      if (options?.prefetch) {
        // Foreground requests dispatch first. Preparing their successors while
        // this frame awaits capture/raster also overlaps the second worker's
        // initial load with the first frame, instead of stalling at play start.
        await Promise.resolve();
        if (!signal?.aborted) this.prepareAhead(project, time, width, height);
      }
      const settled = await Promise.allSettled(pending.values());
      const failure = settled.find((result) => result.status === 'rejected');
      if (failure?.status === 'rejected') throw failure.reason;
      const prepared = new Map(
        [...pending.keys()].map((id, index) => [
          id,
          (settled[index] as PromiseFulfilledResult<any>).value
        ])
      );
      const collect = (layer: any) => {
        const value = prepared.get(layer.item.id);
        bounds.push(value.bound);
        media.push(value.mediaReport);
        return value;
      };
      const entries = plan.layers.map((entry) =>
        entry.kind === 'layer'
          ? { ...entry, layer: collect(entry.layer) }
          : { ...entry, from: collect(entry.from), to: collect(entry.to) }
      );
      for (const [id, text] of this.text)
        if (!used.has(id)) {
          text.player?.dispose();
          this.text.delete(id);
        }
      for (const [id, entry] of this.readers)
        if (!used.has(id)) {
          await entry.reader.close();
          this.readers.delete(id);
        }
      aborted(signal);
      if (this.disposed) throw new Error('渲染器已关闭');
      if (this.gpu) {
        let base = this.gpu.begin(plan.background, width, height),
          index = 0;
        const draw = (layer: any) => {
          const effects = layer.effects.map((fx: any) => {
            if (fx.templateId !== 'lut') return fx;
            const preset = fx.parameters.preset || 'warm';
            if (!this.luts.has(preset)) this.luts.set(preset, makeLut(preset));
            const lut = this.luts.get(preset);
            return {
              ...fx,
              lut: { size: lut.size, texture: this.gpu.lut(lut.data, lut.size, preset) }
            };
          });
          const inputKey = 'input:' + renderIdentity(layer.uploadIdentity);
          const input = layer.residentSource
            ? this.gpu.reuseInput(layer.data)
            : this.gpu.upload(
                layer.data.frame,
                inputKey,
                layer.uploadIdentity,
                layer.data.rasterBounds,
                layer.item.clip.text?.template ? 1 : 0
              );
          if (!layer.residentSource) {
            const data = layer.data;
            this.sourceCache.set(layer.sourceKey, {
              inputKey,
              uploadIdentity: layer.uploadIdentity,
              width: data.width,
              height: data.height,
              timestamp: data.timestamp,
              tickTime: data.tickTime,
              transparent: data.transparent,
              controlBounds: data.controlBounds ? { ...data.controlBounds } : undefined,
              textLayout: data.textLayout ? { ...data.textLayout } : undefined,
              rasterBounds: data.rasterBounds ? { ...data.rasterBounds } : undefined
            });
          }
          return this.gpu.layer(input, layer.geometry, effects, 'layer:' + layer.item.id);
        };
        for (const entry of entries) {
          const layer =
            entry.kind === 'layer'
              ? draw(entry.layer)
              : this.gpu.transition(
                  draw(entry.from),
                  draw(entry.to),
                  entry.transition,
                  'transition:' + entry.transition.id
                );
          base = this.gpu.blend(
            base,
            layer,
            (entry.layer || entry.from).visual.blendMode || 'normal',
            index++
          );
        }
        aborted(signal);
        await this.gpu.finish(base, signal);
      } else {
        if (this.canvas.width !== width) this.canvas.width = width;
        if (this.canvas.height !== height) this.canvas.height = height;
        const ctx = this.ctx!;
        ctx.setTransform(1, 0, 0, 1, 0, 0);
        ctx.globalAlpha = 1;
        ctx.globalCompositeOperation = 'source-over';
        ctx.clearRect(0, 0, width, height);
        ctx.fillStyle = `rgba(${plan.background
          .slice(0, 3)
          .map((v: number) => v * 255)
          .join(',')},${plan.background[3]})`;
        ctx.fillRect(0, 0, width, height);
        const draw = (
          layer: any,
          target: CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D
        ) => {
          if (layer.effects.length)
            throw new Error('特效需要 WebGPU；当前浏览器仅支持基础画布绘制');
          target.save();
          target.setTransform(
            ...(layer.geometry.matrix as [number, number, number, number, number, number])
          );
          target.globalAlpha = layer.geometry.opacity;
          target.globalCompositeOperation =
            layer.visual.blendMode === 'normal'
              ? 'source-over'
              : layer.visual.blendMode || 'source-over';
          const [x, y, w, h] = layer.geometry.crop;
          target.drawImage(
            layer.data.frame,
            x * layer.data.width,
            y * layer.data.height,
            w * layer.data.width,
            h * layer.data.height,
            0,
            0,
            1,
            1
          );
          target.restore();
        };
        for (const entry of entries) {
          if (entry.kind === 'layer') draw(entry.layer, ctx);
          else throw new Error('转场需要 WebGPU；当前浏览器仅支持基础画布绘制');
        }
      }
      this.frames++;
      const result: RenderReport = {
        backend: this.gpu ? 'webgpu' : 'canvas2d',
        time,
        width,
        height,
        frameMs: performance.now() - started,
        ...(this.gpu?.stats() || {
          uploadCount: 0,
          uploadCacheHits: 0,
          textureBytes: 0,
          textureBudget: 0,
          resourceBytes: 0,
          peakResourceBytes: 0,
          workingSetBytes: 0,
          externalTextureBytes: 0,
          effectHits: 0,
          layerHits: 0,
          compositeHits: 0,
          transitionHits: 0,
          gpuPasses: 0,
          evictions: 0
        }),
        graphBuilds: this.graph.builds,
        textLayoutCount: this.textLayoutCount,
        textPrefetch: this.textPool?.stats(),
        bounds,
        media
      };
      this.lastIdentity = frameIdentity;
      this.lastReport = result;
      return result;
    } finally {
      await this.gpu?.abort();
      for (const frame of owned) frame.close();
    }
  }
  async readPixels(): Promise<Uint8Array> {
    await this.ready;
    if (this.gpu) return this.gpu.readPixels();
    return Uint8Array.from(
      this.ctx!.getImageData(0, 0, this.canvas.width, this.canvas.height).data
    );
  }
  async setTextureBudget(bytes: number) {
    await this.ready;
    this.gpu?.setBudget(bytes);
  }
  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    this.textPool?.dispose();
    for (const text of this.text.values()) text.player?.dispose();
    this.text.clear();
    for (const entry of this.readers.values()) void entry.reader.close().catch(() => {});
    this.readers.clear();
    this.sourceCache.clear();
    this.html.dispose();
    this.gpu?.dispose();
  }
}
