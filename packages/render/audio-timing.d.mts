export function audioPacketTiming(
  startFrame: number,
  packetFrames: number,
  encoderDelay: number,
  totalFrames: number,
  sampleRate: number
): { timestamp: number; duration: number } | null;
export function measureAudioDelay(
  reference: Float32Array,
  decoded: Float32Array,
  markerStart: number,
  markerFrames: number,
  maximumDelay?: number
): { delay: number; correlation: number };
