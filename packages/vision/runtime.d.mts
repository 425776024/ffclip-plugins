export const VISION_MAX_FRAMES: number;
export const VISION_MAX_SEGMENTS: number;
export const VISION_MAX_SAMPLES: number;
export interface VideoSegmentPlan {
  startSeconds: number;
  endSeconds: number;
  sampleTimes: number[];
}
export interface VideoSampling {
  strategy: 'uniform-intervals';
  requestedSegmentSeconds: number;
  intervalSeconds: number;
  framesPerSegment: number;
  totalSamples: number;
  coarsened: boolean;
}
export function planVideoSegments(
  begin: number,
  end: number,
  options?: {
    segmentSeconds?: number;
    maxSegments?: number;
    framesPerSegment?: number;
  }
): { segments: VideoSegmentPlan[]; sampling: VideoSampling };
export function storyboardPrompt(prompt: string, segment: VideoSegmentPlan): string;
export function sampleTimes(begin: number, end: number, count: number): number[];
export function describeFrame(options: {
  processor: any;
  model: any;
  image: any;
  prompt: string;
  maxNewTokens: number;
}): Promise<string>;
