import { DOMParser, XMLSerializer } from '@xmldom/xmldom';

export const PAGX_MAX_BYTES = 4 * 1024 * 1024;
const parsed = new Map();
const elements = (doc) => Array.from(doc.getElementsByTagName('*'));

/** Shared admission boundary: a self-contained document driven by one absolute clock.
 * @param {string} xml
 */
export function parsePagx(xml) {
  if (
    typeof xml !== 'string' ||
    !xml.trim() ||
    new TextEncoder().encode(xml).length > PAGX_MAX_BYTES
  )
    throw new Error('PAGX XML 内容无效或超过 4 MiB');
  if (/<!DOCTYPE|<!ENTITY/i.test(xml)) throw new Error('PAGX 不允许外部实体或 DTD');
  if (parsed.has(xml)) return parsed.get(xml).cloneNode(true);
  const doc = new DOMParser({
    onError: (level, message) => {
      throw new Error(`PAGX XML: ${message}`);
    }
  }).parseFromString(xml, 'application/xml');
  if (doc.documentElement?.tagName !== 'pagx') throw new Error('PAGX 根元素必须是 pagx');
  const nodes = elements(doc);
  if (nodes.length > 20000) throw new Error('PAGX 节点过多');
  for (const node of nodes) {
    if (
      ['StateMachine', 'StateMachines', 'DataBind', 'Video', 'Audio', 'svg'].includes(
        node.tagName
      ) ||
      node.hasAttribute('import')
    )
      throw new Error(`PAGX 时间轴暂不支持 ${node.tagName} 状态机、媒体或未展开的导入指令`);
    for (const attr of Array.from(node.attributes))
      if (
        ['file', 'source', 'src', 'href'].includes(attr.name) &&
        !/^data:[^,]+,/i.test(attr.value)
      )
        throw new Error('PAGX 外部资源必须先导入并内嵌');
    if (
      (node.hasAttribute('composition') && !/^@\S+$/.test(node.getAttribute('composition'))) ||
      (['ImagePattern', 'Glyph'].includes(node.tagName) &&
        node.hasAttribute('image') &&
        !/^(?:@\S+$|data:[^,]+,)/i.test(node.getAttribute('image')))
    )
      throw new Error('PAGX 外部资源必须先导入并内嵌');
    if (node.tagName === 'Font' && node.hasAttribute('file'))
      throw new Error('PAGX 字体文件引用需要先用官方 CLI 嵌入字形，或使用系统字体');
    if (node.tagName === 'Timelines')
      throw new Error('PAGX 嵌套时间线尚不支持精确定位，请合并到顶层动画');
    if (
      node.tagName === 'Animation' &&
      (node.parentNode?.nodeName !== 'Animations' ||
        node.parentNode.parentNode !== doc.documentElement)
    )
      throw new Error('PAGX 动画必须位于顶层 Animations 中');
    if (
      node.tagName === 'Animation' &&
      node.hasAttribute('loop') &&
      node.getAttribute('loop') !== 'once'
    )
      throw new Error('PAGX 循环需要展开为有限时长的 once 动画');
    if (node.hasAttribute('matrix3D') || node.getAttribute('preserve3D') === 'true')
      throw new Error('PAGX 3D 动画尚不支持');
    if (node.tagName === 'Channel' && /^point\d+\./.test(node.getAttribute('name')))
      throw new Error(
        '当前 PAGX WASM 不支持路径控制点动画（包括动态 clip-path）；请使用受支持的 PAGX 图层与动画通道重做'
      );
  }
  if (doc.getElementsByTagName('Animation').length > 1)
    throw new Error('PAGX 需要一个顶层动画；请将动画通道合并到同一 Animation');
  const root = doc.documentElement;
  for (const key of ['width', 'height']) {
    const n = Number(root.getAttribute(key));
    if (!Number.isSafeInteger(n) || n < 1 || n > 4096)
      throw new Error(`PAGX ${key} 必须为 1–4096 整数`);
  }
  if (Number(root.getAttribute('width')) * Number(root.getAttribute('height')) > 8388608)
    throw new Error('PAGX 画布超过 8388608 像素');
  parsed.set(xml, doc);
  while (parsed.size > 8) parsed.delete(parsed.keys().next().value);
  return doc.cloneNode(true);
}

/** @param {string} xml */
export function pagxMetadata(xml) {
  const doc = parsePagx(xml),
    root = doc.documentElement;
  const animation = doc.getElementsByTagName('Animation')[0];
  const fps = animation ? Number(animation.getAttribute('frameRate') || 60) : 30;
  const frames = animation ? Number(animation.getAttribute('duration')) : 0;
  if (
    !Number.isFinite(fps) ||
    fps <= 0 ||
    fps > 240 ||
    !Number.isFinite(frames) ||
    frames < 0 ||
    (animation && !frames)
  )
    throw new Error('PAGX 动画帧率或时长无效');
  return {
    width: Number(root.getAttribute('width')),
    height: Number(root.getAttribute('height')),
    animationDuration: Math.round((frames / fps) * 120000),
    frameRate: fps,
    animated: !!animation
  };
}

/** @param {import('./types.js').PagxContent} content */
export function validatePagxContent(content) {
  if (
    !content ||
    typeof content !== 'object' ||
    Array.isArray(content) ||
    Object.keys(content).some(
      (key) => !['xml', 'width', 'height', 'duration', 'transparent'].includes(key)
    )
  )
    throw new Error('PAGX 动画数据无效');
  const meta = pagxMetadata(content.xml);
  if (content.width !== meta.width || content.height !== meta.height)
    throw new Error('PAGX 画布尺寸与 XML 不一致');
  if (
    !Number.isSafeInteger(content.duration) ||
    content.duration < 1 ||
    content.duration > 120000 * 86400
  )
    throw new Error('PAGX 时长必须为有效的 120000 Hz ticks');
  if (meta.animationDuration > 0 && content.duration > meta.animationDuration)
    throw new Error('PAGX 片段时长超过动画时长');
  if (typeof content.transparent !== 'boolean') throw new Error('PAGX 透明背景状态无效');
  return content;
}

/** @param {string} xml
 * @param {{duration?: number, transparent?: boolean}} [options]
 */
export function createPagxContent(xml, options = {}) {
  const meta = pagxMetadata(xml);
  return validatePagxContent({
    xml,
    width: meta.width,
    height: meta.height,
    duration: options.duration ?? (meta.animationDuration || 600000),
    transparent: options.transparent ?? true
  });
}

const fieldSpec = {
  text: ['文字', 'text'],
  fontFamily: ['字体', 'font'],
  fontSize: ['字号', 'number', 1, 1000],
  fauxBold: ['加粗', 'boolean'],
  fauxItalic: ['斜体', 'boolean'],
  letterSpacing: ['字距', 'number', -200, 1000],
  color: ['颜色', 'color'],
  width: ['宽度', 'number', 0, 16384],
  height: ['高度', 'number', 0, 16384],
  left: ['左侧位置', 'number', -16384, 16384],
  top: ['顶部位置', 'number', -16384, 16384],
  x: ['水平偏移', 'number', -16384, 16384],
  y: ['垂直偏移', 'number', -16384, 16384],
  rotation: ['旋转', 'number', -36000, 36000],
  alpha: ['透明度', 'number', 0, 1],
  roundness: ['圆角', 'number', 0, 8192],
  blurX: ['水平模糊', 'number', 0, 500],
  blurY: ['垂直模糊', 'number', 0, 500]
};
const textDefaults = {
  fontFamily: 'system',
  fontSize: '16',
  fauxBold: 'false',
  fauxItalic: 'false',
  letterSpacing: '0'
};
const geometryNodes = new Set(['Layer', 'Group', 'Rectangle', 'Ellipse', 'TextBox']);

/** Parse editable semantic values; animated base values are displayed as read-only.
 * @param {string} xml
 */
export function pagxFields(xml) {
  const doc = parsePagx(xml),
    nodes = elements(doc),
    animated = new Set();
  for (const object of Array.from(doc.getElementsByTagName('Object')))
    for (const channel of Array.from(object.getElementsByTagName('Channel')))
      animated.add(
        object.getAttribute('target').replace(/^@/, '') + ':' + channel.getAttribute('name')
      );
  const fields = [];
  for (const [index, node] of nodes.entries()) {
    let attrs = [];
    if (node.tagName === 'Text') attrs = ['text', ...Object.keys(textDefaults)];
    else if (
      ['Fill', 'Stroke', 'SolidColor', 'ColorStop', 'DropShadowStyle', 'DropShadowFilter'].includes(
        node.tagName
      )
    )
      attrs = ['color', ...(node.tagName === 'Stroke' ? ['width'] : [])];
    else if (geometryNodes.has(node.tagName))
      attrs = [
        'width',
        'height',
        'left',
        'top',
        'x',
        'y',
        'rotation',
        'alpha',
        ...(node.tagName === 'Rectangle' ? ['roundness'] : [])
      ];
    if (['BlurFilter', 'DropShadowStyle', 'DropShadowFilter'].includes(node.tagName))
      attrs.push('blurX', 'blurY');
    let owner = node;
    while (
      owner &&
      owner.nodeType === 1 &&
      !owner.getAttribute('name') &&
      !owner.getAttribute('data-label')
    )
      owner = owner.parentNode;
    const group =
      owner?.nodeType === 1
        ? owner.getAttribute('data-label') || owner.getAttribute('name')
        : node.tagName === 'Text'
          ? '文字内容'
          : ['Fill', 'Stroke', 'SolidColor', 'ColorStop'].includes(node.tagName)
            ? '颜色与描边'
            : '图形属性';
    for (const attr of attrs) {
      const [label, type, min, max] = fieldSpec[attr];
      const value = node.hasAttribute(attr)
        ? node.getAttribute(attr)
        : node.tagName === 'Text'
          ? textDefaults[attr]
          : undefined;
      if (
        value === undefined ||
        (type === 'color' && !/^#(?:[a-f\d]{3,4}|[a-f\d]{6}|[a-f\d]{8})$/i.test(value)) ||
        (type === 'number' && !/^-?(?:\d+\.?\d*|\.\d+)$/.test(value))
      )
        continue;
      fields.push({
        key: `${index}:${attr}`,
        group,
        label: attr === 'width' && node.tagName === 'Stroke' ? '描边宽度' : label,
        type,
        value,
        min,
        max,
        animated: [
          attr,
          ...(attr === 'left'
            ? ['x', 'matrix']
            : attr === 'top'
              ? ['y', 'matrix']
              : attr === 'rotation'
                ? ['matrix']
                : [])
        ].some((channel) => animated.has(node.getAttribute('id') + ':' + channel))
      });
    }
  }
  return fields;
}

/** Atomically update the parsed properties and source canvas/clock.
 * @param {import('./types.js').PagxContent} content
 * @param {Record<string,string>} values
 * @param {{width?:number,height?:number,duration?:number,transparent?:boolean}} [options]
 */
export function updatePagxContent(content, values, options = {}) {
  const fields = pagxFields(content.xml),
    doc = parsePagx(content.xml),
    nodes = elements(doc);
  for (const [key, value] of Object.entries(values)) {
    const field = fields.find((f) => f.key === key);
    if (
      !field ||
      field.animated ||
      typeof value !== 'string' ||
      value.length > 10000 ||
      value.includes('\0')
    )
      throw new Error('PAGX 属性无效或由动画控制');
    if (
      field.type === 'number' &&
      (!value.trim() ||
        !Number.isFinite(Number(value)) ||
        Number(value) < field.min ||
        Number(value) > field.max)
    )
      throw new Error(`${field.label}需要 ${field.min}–${field.max}`);
    if (field.type === 'color' && !/^#(?:[a-f\d]{3,4}|[a-f\d]{6}|[a-f\d]{8})$/i.test(value))
      throw new Error('PAGX 颜色需要十六进制颜色值');
    if (field.type === 'boolean' && !['true', 'false'].includes(value))
      throw new Error('PAGX 布尔属性无效');
    if (field.type === 'font' && (!value.trim() || value.length > 256))
      throw new Error('PAGX 字体名称无效');
    const [index, attr] = key.split(':');
    nodes[Number(index)].setAttribute(attr, value);
  }
  for (const key of ['width', 'height'])
    if (options[key] !== undefined) doc.documentElement.setAttribute(key, String(options[key]));
  if (options.duration !== undefined && options.duration !== content.duration) {
    if (
      !Number.isSafeInteger(options.duration) ||
      options.duration < 1 ||
      options.duration > 120000 * 86400
    )
      throw new Error('PAGX 时长无效');
    const animation = doc.getElementsByTagName('Animation')[0];
    if (animation) {
      const old = Number(animation.getAttribute('duration')),
        frames = Math.max(
          1,
          Math.ceil((options.duration / 120000) * Number(animation.getAttribute('frameRate') || 60))
        );
      animation.setAttribute('duration', String(frames));
      for (const key of Array.from(animation.getElementsByTagName('Key')))
        key.setAttribute(
          'time',
          String(Math.round((Number(key.getAttribute('time')) * frames) / old))
        );
    }
  }
  return validatePagxContent({
    ...content,
    ...options,
    xml: new XMLSerializer().serializeToString(doc)
  });
}

/** @param {import('./types.js').PagxContent} content @param {string} key @param {string} value */
export function setPagxField(content, key, value) {
  return updatePagxContent(content, { [key]: value });
}
