import type { TextEngine, TemplateBundle, LoadInfo, Rect } from './index.mjs';
export interface CompositionFrameInfo {
  timeUs: number;
  totalMs: number;
  textMs: number;
  controlBounds: Rect;
  visualExtent: Rect;
  mediaFrames: number[];
  compositionPlan: Record<string, unknown> | null;
  warnings: LoadInfo['warnings'];
}
export interface BrowserTextComposition {
  readonly profile: 'browser-composition-experimental-v1';
  readonly info: LoadInfo;
  readonly adapter: { vendor?: string; architecture?: string; isFallbackAdapter: boolean };
  setText(text: string): LoadInfo;
  render(options?: {
    timeUs?: number;
    background?: boolean;
    postEffects?: boolean;
  }): Promise<CompositionFrameInfo>;
  readPixels(): Promise<{ width: number; height: number; data: Uint8Array }>;
  dispose(): void;
}
export function createBrowserTextComposition(
  engine: TextEngine,
  canvas: HTMLCanvasElement | OffscreenCanvas,
  bundle: TemplateBundle,
  options?: {
    width?: number;
    height?: number;
    bindings?: Record<string, string>;
    fonts?: Array<{ id: string; mediaType: string; bytes: Uint8Array }>;
    fallbackFonts?: Array<{ id: string; family: string }>;
    experimentalComposition?: boolean;
    gpuContext?: any;
    loadVideo?: (asset: any, metadata: any) => Promise<any>;
  }
): Promise<BrowserTextComposition>;
