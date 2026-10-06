/** Authored SDR color controls shared by editing, validation and the LUT baker. */
const number = (label, min = -100, max = 100, value = 0, unit = '%') => ({
  type: 'number',
  label,
  min,
  max,
  default: value,
  unit,
  step: 1,
  keyframe: false
});
export const LOOK_NAMES = {
  none: '原色',
  grayscale: '黑白',
  bright: '明亮',
  warm: '暖色',
  cool: '冷色',
  vintage: '复古',
  film: '胶片',
  japanese: '日系',
  dramatic: '戏剧',
  soft: '柔和',
  'high-contrast': '高对比',
  sepia: '棕褐',
  cyberpunk: '赛博朋克',
  fairy: '梦幻',
  cinematic: '电影'
};
export const COLOR_PARAMETERS = {
  brightness: number('亮度'),
  contrast: number('对比度'),
  saturation: number('饱和度', 0, 200, 100),
  hue: number('色相', -180, 180, 0, '°'),
  temperature: number('色温'),
  tint: number('色调'),
  exposure: { ...number('曝光', -5, 5, 0, 'EV'), step: 0.1 },
  gamma: { ...number('伽马', 0.1, 3, 1, ''), step: 0.01 },
  highlights: number('高光'),
  shadows: number('阴影'),
  grayscale: number('灰度', 0, 100),
  hslHue: number('色相', -180, 180, 0, '°'),
  hslSaturation: number('饱和度'),
  hslLightness: number('明度'),
  ...Object.fromEntries(
    ['shadows', 'midtones', 'highlights'].flatMap((tone) =>
      ['Hue', 'Saturation', 'Brightness', 'Contrast'].map((key) => [
        tone + key,
        number(
          { Hue: '色相', Saturation: '饱和度', Brightness: '亮度', Contrast: '对比度' }[key],
          key === 'Hue' ? -180 : -100,
          key === 'Hue' ? 180 : 100,
          0,
          key === 'Hue' ? '°' : '%'
        )
      ])
    )
  ),
  ...Object.fromEntries(
    ['rgb', 'red', 'green', 'blue'].map((channel) => [
      'curve' + channel,
      {
        type: 'text',
        label: channel.toUpperCase(),
        default: '[[0,0],[1,1]]',
        maxLength: 4096,
        keyframe: false
      }
    ])
  )
};
export const IDENTITY_CUBE =
  'LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1';
export const RESTORED_EFFECTS = [
  { id: 'color-grade', name: '调色', parameters: COLOR_PARAMETERS },
  {
    id: 'looks',
    name: '滤镜预设',
    parameters: {
      preset: {
        type: 'enum',
        label: '风格',
        values: Object.keys(LOOK_NAMES),
        default: 'film',
        keyframe: false
      },
      amount: { ...number('强度', 0, 1, 1, ''), step: 0.01, keyframe: true }
    }
  },
  {
    id: 'detail',
    name: '画面细节',
    parameters: {
      sharpness: number('锐化', 0, 100),
      noise: number('颗粒', 0, 100),
      vignette: number('暗角', 0, 100)
    }
  },
  {
    id: 'custom-lut',
    name: '自定义 LUT',
    parameters: {
      cube: {
        type: 'text',
        label: 'LUT 文件',
        default: IDENTITY_CUBE,
        maxLength: 8388608,
        keyframe: false
      },
      amount: { ...number('强度', 0, 1, 1, ''), step: 0.01, keyframe: true }
    }
  }
];
export function parseCurve(value) {
  let points;
  try {
    points = JSON.parse(value);
  } catch {
    throw new Error('曲线数据无效');
  }
  if (
    !Array.isArray(points) ||
    points.length < 2 ||
    points.length > 32 ||
    points.some(
      (point, i) =>
        !Array.isArray(point) ||
        point.length !== 2 ||
        point.some((v) => !Number.isFinite(v) || v < 0 || v > 1) ||
        (i && point[0] <= points[i - 1][0])
    ) ||
    points[0][0] !== 0 ||
    points.at(-1)[0] !== 1
  )
    throw new Error('曲线需要从 0 到 1 的递增控制点');
  return points;
}
export function parseCube(source) {
  if (typeof source !== 'string' || source.length > 8388608) throw new Error('LUT 文件过大');
  let size = 0,
    min = [0, 0, 0],
    max = [1, 1, 1];
  const rows = [];
  for (const raw of source.split(/\r?\n/)) {
    const line = raw.replace(/#.*/, '').trim();
    if (!line || line.startsWith('TITLE')) continue;
    const [key, ...values] = line.split(/\s+/);
    if (key === 'LUT_3D_SIZE') {
      if (size || values.length !== 1) throw new Error('LUT 尺寸无效');
      size = Number(values[0]);
      if (!Number.isInteger(size) || size < 2 || size > 64)
        throw new Error('LUT 尺寸需要在 2 到 64 之间');
    } else if (key === 'DOMAIN_MIN' || key === 'DOMAIN_MAX') {
      const domain = values.map(Number);
      if (domain.length !== 3 || domain.some((v) => !Number.isFinite(v)))
        throw new Error('LUT 输入范围无效');
      if (key === 'DOMAIN_MIN') min = domain;
      else max = domain;
    } else {
      const row = [key, ...values].map(Number);
      if (row.length !== 3 || row.some((v) => !Number.isFinite(v)) || rows.length >= 64 ** 3)
        throw new Error('仅支持有效的三维 .cube LUT');
      rows.push(row);
    }
  }
  if (!size || rows.length !== size ** 3 || min.some((v, i) => v >= max[i]))
    throw new Error('LUT 数据数量或输入范围无效');
  return { size, rows, min, max };
}
