export interface SnapTarget {
  value: number;
  multiplier?: number;
}
export interface SnapProbe {
  offset: number;
  targets: readonly SnapTarget[];
}
export interface SnapResult {
  value: number;
  guide?: number;
}

// VideoCut's SnapEngine / EditControl use a 5px capture distance. A slightly
// wider release band prevents pointer noise from repeatedly dropping a guide.
const capturePx = 5;
const releasePx = 8;

/** Freeze targets once per gesture; motion searches only nearby sorted edges. */
export class MagneticSnap {
  private probes: Array<SnapProbe & { radius: number }>;
  private held?: { offset: number; target: SnapTarget };

  constructor(probes: readonly SnapProbe[]) {
    this.probes = probes.map((probe) => {
      const targets = probe.targets
        .filter((target) => Number.isFinite(target.value))
        .map((target) => ({ ...target, multiplier: target.multiplier ?? 1 }))
        .sort((a, b) => a.value - b.value);
      return {
        offset: probe.offset,
        targets,
        radius: targets.reduce((max, target) => Math.max(max, target.multiplier), 1)
      };
    });
  }

  clear() {
    this.held = undefined;
  }

  snap(
    position: number,
    pixelsPerUnit: number,
    enabled = true,
    minimum = -Infinity,
    maximum = Infinity
  ): SnapResult {
    const value = Math.max(minimum, Math.min(maximum, position));
    if (!enabled || !Number.isFinite(pixelsPerUnit) || pixelsPerUnit <= 0) {
      this.clear();
      return { value };
    }
    if (this.held) {
      const { offset, target } = this.held;
      const snapped = target.value - offset;
      if (
        snapped >= minimum &&
        snapped <= maximum &&
        Math.abs(value + offset - target.value) * pixelsPerUnit <= releasePx
      )
        return { value: snapped, guide: target.value };
      this.clear();
    }
    let nearest = Infinity;
    for (const probe of this.probes) {
      const point = value + probe.offset;
      const radius = (capturePx * probe.radius) / pixelsPerUnit;
      let low = 0,
        high = probe.targets.length;
      while (low < high) {
        const middle = (low + high) >>> 1;
        if (probe.targets[middle].value < point - radius) low = middle + 1;
        else high = middle;
      }
      for (let index = low; index < probe.targets.length; index++) {
        const target = probe.targets[index];
        if (target.value > point + radius) break;
        const distance = Math.abs(point - target.value) * pixelsPerUnit;
        const snapped = target.value - probe.offset;
        if (
          snapped >= minimum &&
          snapped <= maximum &&
          distance <= capturePx * (target.multiplier ?? 1) &&
          distance < nearest
        ) {
          nearest = distance;
          this.held = { offset: probe.offset, target };
        }
      }
    }
    return this.held
      ? { value: this.held.target.value - this.held.offset, guide: this.held.target.value }
      : { value };
  }
}

/** Use presented bounds, including text layout, crop, anchor, flips and scale. */
export function previewAxisSnap(
  leading: number,
  extent: number,
  authoredPosition: number,
  canvasExtent: number,
  rotationDegrees: number
) {
  const probes: SnapProbe[] = [
    {
      offset: leading + extent / 2 - authoredPosition,
      targets: [{ value: canvasExtent / 2 }]
    }
  ];
  // At arbitrary rotations the AABB edge is not a visible control-frame edge.
  if (Math.abs(rotationDegrees - Math.round(rotationDegrees / 90) * 90) <= 0.5) {
    probes.push(
      { offset: leading - authoredPosition, targets: [{ value: 0 }] },
      {
        offset: leading + extent - authoredPosition,
        targets: [{ value: canvasExtent }]
      }
    );
  }
  return new MagneticSnap(probes);
}
