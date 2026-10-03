import type { MediaFrame } from './browser';
import { ByteCache } from './runtime.mjs';

// One UI-level cache across every visible clip; components never retain their
// own unbounded bitmap histories. Reading and canvas drawing are synchronous.
const tiles = new ByteCache<MediaFrame>(16 * 1024 * 1024);
export function visualTile(key: string) {
  return tiles.get(key);
}
export function keepVisualTile(key: string, frame: MediaFrame) {
  tiles.set(key, frame, frame.width * frame.height * 4);
}
export function visualTileStats() {
  return { bytes: tiles.bytes, hits: tiles.hits, maximumBytes: tiles.maximumBytes };
}
