import { COLOR_PARAMETERS, parseCurve, parseCube } from '../core/color.mjs';
const clamp = (v) => Math.max(0, Math.min(1, v));
const mix = (a, b, t) => a.map((v, i) => v + (b[i] - v) * t);
const luminance = (rgb) => rgb[0] * 0.2126 + rgb[1] * 0.7152 + rgb[2] * 0.0722;
function hsl(rgb, hue, saturation, lightness) {
  const max = Math.max(...rgb),
    min = Math.min(...rgb),
    d = max - min;
  let h = 0,
    l = (max + min) / 2,
    s = d === 0 ? 0 : d / Math.max(1e-6, 1 - Math.abs(2 * l - 1));
  if (d)
    h =
      (max === rgb[0]
        ? (rgb[1] - rgb[2]) / d
        : max === rgb[1]
          ? (rgb[2] - rgb[0]) / d + 2
          : (rgb[0] - rgb[1]) / d + 4) / 6;
  h = (((h + hue / 360) % 1) + 1) % 1;
  s = clamp(s * (1 + saturation / 100));
  l = clamp(l + lightness / 100);
  const c = (1 - Math.abs(2 * l - 1)) * s,
    x = c * (1 - Math.abs(((h * 6) % 2) - 1)),
    m = l - c / 2;
  return [
    [c, x, 0],
    [x, c, 0],
    [0, c, x],
    [0, x, c],
    [x, 0, c],
    [c, 0, x]
  ][Math.min(5, Math.floor(h * 6))].map((v) => v + m);
}
function curve(points, x) {
  for (let i = 1; i < points.length; i++)
    if (x <= points[i][0]) {
      const [a, b] = [points[i - 1], points[i]];
      return a[1] + ((b[1] - a[1]) * (x - a[0])) / (b[0] - a[0]);
    }
  return points.at(-1)[1];
}
const looks = {
  none: {},
  grayscale: { grayscale: 100 },
  bright: { brightness: 12, exposure: 0.2 },
  warm: { temperature: 35, saturation: 110 },
  cool: { temperature: -35, saturation: 105 },
  vintage: { temperature: 20, saturation: 75, contrast: -12, shadows: 12 },
  film: { temperature: 8, saturation: 88, contrast: 18, highlights: -12 },
  japanese: { brightness: 10, contrast: -12, saturation: 80, temperature: -8 },
  dramatic: { contrast: 35, saturation: 85, shadows: -10 },
  soft: { contrast: -20, brightness: 5 },
  'high-contrast': { contrast: 55 },
  sepia: { saturation: 0, temperature: 60, tint: 12 },
  cyberpunk: { contrast: 20, tint: 35, temperature: -25, saturation: 135 },
  fairy: { brightness: 10, contrast: -15, tint: 12, saturation: 115 },
  cinematic: { contrast: 25, saturation: 85, temperature: -10 }
};
export function colorTransform(parameters = {}) {
  const p = Object.fromEntries(
    Object.entries(COLOR_PARAMETERS).map(([k, d]) => [k, parameters[k] ?? d.default])
  );
  const curves = ['rgb', 'red', 'green', 'blue'].map((c) => parseCurve(p['curve' + c]));
  return (input) => {
    let rgb = input.map((v) => v * 2 ** p.exposure);
    rgb = rgb.map(
      (v, i) =>
        (v + p.brightness / 100 - 0.5) * (1 + p.contrast / 100) +
        0.5 +
        (p.temperature / 100) * [0.12, 0, -0.12][i] +
        (p.tint / 100) * [0.06, -0.09, 0.06][i]
    );
    const l = clamp(luminance(rgb)),
      sw = (1 - l) ** 2,
      hw = l ** 2,
      mw = 1 - sw - hw;
    rgb = rgb.map(
      (v) =>
        clamp(v + (p.shadows / 100) * sw * 0.5 + (p.highlights / 100) * hw * 0.5) ** (1 / p.gamma)
    );
    const gray = luminance(rgb);
    rgb = rgb.map((v) => clamp(gray + ((v - gray) * p.saturation) / 100));
    rgb = hsl(rgb, p.hue + p.hslHue, p.hslSaturation, p.hslLightness);
    for (const [tone, weight] of [
      ['shadows', sw],
      ['midtones', mw],
      ['highlights', hw]
    ]) {
      let graded = hsl(rgb, p[tone + 'Hue'], p[tone + 'Saturation'], p[tone + 'Brightness'] * 0.5);
      graded = graded.map((v) => clamp((v - 0.5) * (1 + p[tone + 'Contrast'] / 100) + 0.5));
      rgb = mix(rgb, graded, weight);
    }
    rgb = rgb.map((v, i) => curve(curves[i + 1], curve(curves[0], clamp(v))));
    return mix(rgb, [luminance(rgb), luminance(rgb), luminance(rgb)], p.grayscale / 100).map(clamp);
  };
}
export function cubeTransform(source) {
  const cube = parseCube(source),
    n = cube.size;
  return (input) => {
    const q = input.map((v, i) => clamp((v - cube.min[i]) / (cube.max[i] - cube.min[i])) * (n - 1));
    const lo = q.map(Math.floor),
      hi = lo.map((v) => Math.min(n - 1, v + 1)),
      t = q.map((v, i) => v - lo[i]);
    const row = (r, g, b) => cube.rows[b * n * n + g * n + r];
    const plane = (b) =>
      mix(
        mix(row(lo[0], lo[1], b), row(hi[0], lo[1], b), t[0]),
        mix(row(lo[0], hi[1], b), row(hi[0], hi[1], b), t[0]),
        t[1]
      );
    return mix(plane(lo[2]), plane(hi[2]), t[2]).map(clamp);
  };
}
export function makeColorLut(effect, size = 33) {
  let transform =
    effect.templateId === 'custom-lut'
      ? cubeTransform(effect.parameters.cube)
      : colorTransform(
          effect.templateId === 'looks' ? looks[effect.parameters.preset] : effect.parameters
        );
  if (effect.templateId === 'looks' && effect.parameters.preset === 'sepia')
    transform = (rgb) =>
      [
        rgb[0] * 0.393 + rgb[1] * 0.769 + rgb[2] * 0.189,
        rgb[0] * 0.349 + rgb[1] * 0.686 + rgb[2] * 0.168,
        rgb[0] * 0.272 + rgb[1] * 0.534 + rgb[2] * 0.131
      ].map(clamp);
  const data = new Uint8Array(size ** 3 * 4);
  for (let g = 0; g < size; g++)
    for (let b = 0; b < size; b++)
      for (let r = 0; r < size; r++)
        data.set(
          [...transform([r, g, b].map((v) => v / (size - 1))).map((v) => Math.round(v * 255)), 255],
          (g * size * size + b * size + r) * 4
        );
  return { data, size };
}
