export type TtsBackend = 'auto' | 'webgpu' | 'wasm';
export type TtsDtype = 'fp32';
export interface TtsRequest {
  text: string;
  voice: string;
  speed: number;
  backend: TtsBackend;
  modelBaseUrl: string;
  dtype: TtsDtype;
}
export interface TtsProgress { phase: string; progress: number; message?: string }
export interface TtsResult {
  wav: ArrayBuffer;
  sampleRate: 24000;
  durationSeconds: number;
  backend: 'webgpu' | 'wasm';
}
