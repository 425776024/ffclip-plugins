export interface SharedGpu {
  device: any;
  adapter: any;
  pipelines: Map<string, Promise<any>>;
}
export function sharedGpu(): Promise<SharedGpu | null>;
export function createGpuCompositor(canvas: HTMLCanvasElement | OffscreenCanvas, options?: {textureBudgetBytes?:number}): Promise<any>;
