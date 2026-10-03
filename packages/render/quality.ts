export interface QualitySample {
  frameMs: number;
  textureBytes?: number;
  resourceBytes?: number;
  workingSetBytes?: number;
  textureBudget?: number;
  compositeHits?: number;
  gpuPasses?: number;
}
/** Hysteresis keeps short decode spikes and cheap cache hits from repeatedly changing resolution. */
export class AdaptivePreviewQuality {
  private levels = [1, 0.75, 0.5, 0.375, 0.25];
  private index = 0;
  private slow = 0;
  private fast = 0;
  private average = 0;
  private lastChange = 0;
  private constrainedUntil = 0;
  get scale() {
    return this.levels[this.index];
  }
  reset() {
    this.index = 0;
    this.average = this.slow = this.fast = this.lastChange = 0;
    this.constrainedUntil = 0;
  }
  paused() {
    this.index = 0;
    this.average = this.slow = this.fast = 0;
    this.constrainedUntil = 0;
  }
  budgetPressure(now = performance.now()) {
    this.constrainedUntil = now + 5000;
    this.slow = this.fast = 0;
    if (this.index === this.levels.length - 1) return false;
    this.index++;
    return true;
  }
  sample(value: QualitySample, fps: number, now: number) {
    if (value.gpuPasses === 0 && (value.compositeHits || 0) > 0) return false;
    this.average = this.average ? this.average * 0.8 + value.frameMs * 0.2 : value.frameMs;
    const target = 1000 / Math.max(1, fps);
    const pressure =
      (value.workingSetBytes ?? value.resourceBytes ?? value.textureBytes ?? 0) /
      (value.textureBudget || Infinity);
    this.slow = this.average > target * 1.15 || pressure > 0.9 ? this.slow + 1 : 0;
    this.fast = this.average < target * 0.58 && pressure < 0.65 ? this.fast + 1 : 0;
    if (now - this.lastChange < 1000) return false;
    if (this.slow >= 4 && this.index < this.levels.length - 1) this.index++;
    else if (this.fast >= 45 && this.index > 0 && now > this.constrainedUntil) this.index--;
    else return false;
    this.lastChange = now;
    this.slow = this.fast = 0;
    return true;
  }
  extent(base: { width: number; height: number }) {
    return {
      width: Math.max(2, Math.round(base.width * this.scale)),
      height: Math.max(2, Math.round(base.height * this.scale))
    };
  }
}
