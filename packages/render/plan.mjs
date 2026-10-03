import {
  evaluateVisual,
  evaluateEffects,
  mapTimelineToSource,
  activeTransitions
} from '../core/project.mjs';

export const clamp = (n, a, b) => Math.min(b, Math.max(a, n));
export function previewExtent(canvas, displayWidth, displayHeight, dpr = 1, playing = false) {
  const cap = playing ? 1280 : 4096;
  const ratio = Math.min(
    1,
    Math.max((displayWidth * dpr) / canvas.width, (displayHeight * dpr) / canvas.height),
    cap / Math.max(canvas.width, canvas.height)
  );
  return {
    width: Math.max(2, Math.round(canvas.width * ratio)),
    height: Math.max(2, Math.round(canvas.height * ratio))
  };
}
export function frameTime(index, frameRate) {
  return Math.round((index * 120000 * frameRate.denominator) / frameRate.numerator);
}
/** Playback uses the project's authored frame grid; paused seeks retain exact ticks. */
export function previewSampleTime(time, frameRate, playing) {
  if (!playing) return time;
  // frameTime rounds fractional tick boundaries; honor those exact stored ticks.
  const frame = Math.floor(((time + 0.5) * frameRate.numerator) / (120000 * frameRate.denominator));
  return frameTime(frame, frameRate);
}
export function parseColor(value, fallback = [0, 0, 0, 1]) {
  if (Array.isArray(value)) return value;
  if (typeof value !== 'string' || !/^#[a-f\d]{6}([a-f\d]{2})?$/i.test(value)) return fallback;
  return [
    parseInt(value.slice(1, 3), 16) / 255,
    parseInt(value.slice(3, 5), 16) / 255,
    parseInt(value.slice(5, 7), 16) / 255,
    value.length === 9 ? parseInt(value.slice(7, 9), 16) / 255 : 1
  ];
}

// Geometry is computed once in authoring pixels, shared by GPU, fallback and hit-testing.
export function layerGeometry(
  project,
  visual,
  sourceWidth,
  sourceHeight,
  width,
  height,
  fullCanvas = false
) {
  const cw = project.canvas.width,
    ch = project.canvas.height;
  const crop = fullCanvas ? { left: 0, top: 0, right: 0, bottom: 0 } : visual.crop || {};
  const l = clamp((crop.left || 0) * sourceWidth, 0, sourceWidth - 1),
    r = clamp((crop.right || 0) * sourceWidth, 0, sourceWidth - l - 1);
  const t = clamp((crop.top || 0) * sourceHeight, 0, sourceHeight - 1),
    b = clamp((crop.bottom || 0) * sourceHeight, 0, sourceHeight - t - 1);
  const sw = sourceWidth - l - r,
    sh = sourceHeight - t - b;
  const fit = fullCanvas ? 'stretch' : visual.fitPolicy || 'contain';
  const s =
    fit === 'cover'
      ? Math.max(cw / sw, ch / sh)
      : fit === 'nativeCrop'
        ? 1
        : Math.min(cw / sw, ch / sh);
  const fw = fit === 'stretch' ? cw : sw * s,
    fh = fit === 'stretch' ? ch : sh * s;
  const sx = (visual.scaleX ?? 1) * (visual.flipHorizontal ? -1 : 1),
    sy = (visual.scaleY ?? 1) * (visual.flipVertical ? -1 : 1);
  const angle = ((visual.rotationDegrees || 0) * Math.PI) / 180,
    c = Math.cos(angle),
    sn = Math.sin(angle);
  const ax = visual.anchorX ?? 0.5,
    ay = visual.anchorY ?? 0.5;
  const centerX = cw / 2 + (visual.positionX || 0),
    centerY = ch / 2 + (visual.positionY || 0);
  // Transform around anchor while the untransformed media remains centered.
  const anchorX = centerX + (ax - 0.5) * fw,
    anchorY = centerY + (ay - 0.5) * fh;
  const m = [c * sx * fw, sn * sx * fw, -sn * sy * fh, c * sy * fh];
  const tx = anchorX - m[0] * ax - m[2] * ay,
    ty = anchorY - m[1] * ax - m[3] * ay;
  const det = m[0] * m[3] - m[1] * m[2];
  const safe = Math.abs(det) < 1e-12 ? 1e-12 : det;
  const inverse = [
    (m[3] * cw) / safe,
    (-m[2] * ch) / safe,
    (m[2] * ty - m[3] * tx) / safe,
    (-m[1] * cw) / safe,
    (m[0] * ch) / safe,
    (m[1] * tx - m[0] * ty) / safe
  ];
  const points = [
    [tx, ty],
    [tx + m[0], ty + m[1]],
    [tx + m[2], ty + m[3]],
    [tx + m[0] + m[2], ty + m[1] + m[3]]
  ];
  const minX = Math.min(...points.map((p) => p[0])),
    minY = Math.min(...points.map((p) => p[1]));
  return {
    inverse,
    crop: [l / sourceWidth, t / sourceHeight, sw / sourceWidth, sh / sourceHeight],
    opacity: visual.opacity ?? 1,
    renderScale: width / cw,
    matrix: [
      (m[0] * width) / cw,
      (m[1] * height) / ch,
      (m[2] * width) / cw,
      (m[3] * height) / ch,
      (tx * width) / cw,
      (ty * height) / ch
    ],
    bounds: {
      x: minX,
      y: minY,
      width: Math.max(...points.map((p) => p[0])) - minX,
      height: Math.max(...points.map((p) => p[1])) - minY
    }
  };
}
export function buildRenderPlan(
  project,
  time,
  width = project.canvas.width,
  height = project.canvas.height
) {
  const active = activeTransitions(project, time);
  const endpoints = new Set(active.flatMap((t) => [t.fromItemId, t.toItemId]));
  const all = project.timeline.tracks.flatMap((track) =>
    track.items.map((item) => ({ track, item }))
  );
  const layer = ({ track, item }) => ({
    item,
    track,
    asset: project.assets.find((a) => a.id === item.clip.assetId),
    visual: evaluateVisual(item, time),
    sourceTime: mapTimelineToSource(item, time, { clamp: false }),
    effects: evaluateEffects(item, time)
  });
  const layers = [];
  for (const track of [...project.timeline.tracks].reverse()) {
    if (!track.visible || track.type === 'audio') continue;
    for (const item of track.items) {
      if (!item.enabled || item.clip.type === 'audio' || endpoints.has(item.id)) continue;
      if (time >= item.placement.begin && time < item.placement.end)
        layers.push({ kind: 'layer', layer: layer({ track, item }) });
    }
    for (const tr of active) {
      const from = all.find((x) => x.item.id === tr.fromItemId),
        to = all.find((x) => x.item.id === tr.toItemId);
      if (!from || !to || from.track.id !== track.id || !from.item.enabled || !to.item.enabled)
        continue;
      layers.push({
        kind: 'transition',
        from: layer(from),
        to: layer(to),
        transition: {
          ...tr,
          style:
            { dissolve: 'cross_dissolve', fade: 'fade_color', wipe: 'wipe', slide: 'slide' }[
              tr.templateId
            ] || tr.templateId,
          direction: tr.parameters?.direction || 'right',
          color: parseColor(tr.parameters?.color)
        }
      });
    }
  }
  return {
    project,
    time,
    width,
    height,
    layers,
    background: parseColor(project.canvas.backgroundColor || project.canvas.background)
  };
}

export function makeLut(preset = 'warm', size = 33) {
  const data = new Uint8Array(size * size * size * 4);
  for (let b = 0; b < size; b++)
    for (let g = 0; g < size; g++)
      for (let r = 0; r < size; r++) {
        let rgb = [r / (size - 1), g / (size - 1), b / (size - 1)];
        // Same 33-cube film curves as native EffectRuntime::ApplyLook.
        const film = (v) => v * v * (3 - 2 * v),
          luma = rgb[0] * 0.2126 + rgb[1] * 0.7152 + rgb[2] * 0.0722;
        if (preset === 'warm')
          rgb = [
            film(rgb[0]) * 1.045 + 0.02,
            film(rgb[1]) * 1.005 + 0.012,
            film(rgb[2]) * 0.92 + 0.028
          ];
        else if (preset === 'cool')
          rgb = [
            film(rgb[0]) * 0.91 + 0.045,
            film(rgb[1]) * 0.98 + 0.055,
            film(rgb[2]) * 1.055 + 0.07
          ];
        else if (preset === 'cinema')
          rgb = [
            film(rgb[0]) * 1.035 + 0.012,
            film(rgb[1]) * 0.995 + 0.008,
            film(rgb[2]) * 0.955 + 0.02
          ].map((v) => luma + (v - luma) * 0.9);
        else if (preset !== 'identity') throw new Error('未知LUT预设：' + preset);
        const i = (g * size * size + b * size + r) * 4;
        data.set([...rgb.map((v) => Math.round(clamp(v, 0, 1) * 255)), 255], i);
      }
  return { data, size };
}
