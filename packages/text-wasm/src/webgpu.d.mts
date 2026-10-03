import type { SdfMesh } from './index.mjs';
export interface GpuEffectOptions {
  effect?: 'sdf' | 'gaussian' | 'soft-glow';
  fill?: number[];
  stroke?: number[];
  strokeWidth?: number;
  radius?: number;
  sigma?: number;
  exposure?: number;
  threshold?: number;
  glowColor?: number[];
  displayGlow?: boolean;
}
export interface TextGpuEffects {
  readonly profile: 'webgpu-effects-experimental-v1';
  readonly adapterInfo: {
    vendor: string;
    architecture: string;
    description: string;
    isFallbackAdapter: boolean | null;
  };
  setMesh(mesh: SdfMesh): void;
  setSource(source: { width: number; height: number; data: Uint8Array }): void;
  render(options?: GpuEffectOptions): {
    profile: string;
    effect: string;
    width: number;
    height: number;
    submissionMs: number;
    generation: number;
    source: string;
    note: string;
  };
  completed(): Promise<void>;
  readPixels(options?: {
    distanceField?: boolean;
  }): Promise<{ width: number; height: number; data: Uint8Array; alphaMode: 'premultiplied' }>;
  dispose(): void;
}
export function createTextGpuEffects(
  canvas: HTMLCanvasElement | OffscreenCanvas
): Promise<TextGpuEffects>;
