import type { Plugin } from 'vite';
export const ttsAssetFiles: ReadonlyMap<string, string>;
export function ttsAssetBytes(name: string): Uint8Array;
export function ttsRuntimeAssets(): Plugin;
