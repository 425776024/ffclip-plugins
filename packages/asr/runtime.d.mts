import type { AudioBlock } from '../media/browser';
import type { AsrSegment } from '../client/types';
export const ASR_SAMPLE_RATE: number;
export interface AudioWindow {
  begin: number;
  end: number;
  keepBegin: number;
  keepEnd: number;
}
export function audioWindows(begin: number, end: number): AudioWindow[];
export function mixMono16k(output: Float32Array, block: AudioBlock, begin: number): void;
export function isSilent(samples: Float32Array): boolean;
export function windowWords(
  result: { text?: string; chunks?: { text: string; timestamp: [number, number | null] }[] },
  window: AudioWindow,
  sourceBegin: number
): AsrSegment[];
export function subtitleSegments(words: AsrSegment[]): AsrSegment[];
