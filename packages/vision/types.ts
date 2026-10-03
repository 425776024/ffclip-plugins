import type { TtsBackend, VisionFrame, VisionSegment } from '../client/types';
import type { VideoSegmentPlan } from './runtime.mjs';
export interface VisionRequest {
  mediaUrl: string;
  modelBaseUrl: string;
  kind: 'image' | 'video';
  sampleTimes: number[];
  prompt: string;
  maxNewTokens: number;
  backend: TtsBackend;
  resultFormat?: 'frames' | 'description' | 'segments';
  segmentPlan?: VideoSegmentPlan[];
}
export interface VisionProgress {
  phase: string;
  progress: number;
  backend?: 'webgpu' | 'wasm';
}
export interface VisionResult {
  frames: VisionFrame[];
  segments?: VisionSegment[];
  backend: 'webgpu' | 'wasm';
}
