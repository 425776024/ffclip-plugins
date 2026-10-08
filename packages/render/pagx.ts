import type { PagxContent } from '../core/types';
import { chooseTemplateFonts } from '../text-wasm/src/system-fonts.mjs';
import { parsePagx } from '../core/pagx.mjs';
import { XMLSerializer } from '@xmldom/xmldom';
import { aliasPagxFont } from './pagx-fonts.mjs';

const canvases = new Map<string, OffscreenCanvas | HTMLCanvasElement>();
let runtime: Promise<any> | undefined;
let catalog: Promise<any> | undefined;
const fontBytes = new Map<string, Uint8Array>();
let nextId = 0;
async function loadRuntime() {
  if (!runtime)
    runtime = (async () => {
      const url = '/pagx-runtime/pagx-viewer.js';
      const { PAGXInit } = await import(/* @vite-ignore */ url);
      return PAGXInit({
        locateFile: (file: string) =>
          new URL('/pagx-runtime/' + file, globalThis.location.origin).href,
        videocutCanvases: canvases
      });
    })().catch((error) => {
      runtime = undefined;
      throw error;
    });
  return runtime;
}
async function documentFonts(xml: string) {
  if (!/<Text(?:\s|>)/.test(xml)) return { xml, fonts: [] };
  catalog ||= fetch('/api/fonts')
    .then(async (response) => {
      if (!response.ok) throw new Error('无法读取 PAGX 系统字体');
      return response.json();
    })
    .catch((error) => {
      catalog = undefined;
      throw error;
    });
  const data = await catalog,
    defaults = chooseTemplateFonts(data);
  const selected = new Map<string, any>();
  const document = parsePagx(xml);
  for (const node of Array.from(document.getElementsByTagName('Text')) as any[]) {
    const family = node.getAttribute('fontFamily') || 'system';
    const names = family.split(',').map((name: string) => name.trim().replace(/^['"]|['"]$/g, ''));
    const style = node.getAttribute('fontStyle') || 'Regular';
    const weight = /black|heavy/i.test(style)
      ? 900
      : /semi.?bold|demi/i.test(style)
        ? 600
        : /bold/i.test(style)
          ? 700
          : /medium/i.test(style)
            ? 500
            : /light/i.test(style)
              ? 300
              : 400;
    const resolvedFamily =
      names.find((name: string) => data.fonts.some((f: any) => f.family === name)) ||
      defaults.cjk.family;
    const candidates = data.fonts.filter((f: any) => f.family === resolvedFamily);
    const score = (f: any) =>
      Math.abs(f.weight - weight) +
      ((/italic|oblique/i.test(style) ? f.slant === 'upright' : f.slant !== 'upright') ? 1000 : 0);
    const face =
      candidates.find((f: any) => f.subfamily.toLowerCase() === style.toLowerCase()) ||
      candidates.sort((a: any, b: any) => score(a) - score(b))[0];
    // Resolve aliases and styles to the face actually registered in WASM. Otherwise
    // editable Semibold/Bold headings silently fall back to the regular CJK face.
    node.setAttribute('fontFamily', 'VideoCut-' + face.id);
    node.setAttribute('fontStyle', face.subfamily);
    selected.set(face.id, face);
  }
  selected.set(defaults.cjk.id, defaults.cjk);
  if (selected.size > 8) throw new Error('PAGX 单个文档最多使用 8 种字体');
  const fonts = await Promise.all(
    [...selected.keys()].map(async (id) => {
      let bytes = fontBytes.get(id);
      if (!bytes) {
        const response = await fetch('/api/fonts/' + encodeURIComponent(id));
        if (!response.ok) throw new Error('无法读取 PAGX 字体数据');
        bytes = aliasPagxFont(new Uint8Array(await response.arrayBuffer()), 'VideoCut-' + id);
        while (
          fontBytes.size &&
          [...fontBytes.values()].reduce((sum, b) => sum + b.byteLength, bytes.byteLength) >
            96 * 1024 * 1024
        )
          fontBytes.delete(fontBytes.keys().next().value!);
        fontBytes.set(id, bytes);
      }
      return bytes;
    })
  );
  return { fonts, xml: new XMLSerializer().serializeToString(document) };
}
const check = (signal?: AbortSignal) => {
  if (signal?.aborted) throw new DOMException('PAGX 帧已过期', 'AbortError');
};
type Entry = {
  key: string;
  selector: string;
  canvas: OffscreenCanvas | HTMLCanvasElement;
  view: any;
};

/** An absolute-time GPU source. No screenshots, PNG encoding, HTTP frames or free-running clock. */
export class PagxFrameSource {
  private entries = new Map<string, Entry>();
  private disposed = false;
  retain(active: ReadonlySet<string>) {
    for (const id of this.entries.keys()) if (!active.has(id)) this.release(id);
  }
  private release(id: string) {
    const entry = this.entries.get(id);
    if (!entry) return;
    entry.view.destroy();
    canvases.delete(entry.selector);
    this.entries.delete(id);
  }
  async frame(id: string, content: PagxContent, time: number, signal?: AbortSignal) {
    check(signal);
    if (this.disposed) throw new Error('PAGX 渲染器已关闭');
    const key = JSON.stringify(content);
    let entry = this.entries.get(id);
    if (entry?.key !== key) {
      this.release(id);
      const [module, resolved] = await Promise.all([loadRuntime(), documentFonts(content.xml)]);
      check(signal);
      if (this.disposed) throw new Error('PAGX 渲染器已关闭');
      if (this.entries.size >= 8) throw new Error('同时显示的 PAGX 动画最多 8 个');
      const bytes = [...this.entries.values()].reduce(
        (sum, e) => sum + e.canvas.width * e.canvas.height * 4,
        content.width * content.height * 4
      );
      if (bytes > 128 * 1024 * 1024) throw new Error('PAGX 图层画布超过 128 MiB');
      const canvas =
        typeof OffscreenCanvas !== 'undefined'
          ? new OffscreenCanvas(content.width, content.height)
          : document.createElement('canvas');
      canvas.width = content.width;
      canvas.height = content.height;
      const selector = '#videocut-pagx-' + ++nextId;
      canvases.set(selector, canvas);
      const view = module.PAGXView.init(selector);
      if (!view) {
        canvases.delete(selector);
        throw new Error('无法创建 PAGX WebGL2 渲染器');
      }
      try {
        for (const font of resolved.fonts) view.registerFonts(font, new Uint8Array());
        view.setBackgroundColor(content.transparent ? '#00000000' : '#000000');
        view.loadPAGX(new TextEncoder().encode(resolved.xml));
        if (view.contentWidth !== content.width || view.contentHeight !== content.height)
          throw new Error('PAGX 加载失败或画布尺寸不匹配');
        view.pause();
        view.stop();
        view.setLoop(false);
        view.updateSize();
        view.updateZoomScaleAndOffset(1, 0, 0);
        entry = { key, selector, canvas, view };
        this.entries.set(id, entry);
      } catch (error) {
        view.destroy();
        canvases.delete(selector);
        throw error;
      }
    }
    const tickTime = Math.max(0, Math.min(Math.round(time), content.duration - 1));
    entry!.view.setCurrentTimeMicros(Math.round((tickTime * 1000000) / 120000));
    // Cache entries must own immutable pixels: the source canvas is reused on the
    // next seek. ImageBitmap copies stay in the graphics path, without PNG/HTTP.
    const frame = await createImageBitmap(entry!.canvas);
    if (signal?.aborted || this.disposed) {
      frame.close();
      check(signal);
      throw new Error('PAGX 渲染器已关闭');
    }
    return {
      frame,
      width: content.width,
      height: content.height,
      timestamp: tickTime / 120000,
      tickTime,
      transparent: content.transparent,
      identity: JSON.stringify(['pagx', key, tickTime]),
      close() {
        frame.close();
      }
    };
  }
  dispose() {
    this.disposed = true;
    for (const id of this.entries.keys()) this.release(id);
  }
}
