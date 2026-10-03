import type { TemplateBundle, Rect } from '../text-wasm/src/index.mjs';
type Canvas = { width: number; height: number };
export interface TextLayoutInfo {
  width: number;
  minimumWidth: number;
}
export function applyTemplateLayout(
  bundle: TemplateBundle,
  width: number | undefined,
  canvas: Canvas
): TemplateBundle;
export function templateLayoutInfo(bundle: TemplateBundle, canvas: Canvas): TextLayoutInfo;
export function templateControlBounds(
  bundle: TemplateBundle,
  bounds: Rect,
  width: number,
  height: number
): Rect;
export function wrapText(
  content: string,
  width: number | undefined,
  measure: (text: string) => number
): string[];
