import { PagxFrameSource } from '../packages/render/pagx';
import { HtmlFrameClient } from '../packages/render/html';
import { SceneRenderer } from '../packages/render/renderer';
import { exportProject } from '../packages/render/export';

const hash = (bytes: Uint8Array | Uint8ClampedArray) => bytes.reduce((h, b) => (h * 31 + b) | 0, 0);
function stats(values: number[]) {
  const sorted = [...values].sort((a, b) => a - b);
  return {
    count: values.length,
    mean: values.reduce((a, b) => a + b, 0) / values.length,
    p50: sorted[Math.floor(sorted.length * 0.5)],
    p95: sorted[Math.floor(sorted.length * 0.95)],
    max: sorted.at(-1),
    over33ms: values.filter((v) => v > 1000 / 30).length
  };
}
function delta(a: Uint8ClampedArray, b: Uint8ClampedArray) {
  let error = 0,
    foreground = 0,
    changed = 0,
    intersection = 0,
    union = 0,
    alphaError = 0;
  for (let i = 0; i < a.length; i += 4) {
    const aa = a[i + 3] / 255,
      ba = b[i + 3] / 255;
    if (aa > 0.03 || ba > 0.03) {
      foreground++;
      let max = 0;
      for (let c = 0; c < 3; c++) {
        const d = Math.abs(a[i + c] * aa - b[i + c] * ba);
        error += d;
        max = Math.max(max, d);
      }
      if (max > 32 || Math.abs(a[i + 3] - b[i + 3]) > 32) changed++;
      alphaError += Math.abs(a[i + 3] - b[i + 3]);
    }
    if (aa > 0.06 || ba > 0.06) union++;
    if (aa > 0.06 && ba > 0.06) intersection++;
  }
  return {
    foregroundPixels: foreground,
    foregroundMae: error / (foreground * 3 || 1),
    changedForegroundPercent: (100 * changed) / (foreground || 1),
    alphaMae: alphaError / (foreground || 1),
    alphaIoU: union ? intersection / union : 1
  };
}
self.onmessage = async ({ data }) => {
  const files: any[] = [];
  let renderer: SceneRenderer | undefined;
  const source = new PagxFrameSource(),
    html = new HtmlFrameClient();
  try {
    const { width, height } = data.html || data.project?.canvas || { width: 1920, height: 1080 };
    if (data.mode === 'native-quality') {
      const reports = [];
      for (const entry of data.entries) {
        const frames = [];
        const content = entry.pagx;
        const canvas = new OffscreenCanvas(content.width, content.height),
          ctx = canvas.getContext('2d')!;
        let first: Uint8ClampedArray | undefined;
        const sampleTimes = [0, 0.6, 2, 4, 7.8, 1.5, 2.4, 5.4, 6.8, 0.6];
        for (const [index, seconds] of sampleTimes.entries()) {
          const result = await source.frame('native', content, Math.round(seconds * 120000));
          ctx.clearRect(0, 0, canvas.width, canvas.height);
          ctx.drawImage(result.frame, 0, 0);
          result.close();
          const pixels = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
          const nonzero = pixels.reduce((n, v, i) => n + (i % 4 === 3 && v > 8 ? 1 : 0), 0);
          frames.push({
            seconds,
            nonzero,
            hash: hash(pixels),
            coverage: (entry.regions || []).map(([left, top, w, h]: number[]) => {
              let count = 0;
              for (let y = top; y < top + h; y++)
                for (let x = left; x < left + w; x++)
                  if (pixels[(y * canvas.width + x) * 4 + 3] > 127) count++;
              return count;
            }),
            probes: (entry.probes || []).map(([x, y]: number[]) =>
              Array.from(pixels.slice((y * canvas.width + x) * 4, (y * canvas.width + x) * 4 + 4))
            ),
            ...(index === sampleTimes.length - 1 ? { rewind: delta(first!, pixels) } : {})
          });
          if (index === 1) first = pixels;
          if (index > 0 && index < sampleTimes.length - 1)
            files.push({
              name: `${entry.id}-${seconds}.png`,
              bytes: new Uint8Array(
                await (await canvas.convertToBlob({ type: 'image/png' })).arrayBuffer()
              )
            });
          if (index === 3) {
            const poster = new OffscreenCanvas(768, 432);
            poster.getContext('2d')!.drawImage(canvas, 0, 0, 768, 432);
            files.push({
              name: `poster-${entry.id}.png`,
              bytes: new Uint8Array(
                await (await poster.convertToBlob({ type: 'image/png' })).arrayBuffer()
              )
            });
          }
        }
        reports.push({ id: entry.id, frames });
        self.postMessage({ progress: true, completed: reports.length });
      }
      self.postMessage({ reports, files });
      return;
    }
    if (data.mode === 'scene-quality') {
      renderer = new SceneRenderer(new OffscreenCanvas(width, height), (id) => data.urls[id]);
      const canvas = new OffscreenCanvas(width, height),
        ctx = canvas.getContext('2d')!;
      for (const seconds of data.times) {
        await renderer.render(data.project, Math.round(seconds * 120000), width, height);
        ctx.putImageData(
          new ImageData(new Uint8ClampedArray(await renderer.readPixels()), width, height),
          0,
          0
        );
        files.push({
          name: `${data.id}-${seconds}.png`,
          bytes: new Uint8Array(
            await (await canvas.convertToBlob({ type: 'image/png' })).arrayBuffer()
          )
        });
      }
      self.postMessage({ files });
      return;
    }
    if (data.mode === 'quality') {
      const frames = [];
      let previous: any;
      const canvas = new OffscreenCanvas(width, height),
        ctx = canvas.getContext('2d')!;
      const read = async (frame: CanvasImageSource, name: string) => {
        ctx.clearRect(0, 0, width, height);
        ctx.drawImage(frame, 0, 0);
        const pixels = ctx.getImageData(0, 0, width, height).data;
        files.push({
          name,
          bytes: new Uint8Array(
            await (await canvas.convertToBlob({ type: 'image/png' })).arrayBuffer()
          )
        });
        return pixels;
      };
      for (const [i, seconds] of data.times.entries()) {
        const a = await html.frame('original', data.html, Math.round(seconds * 120000));
        const b = await source.frame('converted', data.pagx, Math.round(seconds * 120000));
        const ap = await read(a.frame, `${i}-html.png`),
          bp = await read(b.frame, `${i}-pagx.png`);
        frames.push({
          seconds,
          ...delta(ap, bp),
          htmlHash: hash(ap),
          pagxHash: hash(bp),
          ...(previous && seconds === previous.seconds
            ? { repeatDelta: { html: delta(previous.a, ap), pagx: delta(previous.b, bp) } }
            : {})
        });
        if (i === 1) previous = { seconds, a: ap, b: bp };
        a.close();
        b.close();
      }
      self.postMessage({ frames, files });
      return;
    }
    if (data.mode === 'render') {
      const started = performance.now();
      renderer = new SceneRenderer(new OffscreenCanvas(width, height), (id) => data.urls[id]);
      const first = await renderer.render(data.project, 0, width, height);
      await renderer.readPixels();
      const coldMs = performance.now() - started;
      const frameMs = [],
        sourceMs = [],
        renderMs = [];
      let checksum = 0;
      for (let i = 1; i < data.frames; i++) {
        const start = performance.now();
        const r = await renderer.render(data.project, i * 4000, width, height, undefined, {
          prefetch: false,
          htmlPrefetch: false
        });
        const pixels = await renderer.readPixels();
        frameMs.push(performance.now() - start);
        renderMs.push(r.frameMs);
        sourceMs.push(r.media.reduce((n: any, m: any) => n + m.sourceMs, 0));
        checksum ^= pixels[i % pixels.length];
        if (i % 30 === 0) self.postMessage({ progress: true, frame: i, total: data.frames });
      }
      self.postMessage({
        coldMs,
        frameMs: stats(frameMs),
        sourceMs: stats(sourceMs),
        renderMs: stats(renderMs),
        backend: first.backend,
        checksum,
        files
      });
      return;
    }
    if (data.mode === 'export') {
      const started = performance.now(),
        chunks: { position: number; bytes: Uint8Array }[] = [];
      let encoding;
      const result = await exportProject(
        data.project,
        'mp4',
        data.urls,
        {
          async configure(value) {
            encoding = value;
          },
          async write(position, bytes) {
            chunks.push({ position, bytes: bytes.slice() });
          },
          async audio() {
            throw Error('Unexpected PCM fallback');
          },
          async frame() {
            throw Error('Unexpected raster fallback');
          },
          progress(value) {
            if (value.completed % 30 === 0) self.postMessage({ progress: true, ...value });
          }
        },
        new AbortController().signal,
        false
      );
      const elapsedMs = performance.now() - started;
      const bytes = new Uint8Array(Math.max(...chunks.map((c) => c.position + c.bytes.length)));
      for (const c of chunks) bytes.set(c.bytes, c.position);
      files.push({ name: 'export.mp4', bytes });
      self.postMessage({ elapsedMs, encoding, result, files });
      return;
    }
    throw Error('Unknown benchmark mode');
  } catch (error) {
    self.postMessage({ error: error instanceof Error ? error.stack : String(error) });
  } finally {
    renderer?.dispose();
    source.dispose();
    html.dispose();
  }
};
