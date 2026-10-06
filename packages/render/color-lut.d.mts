import type { EffectInstance } from '../core/types.js';
export function colorTransform(
  parameters?: Record<string, number | string>
): (rgb: number[]) => number[];
export function cubeTransform(source: string): (rgb: number[]) => number[];
export function makeColorLut(
  effect: Pick<EffectInstance, 'templateId' | 'parameters'>,
  size?: number
): { data: Uint8Array; size: number };
