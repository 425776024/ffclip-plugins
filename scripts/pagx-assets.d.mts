export const pagxPackageRoot: string;
export const pagxAssetFiles: Map<string, string>;
export const pagxNativeFiles: Map<string, string>;
export function pagxAssetBytes(name: string): Buffer;
export function pagxRuntimeAssets(): import('vite').Plugin;
