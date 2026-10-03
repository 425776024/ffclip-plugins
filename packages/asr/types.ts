import type { AsrLanguage, AsrSegment, TtsBackend } from '../client/types';
export interface AsrRequest {
  mediaUrl: string;
  modelBaseUrl: string;
  sourceBegin: number;
  sourceEnd: number;
  language: AsrLanguage;
  backend: TtsBackend;
}
export interface AsrProgress {
  phase: string;
  progress: number;
  backend?: 'webgpu' | 'wasm';
}
export interface AsrResult {
  text: string;
  segments: AsrSegment[];
  backend: 'webgpu' | 'wasm';
}
