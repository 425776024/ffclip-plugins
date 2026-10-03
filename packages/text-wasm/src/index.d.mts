export interface TemplateBundle {
  manifest: Record<string, unknown>;
  composition: Record<string, unknown>;
  animation: Record<string, unknown>;
  effectProgram: Record<string, unknown>;
  assets: Map<string, { bytes: Uint8Array; mediaType: string }>;
}
export interface Rect { x: number; y: number; width: number; height: number }
export interface RenderOptions { timeUs?: number; width?: number; height?: number; profile?: boolean }
/** Opt-in wall times at the synchronous JS/WASM boundary; readback/hashing are excluded. */
export interface TextFrameTimings {
  nativeRenderMs: number; resultReadMs: number; pixelPointerMs: number;
  pixelAllocationMs: number; rowCopyMs: number; renderTotalMs: number;
  imageDataMs?: number; clearRectMs?: number; putImageDataMs?: number; drawTotalMs?: number;
  nativeTimings?: Record<string, unknown>;
}
export interface LoadInfo {
  ok: true; profile: 'wasm-raster-v1'; durationUs: number;
  referenceWidth: number; referenceHeight: number;
  requiresBrowserComposition: boolean;
  warnings: Array<{ code: string; message: string }>;
}
export interface TextFrame {
  ok: true; profile: 'wasm-raster-v1'; data: Uint8ClampedArray;
  width: number; height: number; rowBytes: number; byteLength: number;
  originX: number; originY: number; timeUs: number;
  logicalBounds: Rect; inkBounds: Rect; cacheBytes: number;
  /** Stable editor geometry in output pixels, never a pixel scissor. */
  controlBounds: Rect; visualExtent: Rect;
  timings?: TextFrameTimings;
  compositionPlan?: {profile:'browser-composition-experimental-v1';surface:'output-canvas';decorations:Array<Record<string,unknown>>;effects:Array<Record<string,unknown>>};
  warnings: Array<{ code: string; message: string }>;
}
export interface TextRenderer {
  /** Base layout only. Does not evaluate template animation or material passes. */
  prepareSdfMesh(options?: { width?: number; height?: number; range?: number }): SdfMesh;
  registerAsset(id: string, mediaType: string, bytes: Uint8Array | ArrayBuffer): { ok: true; digest: string };
  loadTemplate(bundle: TemplateBundle, options?: { bindings?: Record<string, string>; allowRasterFallback?: boolean; experimentalComposition?: boolean; fallbackFonts?: Array<{ id: string; family: string }> }): Promise<LoadInfo>;
  setText(text: string, bindingId?: string): LoadInfo;
  setBindings(bindings: Record<string, string>): LoadInfo;
  render(options?: RenderOptions): TextFrame;
  draw(canvas: HTMLCanvasElement | OffscreenCanvas, options?: RenderOptions): TextFrame;
  readonly info: LoadInfo | null;
  dispose(): void;
}
export interface SdfMesh {
  width: number; height: number; range: number; glyphCount: number;
  distanceCount: number; shapeCount: number;
  distanceVertices: Float32Array; shapeVertices: Float32Array;
  scope: 'base-layout-outline-only';
}
export interface TextEngine {
  readonly profile: 'wasm-raster-v1';
  readonly capabilities: { gpu: false; nativeSdfParity: false; postEffects: false; animatedMedia: false; rasterPipelineLanes: number };
  createRenderer(): TextRenderer;
  dispose(): void;
}
export function createTextEngine(options?: { wasmUrl?: string | URL; wasmBinary?: Uint8Array | ArrayBuffer; printErr?: (message: string) => void }): Promise<TextEngine>;
export function loadTextTemplate(manifestUrl: string | URL, options?: { fetch?: typeof fetch; signal?: AbortSignal }): Promise<TemplateBundle>;
export function sha256(bytes: Uint8Array | ArrayBuffer): Promise<string>;
