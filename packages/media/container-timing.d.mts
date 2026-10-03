export function matroskaCodecDelay(bytes: Uint8Array, trackNumber: number): number | null;
export function readMatroskaCodecDelay(
  url: string,
  trackNumber: number,
  signal?: AbortSignal
): Promise<number>;
