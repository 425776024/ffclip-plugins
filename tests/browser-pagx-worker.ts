import { PagxFrameSource } from '../packages/render/pagx';
import { HtmlFrameClient } from '../packages/render/html';
import { SceneRenderer } from '../packages/render/renderer';
import { exportProject } from '../packages/render/export';
import type { PagxContent } from '../packages/core/types';
const pixels = (frame: CanvasImageSource, width: number, height: number) => {
  const canvas = new OffscreenCanvas(width, height),
    ctx = canvas.getContext('2d')!;
  ctx.drawImage(frame, 0, 0);
  return ctx.getImageData(0, 0, width, height).data;
};
const hash = (bytes: Uint8Array | Uint8ClampedArray) => bytes.reduce((h, b) => (h * 31 + b) | 0, 0);
self.onmessage = async ({ data }) => {
  const source = new PagxFrameSource();
  const html = new HtmlFrameClient();
  let renderer: SceneRenderer | undefined;
  try {
    const frames = [];
    for (let i = 0; i < data.clips.length; i++) {
      const clip = data.clips[i] as PagxContent;
      const initial = await source.frame('test', clip, 0);
      const before = hash(pixels(initial.frame, 320, 180));
      const middle = await source.frame('test', clip, 60000);
      const bytes = pixels(middle.frame, 320, 180),
        middleHash = hash(bytes);
      const oldAfterSeek = hash(pixels(initial.frame, 320, 180));
      let minX = 320,
        maxX = 0,
        nonzero = 0;
      for (let y = 0; y < 180; y++)
        for (let x = 0; x < 320; x++)
          if (bytes[(y * 320 + x) * 4 + 3]) {
            minX = Math.min(x, minX);
            maxX = Math.max(x, maxX);
            nonzero++;
          }
      const rewound = await source.frame('test', clip, 0);
      frames.push({
        before,
        middleHash,
        oldAfterSeek,
        rewound: hash(pixels(rewound.frame, 320, 180)),
        minX,
        maxX,
        nonzero,
        pixel: Array.from(bytes.slice((30 * 320 + 60) * 4, (30 * 320 + 60) * 4 + 4))
      });
      initial.close();
      middle.close();
      rewound.close();
    }
    const canvas = new OffscreenCanvas(320, 180);
    const title: any = {};
    for (const kind of ['html', 'pagx']) {
      const positions = [];
      for (const time of [0, 60000]) {
        const frame =
          kind === 'html'
            ? await html.frame('title', data.titleHtml, time)
            : await source.frame('title', data.titlePagx, time);
        const bytes = pixels(frame.frame, 320, 180);
        let x = 320,
          alpha = 0;
        for (let i = 0; i < bytes.length; i += 4) {
          if (bytes[i + 3] > 16) x = Math.min(x, (i / 4) % 320);
          alpha = Math.max(alpha, bytes[i + 3]);
        }
        positions.push({ x, alpha });
        frame.close();
      }
      title[kind] = { dx: positions[1].x - positions[0].x, alpha: positions[1].alpha };
    }
    const graphicFrame = await source.frame('graphic', data.graphic, 0);
    const graphicBytes = pixels(graphicFrame.frame, 320, 180);
    const graphic = { green: 0, text: 0, initial: hash(graphicBytes), end: 0 };
    for (let y = 0; y < 180; y++)
      for (let x = 0; x < 320; x++) {
        const i = (y * 320 + x) * 4;
        if (x < 40 && graphicBytes[i + 1] > 180 && graphicBytes[i + 3] > 200) graphic.green++;
        if (
          x >= 60 &&
          graphicBytes[i] > 180 &&
          graphicBytes[i + 1] > 180 &&
          graphicBytes[i + 3] > 100
        )
          graphic.text++;
      }
    const graphicEnd = await source.frame('graphic', data.graphic, 200000);
    graphic.end = hash(pixels(graphicEnd.frame, 320, 180));
    graphicFrame.close();
    graphicEnd.close();
    renderer = new SceneRenderer(canvas, (id: string) => data.urls[id]);
    const sceneHashes = [];
    for (const t of [0, 60000, 120000, 60000]) {
      await renderer.render(data.project, t, 320, 180, new AbortController().signal);
      sceneHashes.push(hash(await renderer.readPixels()));
    }
    const start = performance.now();
    for (let i = 0; i < 60; i++) {
      const frame = await source.frame('speed', data.clips[0], i * 4000);
      frame.close();
    }
    const pagxMs = performance.now() - start;
    const chunks: { position: number; data: Uint8Array }[] = [];
    let encoding: unknown;
    const result = await exportProject(
      data.project,
      'mp4',
      data.urls,
      {
        async configure(value) {
          encoding = value;
        },
        async write(position, bytes) {
          chunks.push({ position, data: bytes.slice() });
        },
        async audio() {
          throw Error('Unexpected PCM fallback');
        },
        async frame() {
          throw Error('Unexpected raster fallback');
        },
        progress() {}
      },
      new AbortController().signal,
      false
    );
    const size = Math.max(...chunks.map((c) => c.position + c.data.length)),
      output = new Uint8Array(size);
    for (const chunk of chunks) output.set(chunk.data, chunk.position);
    self.postMessage(
      { frames, graphic, title, sceneHashes, pagxMs, encoding, result, output },
      { transfer: [output.buffer] }
    );
  } catch (e) {
    self.postMessage({ error: e instanceof Error ? e.stack : String(e) });
  } finally {
    renderer?.dispose();
    source.dispose();
    html.dispose();
  }
};
