import { htmlSourceTime } from './html';

export interface RasterBounds {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** Retain one transparent texel around native finished pixels for linear sampling. */
export function sourceRasterBounds(width: number, height: number, bounds?: RasterBounds) {
  if (
    !bounds ||
    ![width, height, bounds.x, bounds.y, bounds.width, bounds.height].every(Number.isSafeInteger) ||
    width < 1 ||
    height < 1 ||
    bounds.x < 0 ||
    bounds.y < 0 ||
    bounds.width < 1 ||
    bounds.height < 1 ||
    bounds.x + bounds.width > width ||
    bounds.y + bounds.height > height
  )
    return;
  const x = Math.max(0, bounds.x - 1),
    y = Math.max(0, bounds.y - 1);
  const right = Math.min(width, bounds.x + bounds.width + 1),
    bottom = Math.min(height, bounds.y + bounds.height + 1);
  if (x === 0 && y === 0 && right === width && bottom === height) return;
  return { x, y, width: right - x, height: bottom - y };
}

export interface ResidentSource {
  inputKey: string;
  uploadIdentity: string;
  width: number;
  height: number;
  timestamp?: number;
  tickTime?: number;
  transparent?: boolean;
  controlBounds?: { x: number; y: number; width: number; height: number };
  textLayout?: { width: number; minimumWidth: number };
  /** Logical dimensions above remain authored; only this subrect occupies the texture. */
  rasterBounds?: RasterBounds;
}

/** Exact source dependencies exclude placement, effects and audio. No time quantization. */
export function sourceFrameKey(
  layer: any,
  width: number,
  height: number,
  projectWidth: number,
  projectHeight: number,
  mediaUrl: (id: string) => string
) {
  const { clip } = layer.item;
  if (clip.html)
    return JSON.stringify(['html', clip.html, htmlSourceTime(clip.html, layer.sourceTime)]);
  if (clip.text)
    return JSON.stringify([
      'text',
      clip.text,
      width,
      height,
      projectWidth,
      projectHeight,
      clip.text.template ? layer.sourceTime : null
    ]);
  return JSON.stringify([
    'media',
    layer.asset,
    mediaUrl(layer.asset.id),
    layer.asset.kind === 'video' ? layer.sourceTime : null
  ]);
}

/** Metadata only; GPU residency is checked before any source work is skipped. */
export class ResidentSourceCache {
  private entries = new Map<string, ResidentSource>();
  private keyChars = 0;
  constructor(
    readonly maximumEntries = 512,
    readonly maximumKeyChars = 4 * 1048576
  ) {}
  peek(key: string) {
    return this.entries.get(key);
  }
  get(key: string, resident: (source: ResidentSource) => boolean) {
    const value = this.entries.get(key);
    if (!value) return;
    if (!resident(value)) {
      this.delete(key);
      return;
    }
    this.entries.delete(key);
    this.entries.set(key, value);
    return value;
  }
  set(key: string, value: ResidentSource) {
    this.delete(key);
    if (key.length > this.maximumKeyChars || this.maximumEntries < 1) return;
    this.entries.set(key, value);
    this.keyChars += key.length;
    while (this.entries.size > this.maximumEntries || this.keyChars > this.maximumKeyChars)
      this.delete(this.entries.keys().next().value!);
  }
  delete(key: string) {
    if (this.entries.delete(key)) this.keyChars -= key.length;
  }
  clear() {
    this.entries.clear();
    this.keyChars = 0;
  }
  get size() {
    return this.entries.size;
  }
}
