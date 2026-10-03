/** Exact authored frames retain their original PNG bytes under a fixed budget. */
export class HtmlFrameCache {
  constructor({ maximumBytes = 64 * 1048576, maximumFrames = 256 } = {}) {
    if (
      !Number.isSafeInteger(maximumBytes) ||
      maximumBytes < 1 ||
      !Number.isSafeInteger(maximumFrames) ||
      maximumFrames < 1
    )
      throw new RangeError('HTML frame cache requires finite positive budgets');
    this.maximumBytes = maximumBytes;
    this.maximumFrames = maximumFrames;
    this.frames = new Map();
    this.bytes = 0;
  }
  get(key) {
    const png = this.frames.get(key);
    if (png) {
      this.frames.delete(key);
      this.frames.set(key, png);
    }
    return png;
  }
  set(key, png) {
    this.delete(key);
    if (png.length > this.maximumBytes) return;
    while (this.frames.size >= this.maximumFrames || this.bytes + png.length > this.maximumBytes)
      this.delete(this.frames.keys().next().value);
    this.frames.set(key, png);
    this.bytes += png.length;
  }
  delete(key) {
    const old = this.frames.get(key);
    if (old) {
      this.bytes -= old.length;
      this.frames.delete(key);
    }
  }
  clear() {
    this.frames.clear();
    this.bytes = 0;
  }
}
