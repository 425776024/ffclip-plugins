import { clone, identity, ticks, validateProject } from './project.mjs';

const numeric = (label, min, max, defaultValue, unit = '', keyframe = true) => ({
  type: 'number',
  label,
  min,
  max,
  default: defaultValue,
  unit,
  keyframe
});
/** Shared authored property contract. Presentation state never enters this table. */
const propertyDefinitions = {
  'visual.positionX': numeric('位置 X', -32768, 32768, 0, 'px'),
  'visual.positionY': numeric('位置 Y', -32768, 32768, 0, 'px'),
  'visual.scaleX': numeric('缩放 X', 0.01, 20, 1),
  'visual.scaleY': numeric('缩放 Y', 0.01, 20, 1),
  'visual.rotationDegrees': numeric('旋转', -360, 360, 0, '°'),
  'visual.opacity': numeric('不透明度', 0, 1, 1),
  'visual.anchorX': numeric('锚点 X', 0, 1, 0.5),
  'visual.anchorY': numeric('锚点 Y', 0, 1, 0.5),
  'visual.crop.left': numeric('左裁剪', 0, 0.999, 0, '', false),
  'visual.crop.top': numeric('上裁剪', 0, 0.999, 0, '', false),
  'visual.crop.right': numeric('右裁剪', 0, 0.999, 0, '', false),
  'visual.crop.bottom': numeric('下裁剪', 0, 0.999, 0, '', false),
  'visual.fitPolicy': {
    type: 'enum',
    label: '适配',
    values: ['contain', 'cover', 'stretch', 'nativeCrop'],
    default: 'contain',
    keyframe: false
  },
  'visual.flipHorizontal': { type: 'boolean', label: '水平翻转', default: false, keyframe: false },
  'visual.flipVertical': { type: 'boolean', label: '垂直翻转', default: false, keyframe: false },
  'visual.blendMode': {
    type: 'enum',
    label: '混合',
    values: ['normal', 'multiply', 'screen', 'overlay', 'darken', 'lighten'],
    default: 'normal',
    keyframe: false
  },
  'audio.gainLinear': numeric('音量', 0, 4, 1),
  'audio.muted': { type: 'boolean', label: '静音', default: false, keyframe: false },
  'audio.fadeIn': numeric('淡入', 0, 10368000000, 0, 'ticks', false),
  'audio.fadeOut': numeric('淡出', 0, 10368000000, 0, 'ticks', false),
  'text.content': { type: 'text', label: '文字内容', default: '默认文字', maxLength: 10000, keyframe: false },
  'text.fontSize': numeric('字号', 4, 1000, 64, 'px', false),
  'text.color': { type: 'color', label: '文字颜色', default: '#ffffff', pattern: '^#[0-9a-fA-F]{6}$', keyframe: false },
  'time.start': numeric('时间轴入点', 0, 10368000000, 0, 'ticks', false),
  'time.duration': numeric('片段时长', 1, 10368000000, 120000, 'ticks', false),
  'time.sourceIn': numeric('素材入点', 0, 10368000000, 0, 'ticks', false),
  'time.speed': numeric('播放速度', 0.1, 10, 1, '×', false)
};
/** @type {Readonly<Record<string, import('./types.js').PropertyDescriptor>>} */
export const PROPERTY_DESCRIPTORS = Object.freeze(Object.fromEntries(
  Object.entries(propertyDefinitions).map(([path, descriptor]) => [path, {
    ...descriptor,
    owner: path.startsWith('audio.') ? 'audio' : path.startsWith('visual.') ? 'visual' :
      path.startsWith('text.') ? (path === 'text.content' ? 'text' : 'plainText') :
        ['time.sourceIn', 'time.speed'].includes(path) ? 'media' : 'item',
    impact: path.startsWith('audio.') ? ['audio'] : path.startsWith('time.') ? ['media', 'layout', 'effect', 'composite', 'audio'] :
      path.startsWith('text.') ? ['layout', 'effect', 'composite'] :
        ['visual.opacity', 'visual.blendMode'].includes(path) ? ['composite'] : ['layout', 'composite']
  }])
));
/** @type {readonly import('./types.js').EffectTemplate[]} */
export const EFFECT_TEMPLATES = Object.freeze([
  {
    id: 'blur',
    name: '高斯模糊',
    parameters: { radius: { ...numeric('半径', 0, 64, 8, 'px', false), step: 1 } }
  },
  {
    id: 'glow',
    name: '柔光',
    parameters: {
      radius: numeric('半径', 0, 100, 12, 'px', false),
      strength: numeric('强度', 0, 2, 0.4, '', false)
    }
  },
  {
    id: 'lut',
    name: '电影调色',
    parameters: {
      preset: { type: 'enum', values: ['warm', 'cool', 'cinema'], default: 'cinema' },
      amount: numeric('强度', 0, 1, 1)
    }
  }
]);
/** @type {readonly import('./types.js').EffectTemplate[]} */
export const TRANSITION_TEMPLATES = Object.freeze([
  { id: 'dissolve', name: '叠化', parameters: {} },
  {
    id: 'fade',
    name: '淡色',
    parameters: {
      color: { type: 'enum', label: '颜色', values: ['#000000', '#ffffff'], default: '#000000' }
    }
  },
  {
    id: 'wipe',
    name: '擦除',
    parameters: {
      direction: { type: 'enum', values: ['left', 'right', 'up', 'down'], default: 'left' }
    }
  },
  {
    id: 'slide',
    name: '推移',
    parameters: {
      direction: { type: 'enum', values: ['left', 'right', 'up', 'down'], default: 'left' }
    }
  }
]);
/** @type {(object: unknown,path: string) => unknown} */
export function readPath(object, path) {
  if (path.startsWith('effects.')) {
    const { effect, parameter } = effectProperty(object, path);
    return effect?.parameters[parameter];
  }
  return path.split('.').reduce((value, key) => value?.[key], object);
}
/** @type {(object: object,path: string,value: unknown) => void} */
export function writePath(object, path, value) {
  if (path.startsWith('effects.')) {
    const { effect, parameter } = effectProperty(object, path);
    if (!effect) throw new Error(`特效属性不存在：${path}`);
    effect.parameters[parameter] = value;
    return;
  }
  const parts = path.split('.'),
    key = parts.pop();
  let target = object;
  for (const part of parts) target = target[part] ??= {};
  target[key] = value;
}
function effectProperty(clip, path) {
  const end = path.lastIndexOf('.');
  return {
    effect: clip?.effects?.find((effect) => effect.id === path.slice(8, end)),
    parameter: path.slice(end + 1)
  };
}
function descriptorForProperty(clip, property) {
  if (!property.startsWith('effects.'))
    return Object.hasOwn(PROPERTY_DESCRIPTORS, property)
      ? PROPERTY_DESCRIPTORS[property]
      : undefined;
  const { effect, parameter } = effectProperty(clip, property);
  const parameters = EFFECT_TEMPLATES.find(
    (template) => template.id === effect?.templateId
  )?.parameters;
  return parameters && Object.hasOwn(parameters, parameter) ? { ...parameters[parameter], owner: 'visual', impact: ['effect', 'composite'] } : undefined;
}
/** Includes only properties admitted by the current native effect catalog. */
/** @type {(item: import('./types.js').Item,property: string) => import('./types.js').PropertyDescriptor | undefined} */
export function getPropertyDescriptor(item, property) {
  return descriptorForProperty(item.clip, property);
}
/** Whether Format 1 has an authored property owner for this media content. */
/** @type {(project: import('./types.js').Project,item: import('./types.js').Item,property: string) => boolean} */
export function isPropertyApplicable(project, item, property) {
  if (!getPropertyDescriptor(item, property)) return false;
  if (property.startsWith('visual.') || property.startsWith('effects.'))
    return item.clip.type !== 'audio';
  if (property.startsWith('audio.'))
    return (
      item.clip.type === 'audio' ||
      (item.clip.type === 'video' &&
        project.assets.some((asset) => asset.id === item.clip.assetId && asset.hasAudio))
    );
  if (property.startsWith('text.')) return item.clip.type === 'text' && (property === 'text.content' || !item.clip.text.template);
  if (property.startsWith('time.')) return !['time.sourceIn', 'time.speed'].includes(property) || ['video', 'audio', 'html-clip'].includes(item.clip.type);
  return false;
}
/** One closure for model commands and their timeline interaction projections. */
/** @type {(project: import('./types.js').Project,itemIds: string[],options?: { expand?: boolean; groups?: boolean; links?: boolean }) => string[]} */
export function selectionClosure(project, ids, { expand = true, groups = true, links = true } = {}) {
  if (!Array.isArray(ids) || !ids.length || new Set(ids).size !== ids.length)
    throw new Error('选择片段无效');
  const known = new Set(
    project.timeline.tracks.flatMap((track) => track.items.map((item) => item.id))
  );
  const result = new Set(ids);
  if (expand) {
    let changed = true;
    while (changed) {
      changed = false;
      for (const relation of [
        ...(groups ? project.timeline.groups ?? [] : []),
        ...(links ? project.timeline.links ?? [] : [])
      ])
        if (relation.itemIds.some((id) => result.has(id)))
          for (const id of relation.itemIds)
            if (!result.has(id)) {
              result.add(id);
              changed = true;
            }
    }
  }
  if ([...result].some((id) => !known.has(id))) throw new Error('片段不存在');
  return [...result];
}
/** @type {(property: string,value: unknown,clip?: import('./types.js').Item['clip']) => unknown} */
export function validateProperty(property, value, clip) {
  const d = descriptorForProperty(clip, property);
  if (!d) throw new Error(`未知属性：${property}`);
  validateValue(d, value, property);
  return value;
}
/** JSON Schema is derived from the same range/type contract used by the Inspector. */
/** @param {import('./types.js').PropertyDescriptor} descriptor */
export function descriptorInputSchema(descriptor, { seconds: useSeconds = false } = {}) {
  const scale = useSeconds && descriptor.unit === 'ticks' ? 120000 : 1;
  return {
    type: ['enum', 'text', 'color'].includes(descriptor.type) ? 'string' : descriptor.step === 1 ? 'integer' : descriptor.type,
    ...(descriptor.values ? { enum: descriptor.values } : {}),
    ...(descriptor.min === undefined ? {} : { minimum: descriptor.min / scale, maximum: descriptor.max / scale }),
    ...(descriptor.maxLength ? { maxLength: descriptor.maxLength } : {}),
    ...(descriptor.pattern ? { pattern: descriptor.pattern } : {}),
    description: `${descriptor.label}${descriptor.unit ? ` (${useSeconds && descriptor.unit === 'ticks' ? 'seconds' : descriptor.unit})` : ''}`
  };
}
export function propertyInputSchema(property, options = {}) {
  const descriptor = PROPERTY_DESCRIPTORS[property];
  if (!descriptor) throw new Error(`未知属性：${property}`);
  return descriptorInputSchema(descriptor, options);
}
function validateValue(d, value, name) {
  if (d.step === 1 && !Number.isInteger(value)) throw new Error(`属性必须为整数：${name}`);
  if (
    d.type === 'number' &&
    (typeof value !== 'number' || !Number.isFinite(value) || value < d.min || value > d.max)
  )
    throw new Error(`属性范围无效：${name}`);
  if (d.type === 'boolean' && typeof value !== 'boolean') throw new Error(`布尔属性无效：${name}`);
  if (d.type === 'enum' && !d.values.includes(value)) throw new Error(`枚举属性无效：${name}`);
  if (['text', 'color'].includes(d.type) && (typeof value !== 'string' || !value.trim() || value.includes('\0') ||
      (d.maxLength && value.length > d.maxLength) || (d.pattern && !new RegExp(d.pattern).test(value))))
    throw new Error(`文字属性无效：${name}`);
}
/** @type {(template: import('./types.js').EffectTemplate) => Record<string, number | boolean | string>} */
export function defaultParameters(template) {
  return Object.fromEntries(
    Object.entries(template.parameters).map(([key, d]) => [key, d.default])
  );
}
/** @type {(clip: import('./types.js').Item['clip']) => import('./types.js').Item['clip']} */
export function enrichClip(clip) {
  clip.visual ??= {};
  clip.audio ??= {};
  for (const [path, descriptor] of Object.entries(PROPERTY_DESCRIPTORS))
    if ((path.startsWith('visual.') || path.startsWith('audio.')) && readPath(clip, path) === undefined) writePath(clip, path, descriptor.default);
  clip.retime ??= { version: 1, mode: 'constant', constantRatePpm: 1000000 };
  clip.automation ??= {};
  clip.effects ??= [];
  return clip;
}
function keys(value, allowed, label) {
  if (!value || typeof value !== 'object' || Array.isArray(value))
    throw new Error(`${label}必须是对象`);
  for (const key of Object.keys(value))
    if (!allowed.includes(key)) throw new Error(`${label}包含未知字段：${key}`);
}
function integer(value, min, max, label) {
  if (!Number.isSafeInteger(value) || value < min || value > max)
    throw new Error(`${label}必须是有效整数`);
}
export const HTML_MAX_BYTES = 1048576;
export const HTML_MAX_SIDE = 4096;
export const HTML_MAX_PIXELS = 8388608;
/** A bounded, JSON-persistable animation document. Media stays on native media tracks.
 * The media-tag check is deliberately lexical and conservative: `<audio` / `<video`
 * tags are rejected even in comments or JS strings. Runtime isolation is separate.
 * @type {(content: import('./types.js').HtmlContent) => import('./types.js').HtmlContent}
 */
export function validateHtmlContent(content) {
  keys(content, ['html', 'width', 'height', 'duration', 'transparent', 'variables'], 'HTML 动画');
  if (typeof content.html !== 'string' || !content.html.trim() || content.html.includes('\0') ||
      content.html.length > HTML_MAX_BYTES || new TextEncoder().encode(content.html).length > HTML_MAX_BYTES)
    throw new Error('HTML 动画内容无效或超过 1 MiB');
  if (/<\s*(?:audio|video)(?=[\s/>])/i.test(content.html))
    throw new Error('HTML 动画不能包含 audio/video 媒体元素，请使用现有音视频轨道');
  integer(content.width, 1, HTML_MAX_SIDE, 'HTML 动画宽度');
  integer(content.height, 1, HTML_MAX_SIDE, 'HTML 动画高度');
  if (content.width * content.height > HTML_MAX_PIXELS)
    throw new Error('HTML 动画总计不能超过 8388608 像素');
  integer(content.duration, 1, 120000 * 86400, 'HTML 动画时长');
  if (typeof content.transparent !== 'boolean') throw new Error('HTML 动画透明背景状态无效');
  if (content.variables !== undefined) {
    keys(content.variables, Object.keys(content.variables ?? {}), 'HTML 动画变量');
    if (Object.keys(content.variables).length > 100) throw new Error('HTML 动画变量最多 100 个');
    for (const [name, value] of Object.entries(content.variables)) {
      if (!/^[a-zA-Z_][a-zA-Z0-9_.-]{0,79}$/.test(name) || ['__proto__', 'prototype', 'constructor'].includes(name))
        throw new Error('HTML 动画变量名称无效');
      if (!['string', 'number', 'boolean'].includes(typeof value) ||
          (typeof value === 'number' && !Number.isFinite(value)) ||
          (typeof value === 'string' && (value.length > 10000 || value.includes('\0'))))
        throw new Error('HTML 动画变量必须是有界文字、有限数字或布尔值');
    }
  }
  return content;
}
/** @type {(project: import('./types.js').Project,seen: Set<string>) => void} */
export function validateExtensions(project, seen) {
  keys(
    project,
    ['format', 'version', 'id', 'name', 'canvas', 'frameRate', 'audio', 'assets', 'timeline'],
    '作品'
  );
  keys(project.canvas, ['width', 'height'], '画布');
  keys(project.frameRate, ['numerator', 'denominator'], '帧率');
  keys(project.audio, ['sampleRate', 'channels'], '音频设置');
  keys(project.timeline, ['id', 'tracks', 'groups', 'links', 'transitions'], '时间轴');
  for (const asset of project.assets)
    keys(
      asset,
      [
        'id',
        'name',
        'kind',
        'path',
        'duration',
        'size',
        'width',
        'height',
        'hasAudio',
        'sourceIdentity',
        'firstTimestamp',
        'streams',
        'codec',
        'fingerprint',
        'nativeProbe'
      ],
      '素材'
    );
  const newId = (id) => {
    if (typeof id !== 'string' || !/^[a-zA-Z0-9][a-zA-Z0-9_.:-]{0,159}$/.test(id) || seen.has(id))
      throw new Error('标识无效或重复');
    seen.add(id);
  };
  const items = new Map();
  for (const track of project.timeline.tracks) {
    keys(
      track,
      ['id', 'name', 'type', 'visible', 'muted', 'locked', 'syncLocked', 'items'],
      '轨道'
    );
    if (track.syncLocked !== undefined && typeof track.syncLocked !== 'boolean')
      throw new Error('同步锁状态无效');
    for (const item of track.items) {
      items.set(item.id, { item, track });
      keys(item, ['id', 'name', 'enabled', 'placement', 'clip'], '片段');
      keys(item.placement, ['begin', 'end'], '片段位置');
      const clip = item.clip;
      keys(
        clip,
        [
          'id',
          'type',
          'assetId',
          'source',
          'text',
          'html',
          'visual',
          'audio',
          'retime',
          'automation',
          'effects'
        ],
        '内容'
      );
      if (clip.type === 'html-clip') {
        validateHtmlContent(clip.html);
        if (clip.text !== undefined) throw new Error('HTML 动画不能同时包含文字片段内容');
      } else if (clip.html !== undefined) throw new Error('只有 html-clip 可以包含 HTML 动画内容');
      keys(clip.source, ['begin', 'end'], '素材范围');
      keys(
        clip.visual,
        [
          'positionX',
          'positionY',
          'scaleX',
          'scaleY',
          'rotationDegrees',
          'opacity',
          'anchorX',
          'anchorY',
          'crop',
          'fitPolicy',
          'flipHorizontal',
          'flipVertical',
          'blendMode'
        ],
        '画面属性'
      );
      keys(clip.audio, ['gainLinear', 'muted', 'fadeIn', 'fadeOut'], '音频属性');
      if (clip.text) {
        keys(clip.text, ['content', 'fontSize', 'color', 'fontFamily', 'font', 'template', 'layoutWidth'], '文字');
        if (clip.text.layoutWidth !== undefined && (!Number.isFinite(clip.text.layoutWidth) || clip.text.layoutWidth < 1 || clip.text.layoutWidth > 65536))
          throw new Error('文字框宽度必须为 1–65536');
        if (clip.text.font) {
          if (clip.text.template) throw new Error('文字模板使用自身字体，不能指定基础文字字体');
          const font = clip.text.font;
          keys(
            font,
            [
              'family',
              'postscriptName',
              'weight',
              'width',
              'slant',
              'sourceFaceIndex',
              'platform',
              'identity'
            ],
            '系统字体'
          );
          if (
            typeof font.family !== 'string' ||
            !font.family ||
            font.family.length > 256 ||
            font.family.includes('\0') ||
            typeof font.postscriptName !== 'string' ||
            font.postscriptName.length > 256 ||
            font.postscriptName.includes('\0') ||
            !Number.isInteger(font.weight) ||
            font.weight < 1 ||
            font.weight > 1000 ||
            !Number.isInteger(font.width) ||
            font.width < 1 ||
            font.width > 9 ||
            !['upright', 'italic', 'oblique'].includes(font.slant) ||
            !Number.isInteger(font.sourceFaceIndex) ||
            font.sourceFaceIndex < 0 ||
            font.sourceFaceIndex > 255 ||
            typeof font.platform !== 'string' ||
            !/^[a-z0-9._-]{1,64}$/.test(font.platform) ||
            typeof font.identity !== 'string' ||
            !/^[a-f0-9]{32}$/.test(font.identity)
          )
            throw new Error('系统字体身份无效');
        } else if (clip.text.font !== undefined) throw new Error('系统字体身份无效');
        if (clip.text.template)
          keys(clip.text.template, ['id', 'version', 'packageDigest', 'resourceBase', 'recipe', 'style'], '文字模板');
        if (clip.text.template?.recipe !== undefined)
          keys(clip.text.template.recipe, ['base', 'backdrop', 'animation'], '文字模板配方');
        if (clip.text.template?.style !== undefined) {
          keys(clip.text.template.style, ['color', 'fontSize'], '模板文字样式');
          for (const [key, value] of Object.entries(clip.text.template.style))
            validateProperty(`text.${key}`, value);
        }
        if (
          clip.text.template?.packageDigest !== undefined &&
          !/^sha256:[0-9a-f]{64}$/.test(clip.text.template.packageDigest)
        )
          throw new Error('模板摘要无效');
      }
      if (clip.visual.crop) keys(clip.visual.crop, ['left', 'top', 'right', 'bottom'], '裁剪');
      for (const [path, descriptor] of Object.entries(PROPERTY_DESCRIPTORS)) {
        const value = readPath(clip, path);
        if (value !== undefined) validateValue(descriptor, value, path);
        if (
          value !== undefined &&
          value !== descriptor.default &&
          !isPropertyApplicable(project, item, path)
        )
          throw new Error(`片段不支持此属性：${path}`);
      }
      const crop = clip.visual.crop;
      if (crop && (crop.left + crop.right >= 1 || crop.top + crop.bottom >= 1))
        throw new Error('裁剪范围必须保留可见画面');
      for (const key of ['fadeIn', 'fadeOut'])
        if (clip.audio[key] !== undefined)
          integer(clip.audio[key], 0, item.placement.end - item.placement.begin, '音频淡入淡出');
      if (clip.retime) {
        keys(clip.retime, ['version', 'mode', 'constantRatePpm'], '变速');
        if (clip.retime.version !== 1 || clip.retime.mode !== 'constant')
          throw new Error('仅支持恒速 TimeWarp 版本1');
        integer(clip.retime.constantRatePpm, 100000, 10000000, '变速倍率');
        const rate = clip.retime.constantRatePpm / 1000000;
        if (
          !Number.isFinite(rate) ||
          rate < 0.1 ||
          rate > 10 ||
          Math.abs(rate * 1e6 - Math.round(rate * 1e6)) > 1e-6
        )
          throw new Error('速度必须为 0.1–10，最多六位小数');
        if (!['video', 'audio', 'html-clip'].includes(clip.type) && rate !== 1)
          throw new Error('只有视频、音频和 HTML 动画可以变速');
      }
      if (clip.automation !== undefined) {
        keys(clip.automation, Object.keys(clip.automation), '关键帧');
        for (const [property, binding] of Object.entries(clip.automation)) {
          if (!descriptorForProperty(clip, property)?.keyframe)
            throw new Error(`属性不支持关键帧：${property}`);
          if (!isPropertyApplicable(project, item, property))
            throw new Error(`片段不支持此关键帧：${property}`);
          keys(binding, ['timeDomain', 'keyframes'], '关键帧绑定');
          if (binding.timeDomain !== 'itemLocal') throw new Error('仅支持片段局部时间关键帧');
          const frames = binding.keyframes;
          if (!Array.isArray(frames) || !frames.length || frames.length > 10000)
            throw new Error('关键帧数量无效');
          let previous = -1;
          for (const frame of frames) {
            keys(frame, ['id', 'time', 'value', 'interpolation', 'segment'], '关键帧');
            newId(frame.id);
            if (frame.segment) {
              keys(frame.segment, ['firstControl', 'secondControl'], '曲线形状');
              for (const key of ['firstControl', 'secondControl']) {
                keys(frame.segment[key], ['x', 'y'], '曲线控制点');
                const point = frame.segment[key];
                if (
                  !Number.isFinite(point.x) ||
                  point.x < 0 ||
                  point.x > 1 ||
                  !Number.isFinite(point.y) ||
                  point.y < 0 ||
                  point.y > 1
                )
                  throw new Error('曲线控制点无效');
              }
              if (frame.segment.firstControl.x > frame.segment.secondControl.x)
                throw new Error('曲线时间控制点无效');
            }
            integer(frame.time, 0, item.placement.end - item.placement.begin, '关键帧时间');
            if (frame.time <= previous) throw new Error('关键帧必须按时间递增且不能重复');
            previous = frame.time;
            validateProperty(property, frame.value, clip);
            if (!['linear', 'hold', 'easeIn', 'easeOut', 'easeInOut'].includes(frame.interpolation))
              throw new Error('关键帧插值无效');
          }
        }
      }
      if (clip.effects !== undefined) {
        if (!Array.isArray(clip.effects) || clip.effects.length > 32)
          throw new Error('特效数量无效');
        for (const effect of clip.effects) {
          keys(effect, ['id', 'templateId', 'enabled', 'parameters'], '特效');
          newId(effect.id);
          if (typeof effect.enabled !== 'boolean') throw new Error('特效启用状态无效');
          validateTemplate(effect, EFFECT_TEMPLATES);
          if (clip.type === 'audio') throw new Error('视觉特效不能应用到音频');
        }
      }
    }
  }
  for (const field of ['groups', 'links']) {
    const memberships = new Set();
    const entries = project.timeline[field] ?? [];
    if (!Array.isArray(entries) || entries.length > 10000) throw new Error('分组或关联数量无效');
    for (const entry of entries) {
      keys(entry, ['id', 'itemIds'], '分组或关联');
      newId(entry.id);
      if (!Array.isArray(entry.itemIds) || entry.itemIds.length < 2)
        throw new Error('分组或关联至少需要两个片段');
      for (const id of entry.itemIds) {
        if (!items.has(id) || memberships.has(id)) throw new Error('分组或关联引用无效或重复');
        memberships.add(id);
      }
    }
  }
  const transitions = project.timeline.transitions ?? [];
  if (!Array.isArray(transitions) || transitions.length > 10000) throw new Error('转场数量无效');
  const cuts = new Set(),
    windows = new Map();
  for (const transition of transitions) {
    keys(
      transition,
      ['id', 'fromItemId', 'toItemId', 'templateId', 'duration', 'parameters'],
      '转场'
    );
    newId(transition.id);
    validateTemplate(transition, TRANSITION_TEMPLATES);
    integer(transition.duration, 1, ticks(30), '转场时长');
    const from = items.get(transition.fromItemId),
      to = items.get(transition.toItemId);
    if (
      !from ||
      !to ||
      from.track !== to.track ||
      from.item.placement.end !== to.item.placement.begin ||
      from.item.id === to.item.id ||
      from.track.type === 'audio'
    )
      throw new Error('转场需要同轨相邻的视觉片段');
    if (cuts.has(transition.fromItemId)) throw new Error('剪切点已有转场');
    cuts.add(transition.fromItemId);
    const window = transitionWindow(project, transition);
    if (window.begin < from.item.placement.begin || window.end > to.item.placement.end)
      throw new Error('转场时间超过片段长度');
    const previous = windows.get(from.track.id) ?? [];
    if (previous.some((w) => window.begin < w.end && window.end > w.begin))
      throw new Error('转场窗口不能重叠');
    previous.push(window);
    windows.set(from.track.id, previous);
    for (const { item } of [from, to])
      if (['video', 'html-clip'].includes(item.clip.type)) {
        const asset = project.assets.find((a) => a.id === item.clip.assetId);
        const sourceDuration = item.clip.type === 'html-clip' ? item.clip.html.duration : asset.duration;
        const start = mapTimelineToSource(item, window.begin, { clamp: false });
        const end = mapTimelineToSource(item, window.end, { clamp: false });
        if (start < 0 || end > sourceDuration) throw new Error('转场需要足够的素材前后余量');
      }
  }
}
function validateTemplate(instance, catalog) {
  const template = catalog.find((t) => t.id === instance.templateId);
  if (!template) throw new Error(`未知模板：${instance.templateId}`);
  keys(instance.parameters, Object.keys(template.parameters), '模板参数');
  for (const [key, descriptor] of Object.entries(template.parameters))
    validateValue(descriptor, instance.parameters[key], key);
}
/** @type {(item: import('./types.js').Item,time: number,options?: { clamp?: boolean }) => number} */
export function mapTimelineToSource(item, time, { clamp = true } = {}) {
  if (!Number.isFinite(time)) throw new Error('时间无效');
  const { placement, clip } = item;
  const local = clamp
    ? Math.max(0, Math.min(time - placement.begin, placement.end - placement.begin))
    : time - placement.begin;
  return (
    clip.source.begin +
    Math.round((local * (clip.source.end - clip.source.begin)) / (placement.end - placement.begin))
  );
}
/** Text has an extensible source clock; rebasing keeps extended ranges nonnegative. */
/** @type {(item: import('./types.js').Item,begin: number,end: number) => { begin: number; end: number }} */
export function trimSourceRange(item, begin, end) {
  const sourceBegin = mapTimelineToSource(item, begin, { clamp: false });
  const sourceEnd = mapTimelineToSource(item, end, { clamp: false });
  const offset = item.clip.type === 'text' ? Math.max(0, -sourceBegin) : 0;
  return { begin: sourceBegin + offset, end: sourceEnd + offset };
}
/** @type {(item: import('./types.js').Item,time: number,options?: { clamp?: boolean }) => number} */
export function mapSourceToTimeline(item, time, { clamp = true } = {}) {
  if (!Number.isFinite(time)) throw new Error('时间无效');
  const { placement, clip } = item;
  const local = clamp
    ? Math.max(0, Math.min(time - clip.source.begin, clip.source.end - clip.source.begin))
    : time - clip.source.begin;
  return (
    placement.begin +
    Math.round((local * (placement.end - placement.begin)) / (clip.source.end - clip.source.begin))
  );
}
/** @type {(time: number,frameRate: import('./types.js').Project['frameRate']) => number} */
export function quantizeTime(value, frameRate) {
  if (!Number.isFinite(value)) throw new Error('时间无效');
  const frameIndex = Math.round((value * frameRate.numerator) / (120000 * frameRate.denominator));
  return Math.round((frameIndex * 120000 * frameRate.denominator) / frameRate.numerator);
}
/** @type {(item: import('./types.js').Item,property: string,itemLocalTime: number) => number | boolean | string} */
export function sampleProperty(item, property, itemLocalTime) {
  const descriptor = getPropertyDescriptor(item, property);
  if (!descriptor) throw new Error(`未知属性：${property}`);
  if (property === 'time.start') return item.placement.begin;
  if (property === 'time.duration') return item.placement.end - item.placement.begin;
  if (property === 'time.sourceIn') return item.clip.source.begin;
  if (property === 'time.speed') return (item.clip.retime?.constantRatePpm ?? 1000000) / 1e6;
  const frames = item.clip.automation?.[property]?.keyframes;
  if (!frames?.length) return readPath(item.clip, property) ?? descriptor.default;
  if (itemLocalTime <= frames[0].time) return frames[0].value;
  if (itemLocalTime >= frames.at(-1).time) return frames.at(-1).value;
  let low = 0,
    high = frames.length - 1;
  while (high - low > 1) {
    const mid = (low + high) >> 1;
    if (frames[mid].time <= itemLocalTime) low = mid;
    else high = mid;
  }
  const a = frames[low],
    b = frames[high];
  let u = (itemLocalTime - a.time) / (b.time - a.time);
  if (a.segment) {
    const controls = segmentControls(a);
    u = cubicPoint(controls, solveBezierX(controls, u)).y;
  } else if (a.interpolation === 'hold') u = 0;
  else if (a.interpolation === 'easeIn') u = u * u * u;
  else if (a.interpolation === 'easeOut') u = 1 - (1 - u) * (1 - u) * (1 - u);
  else if (a.interpolation === 'easeInOut')
    u = u < 0.5 ? 4 * u * u * u : 1 - Math.pow(-2 * u + 2, 3) / 2;
  return a.value + (b.value - a.value) * u;
}
function segmentControls(frame) {
  const shapes = { linear: [1 / 3, 2 / 3], easeIn: [0, 0], easeOut: [1, 1] };
  const y = shapes[frame.interpolation] ?? shapes.linear;
  return [
    { x: 0, y: 0 },
    frame.segment?.firstControl ?? { x: 1 / 3, y: y[0] },
    frame.segment?.secondControl ?? { x: 2 / 3, y: y[1] },
    { x: 1, y: 1 }
  ];
}
function mixPoint(a, b, t) {
  return { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t };
}
function splitBezier(p, t) {
  const a = mixPoint(p[0], p[1], t),
    b = mixPoint(p[1], p[2], t),
    c = mixPoint(p[2], p[3], t),
    d = mixPoint(a, b, t),
    e = mixPoint(b, c, t),
    f = mixPoint(d, e, t);
  return [
    [p[0], a, d, f],
    [f, e, c, p[3]]
  ];
}
function cubicPoint(p, t) {
  return splitBezier(p, t)[0][3];
}
function solveBezierX(p, x) {
  let lo = 0,
    hi = 1;
  for (let n = 0; n < 48; n++) {
    const mid = (lo + hi) / 2;
    if (cubicPoint(p, mid).x < x) lo = mid;
    else hi = mid;
  }
  return (lo + hi) / 2;
}
function restrictSegment(frame, a, b) {
  if (!frame.segment && frame.interpolation === 'easeInOut') {
    if (b <= 0.5 + 1e-12) {
      frame = { interpolation: 'easeIn' };
      a *= 2;
      b *= 2;
    } else {
      frame = { interpolation: 'easeOut' };
      a = (a - 0.5) * 2;
      b = (b - 0.5) * 2;
    }
  }
  const controls = segmentControls(frame),
    start = solveBezierX(controls, a),
    end = solveBezierX(controls, b);
  let p = splitBezier(controls, end)[0];
  if (start > 0) p = splitBezier(p, start / end)[1];
  const dx = p[3].x - p[0].x,
    dy = p[3].y - p[0].y;
  if (dx < 1e-12 || Math.abs(dy) < 1e-12) return undefined;
  const normalize = (q) => ({
    x: Math.max(0, Math.min(1, (q.x - p[0].x) / dx)),
    y: Math.max(0, Math.min(1, (q.y - p[0].y) / dy))
  });
  return { firstControl: normalize(p[1]), secondControl: normalize(p[2]) };
}
/** Slice the exact Bezier segment, preserving motion after repeated trims/splits. */
/** @type {(item: import('./types.js').Item,begin: number,end: number,options?: { freshIds?: boolean }) => Record<string, import('./types.js').AutomationBinding>} */
export function sliceAutomation(item, begin, end, { freshIds = false } = {}) {
  const result = {};
  for (const [property, binding] of Object.entries(item.clip.automation ?? {})) {
    const frames = binding.keyframes;
    const midpoints = frames
      .flatMap((f, n) =>
        !f.segment && f.interpolation === 'easeInOut' && frames[n + 1]
          ? [(f.time + frames[n + 1].time) / 2]
          : []
      )
      .filter((t) => t > begin && t < end);
    const points = [
      ...new Set([
        begin,
        ...frames.filter((f) => f.time > begin && f.time < end).map((f) => f.time),
        ...midpoints.map(Math.round),
        end
      ])
    ].sort((a, b) => a - b);
    const selected = points.map((time, index) => {
      const exact = frames.find((f) => f.time === time);
      const frame = {
        id: (!freshIds && exact?.id) || identity('keyframe'),
        time: time - begin,
        value: sampleProperty(item, property, time),
        interpolation: 'linear'
      };
      if (index === points.length - 1) return frame;
      const next = points[index + 1],
        left = frames.findLast((f) => f.time <= time),
        right = left && frames[frames.indexOf(left) + 1];
      if (!left || !right) {
        frame.interpolation = 'hold';
        return frame;
      }
      frame.interpolation = left.interpolation;
      if (left.interpolation !== 'hold' && (left.interpolation !== 'linear' || left.segment)) {
        const segment = restrictSegment(
          left,
          (time - left.time) / (right.time - left.time),
          (next - left.time) / (right.time - left.time)
        );
        if (segment) frame.segment = segment;
      }
      return frame;
    });
    result[property] = { timeDomain: 'itemLocal', keyframes: selected };
  }
  return result;
}
/** @type {(project: import('./types.js').Project,transition: import('./types.js').TransitionInstance) => { begin: number; end: number }} */
export function transitionWindow(project, transition) {
  const from = project.timeline.tracks
    .flatMap((t) => t.items)
    .find((i) => i.id === transition.fromItemId);
  if (!from) throw new Error('转场引用无效');
  const begin = from.placement.end - Math.floor(transition.duration / 2);
  return { begin, end: begin + transition.duration };
}
/** @type {(project: import('./types.js').Project,time: number) => { time: number; canvas: import('./types.js').Project['canvas']; layers: import('./types.js').FrameLayer[]; audio: import('./types.js').FrameLayer[] }} */
export function evaluateFrame(project, time) {
  if (!Number.isFinite(time)) throw new Error('时间无效');
  const activeTransitions = (project.timeline.transitions ?? []).flatMap((t) => {
    const w = transitionWindow(project, t);
    return time >= w.begin && time < w.end
      ? [{ ...t, progress: (time - w.begin) / (w.end - w.begin) }]
      : [];
  });
  const layers = [],
    audio = [];
  for (const track of [...project.timeline.tracks].reverse())
    for (const item of track.items) {
      if (!item.enabled) continue;
      const transition = activeTransitions.find(
        (t) => t.fromItemId === item.id || t.toItemId === item.id
      );
      const active = time >= item.placement.begin && time < item.placement.end;
      if (!active && !transition) continue;
      const itemLocalTime = time - item.placement.begin;
      const visual = {},
        sound = {};
      for (const path of Object.keys(PROPERTY_DESCRIPTORS))
        writePath({ visual, audio: sound }, path, sampleProperty(item, path, itemLocalTime));
      const length = item.placement.end - item.placement.begin;
      if (sound.fadeIn) sound.gainLinear *= Math.min(1, Math.max(0, itemLocalTime / sound.fadeIn));
      if (sound.fadeOut)
        sound.gainLinear *= Math.min(1, Math.max(0, (length - itemLocalTime) / sound.fadeOut));
      const layer = {
        itemId: item.id,
        trackId: track.id,
        assetId: item.clip.assetId,
        type: item.clip.type,
        item,
        sourceTime: mapTimelineToSource(item, time, { clamp: !transition }),
        itemLocalTime,
        visual,
        audio: sound,
        effects: evaluateEffects(item, time)
      };
      if (transition)
        layer.transition = {
          id: transition.id,
          templateId: transition.templateId,
          progress: transition.progress,
          role: transition.fromItemId === item.id ? 'from' : 'to',
          parameters: transition.parameters
        };
      if (track.visible && item.clip.type !== 'audio') layers.push(layer);
      if (
        active &&
        !track.muted &&
        !sound.muted &&
        (item.clip.type === 'audio' ||
          project.assets.find((a) => a.id === item.clip.assetId)?.hasAudio)
      )
        audio.push(layer);
    }
  return { time, canvas: project.canvas, layers, audio };
}
/** @type {(item: import('./types.js').Item,time: number) => import('./types.js').EffectInstance[]} */
export function evaluateEffects(item, time) {
  return (item.clip.effects ?? [])
    .filter((effect) => effect.enabled)
    .map((effect) => ({
      ...effect,
      parameters: Object.fromEntries(
        Object.keys(effect.parameters).map((parameter) => [
          parameter,
          sampleProperty(item, `effects.${effect.id}.${parameter}`, time - item.placement.begin)
        ])
      )
    }));
}
/** Copies must not leave animation paths attached to the old effect identity. */
/** @type {(clip: import('./types.js').Item['clip']) => void} */
export function renewEffectIdentities(clip) {
  for (const effect of clip.effects ?? []) {
    const previous = effect.id;
    effect.id = identity('effect');
    for (const path of Object.keys(clip.automation ?? {})) {
      const end = path.lastIndexOf('.');
      if (path.startsWith('effects.') && path.slice(8, end) === previous) {
        clip.automation[`effects.${effect.id}${path.slice(end)}`] = clip.automation[path];
        delete clip.automation[path];
      }
    }
  }
}
/** @type {(item: import('./types.js').Item,time: number) => Required<import('./types.js').VisualProperties>} */
export function evaluateVisual(item, time) {
  const visual = {};
  for (const property of Object.keys(PROPERTY_DESCRIPTORS).filter((p) => p.startsWith('visual.')))
    writePath({ visual }, property, sampleProperty(item, property, time - item.placement.begin));
  return visual;
}
/** @type {(item: import('./types.js').Item,time: number) => Required<import('./types.js').AudioProperties>} */
export function evaluateAudio(item, time) {
  const audio = {};
  for (const property of Object.keys(PROPERTY_DESCRIPTORS).filter((p) => p.startsWith('audio.')))
    writePath({ audio }, property, sampleProperty(item, property, time - item.placement.begin));
  const local = time - item.placement.begin,
    length = item.placement.end - item.placement.begin;
  if (audio.fadeIn) audio.gainLinear *= Math.min(1, Math.max(0, local / audio.fadeIn));
  if (audio.fadeOut) audio.gainLinear *= Math.min(1, Math.max(0, (length - local) / audio.fadeOut));
  return audio;
}
/** @type {(project: import('./types.js').Project,time: number) => (import('./types.js').TransitionInstance & { begin: number; end: number; progress: number })[]} */
export function activeTransitions(project, time) {
  return (project.timeline.transitions ?? []).flatMap((t) => {
    const w = transitionWindow(project, t);
    return time >= w.begin && time < w.end
      ? [{ ...t, ...w, progress: (time - w.begin) / (w.end - w.begin) }]
      : [];
  });
}
