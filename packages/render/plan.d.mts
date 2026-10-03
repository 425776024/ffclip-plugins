import type { Project } from '../core/project.mjs';
export function previewExtent(
  canvas: Project['canvas'],
  width: number,
  height: number,
  dpr?: number,
  playing?: boolean
): { width: number; height: number };
export function frameTime(index: number, frameRate: Project['frameRate']): number;
export function previewSampleTime(
  time: number,
  frameRate: Project['frameRate'],
  playing: boolean
): number;
export function parseColor(value: unknown, fallback?: number[]): number[];
export function layerGeometry(
  project: Project,
  visual: any,
  sourceWidth: number,
  sourceHeight: number,
  width: number,
  height: number,
  fullCanvas?: boolean
): any;
export function buildRenderPlan(
  project: Project,
  time: number,
  width?: number,
  height?: number
): any;
export function makeLut(preset?: string, size?: number): { data: Uint8Array; size: number };
