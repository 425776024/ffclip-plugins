import type { VisualProperties } from '../../packages/core/types';

export type ResizeCorner = 'tl' | 'tr' | 'bl' | 'br';
export type TextWidthEdge = 'left' | 'right';
export type PreviewTransform = Partial<VisualProperties> & { layoutWidth?: number };
type Point = { x: number; y: number };
type Bounds = Point & { width: number; height: number };

/** Uniform scaling of the presented control frame, with its opposite corner fixed. */
export function cornerResize(
  bounds: Bounds,
  visual: VisualProperties,
  pivot: Point,
  corner: ResizeCorner
) {
  const right = corner.endsWith('r'),
    bottom = corner.startsWith('b');
  const fixed = {
    x: bounds.x + (right ? 0 : bounds.width),
    y: bounds.y + (bottom ? 0 : bounds.height)
  };
  const vector = { x: (right ? 1 : -1) * bounds.width, y: (bottom ? 1 : -1) * bounds.height };
  const squared = vector.x ** 2 + vector.y ** 2;
  const minimum = Math.max(0.01 / visual.scaleX, 0.01 / visual.scaleY);
  const maximum = Math.min(20 / visual.scaleX, 20 / visual.scaleY);
  return (dx: number, dy: number) => {
    const ratio = Math.max(
      minimum,
      Math.min(maximum, squared > 1e-12 ? 1 + (dx * vector.x + dy * vector.y) / squared : 1)
    );
    return {
      positionX: visual.positionX + (1 - ratio) * (fixed.x - pivot.x),
      positionY: visual.positionY + (1 - ratio) * (fixed.y - pivot.y),
      scaleX: Math.max(0.01, Math.min(20, visual.scaleX * ratio)),
      scaleY: Math.max(0.01, Math.min(20, visual.scaleY * ratio))
    };
  };
}

/** Width reflow leaves font/scale untouched and pins the opposite text-box edge. */
export function textWidthResize(
  layout: { width: number; minimumWidth: number; left: Point; right: Point },
  visual: VisualProperties,
  edge: TextWidthEdge
) {
  const vector = { x: layout.right.x - layout.left.x, y: layout.right.y - layout.left.y };
  const length = Math.hypot(vector.x, vector.y);
  const axis = { x: vector.x / length, y: vector.y / length };
  const direction = edge === 'right' ? 1 : -1;
  return (dx: number, dy: number, fromCenter = false) => {
    const multiplier = fromCenter ? 2 : 1;
    const width = Math.max(
      layout.minimumWidth,
      Math.min(
        65536,
        layout.width + (direction * (dx * axis.x + dy * axis.y) * multiplier) / visual.scaleX
      )
    );
    const travel = fromCenter ? 0 : (direction * (width - layout.width) * visual.scaleX) / 2;
    return {
      positionX: visual.positionX + axis.x * travel,
      positionY: visual.positionY + axis.y * travel,
      layoutWidth: width
    };
  };
}
