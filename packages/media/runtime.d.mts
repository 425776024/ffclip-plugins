export function abortError(): DOMException;
export function throwIfAborted(signal?: AbortSignal): void;
export class ByteCache<T> {
  constructor(maximumBytes: number);
  maximumBytes: number; bytes: number; hits: number; misses: number;
  entries: Map<string, {value:T; bytes:number}>;
  get(key: string): T | undefined;
  set(key: string, value: T, bytes: number): T;
  delete(key: string): void;
  clear(): void;
}
export class MediaScheduler {
  constructor(concurrency?: number);
  active: number; completed: number; cancelled: number;
  run<T, R = T>(key: string, produce: (signal: AbortSignal) => Promise<T> | T, options?: {signal?: AbortSignal; priority?: number; consume?: (value:T)=>R; afterDispatch?: (value:T)=>void}): Promise<R>;
  dispose(): void;
}
export interface PeakChannel {min: Float32Array; max: Float32Array}
export interface PeakLevel {bucketFrames: number; channels: PeakChannel[]}
export class PeakAccumulator {
  constructor(numberOfFrames: number, channels: number, bucketFrames?: number);
  add(data: Float32Array[], offsetFrames: number): void;
  finish(): PeakLevel[];
}
export function rasterSize(width: number, height: number, displayHeight: number, dpr?: number): {width:number;height:number};
export function thumbnailGeometry(sourceWidth:number,sourceHeight:number,displayWidth?:number,displayHeight?:number,dpr?:number,fit?:'cover'|'contain'):{width:number;height:number;x:number;y:number;drawWidth:number;drawHeight:number};
export function sampleLinear(data: Float32Array | undefined, position: number): number;

export function sourceCacheIdentity(url: string): string;
