import { parse, parseFragment, type DefaultTreeAdapterMap } from 'parse5';
import type { HtmlContent } from './html-presets';

export interface HtmlProperty {
  id: string;
  group: 'text' | 'style' | 'variable';
  label: string;
  context: string;
  labelIsAuthored?: boolean;
  contextIsAuthored?: boolean;
  kind: 'text' | 'number' | 'color' | 'font' | 'boolean';
  value: string | number | boolean;
  original: string | number | boolean;
  unit?: string;
  start?: number;
  end?: number;
  encoding?: 'text' | 'attribute';
  variable?: string;
  cssProperty?: string;
}

const labels: Record<string, string> = {
  color: '文字颜色',
  background: '背景颜色',
  'background-color': '背景颜色',
  fill: '填充颜色',
  stroke: '描边颜色',
  'border-color': '边框颜色',
  'font-family': '字体',
  'font-size': '字号',
  'font-weight': '字重',
  'line-height': '行高',
  'letter-spacing': '字间距',
  'word-spacing': '词间距',
  width: '宽度',
  height: '高度',
  'min-width': '最小宽度',
  'max-width': '最大宽度',
  'min-height': '最小高度',
  'max-height': '最大高度',
  left: '左侧位置',
  right: '右侧位置',
  top: '顶部位置',
  bottom: '底部位置',
  opacity: '不透明度',
  padding: '内边距',
  margin: '外边距',
  gap: '间距',
  'row-gap': '行间距',
  'column-gap': '列间距',
  'border-radius': '圆角',
  'border-width': '边框宽度',
  'stroke-width': '描边宽度'
};
for (const side of ['top', 'right', 'bottom', 'left']) {
  const name = { top: '上', right: '右', bottom: '下', left: '左' }[side];
  labels[`padding-${side}`] = `${name}内边距`;
  labels[`margin-${side}`] = `${name}外边距`;
  labels[`border-${side}-width`] = `${name}边框宽度`;
  labels[`border-${side}-color`] = `${name}边框颜色`;
}
const numeric = /^[+-]?(?:\d+\.?\d*|\.\d+)(px|%|em|rem|vh|vw|vmin|vmax|s|ms|deg)?$/i;
const commonColors = new Set(
  'transparent black white red green blue yellow orange purple pink gray grey cyan magenta lime teal navy silver maroon olive aqua fuchsia rebeccapurple'.split(
    ' '
  )
);
export function isHtmlColor(value: string): boolean {
  // CSS.supports accepts otherwise invalid color expressions containing var().
  // Only expose resolved color values, never gradient or variable expressions.
  if (/\b(?:var|env)\(/i.test(value)) return false;
  return typeof CSS !== 'undefined'
    ? CSS.supports('color', value) &&
        !/^(?:inherit|initial|unset|revert|currentcolor|var\()/i.test(value)
    : /^(?:#[\da-f]{3,4}|#[\da-f]{6}|#[\da-f]{8})$/i.test(value) ||
        commonColors.has(value.toLowerCase()) ||
        /^(?:rgb|hsl)a?\([\d\s.,%+/-]+\)$/i.test(value);
}

// Scan structural CSS delimiters while leaving strings, comments and functions intact.
function delimiters(source: string, start: number, end: number, encoded = false) {
  const result: { at: number; end: number; char: string }[] = [];
  let quote = '',
    depth = 0;
  for (let i = start; i < end; i++) {
    const at = i;
    let char = source[i];
    if (encoded && char === '&') {
      const entity = /^&(?:#x[\da-f]+|#\d+|[a-z]+);/i.exec(source.slice(i, end))?.[0];
      if (entity) {
        char = decodeAttribute(entity);
        i += entity.length - 1;
      }
    }
    if (char === '\\') {
      i++;
      continue;
    }
    if (quote) {
      if (char === quote) quote = '';
      continue;
    }
    if (char === '"' || char === "'") {
      quote = char;
      continue;
    }
    if (char === '/' && source[i + 1] === '*') {
      const close = source.indexOf('*/', i + 2);
      i = close < 0 ? end : close + 1;
      continue;
    }
    if (char === '(' || char === '[') depth++;
    else if (char === ')' || char === ']') depth--;
    else if (!depth && ';:{}'.includes(char)) result.push({ at, end: i + 1, char });
  }
  return result;
}
const clean = (value: string) => value.replace(/\/\*[\s\S]*?\*\//g, '').trim();
const escapeHtml = (value: string) =>
  value.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const decodeAttribute = (value: string) => {
  const node = parseFragment(`<i data-value="${value.replace(/"/g, '&quot;')}"></i>`).childNodes[0];
  return 'attrs' in node ? node.attrs[0].value : value;
};

export function parseHtmlProperties(content: HtmlContent): HtmlProperty[] {
  const source = content.html,
    fields: HtmlProperty[] = [];
  const parsedStyles = new Set<number>();
  let textCount = 0;
  const authoredContexts = new Set<string>();
  const add = (field: Omit<HtmlProperty, 'id' | 'original'>) =>
    fields.push({
      ...field,
      ...(field.group === 'variable' && field.label === field.variable
        ? { labelIsAuthored: true }
        : {}),
      ...(field.group === 'style' && authoredContexts.has(field.context)
        ? { contextIsAuthored: true }
        : {}),
      id: `html-property-${fields.length}`,
      original: field.value
    });
  const authoredLabel = (node: DefaultTreeAdapterMap['element']) =>
    node.attrs.find((attr) => ['aria-label', 'data-label'].includes(attr.name))?.value;
  const title = (node: DefaultTreeAdapterMap['element']) => {
    const tag = node.tagName;
    return (
      authoredLabel(node) ||
      (/^h[1-6]$/.test(tag)
        ? '标题'
        : tag === 'p'
          ? '正文'
          : ['html', 'body'].includes(tag)
            ? '页面'
            : '元素')
    );
  };
  const names = new Map<string, string>();
  let elements = 0;
  const tree = parse(source, { sourceCodeLocationInfo: true });
  const index = (node: DefaultTreeAdapterMap['node']) => {
    if ('tagName' in node) {
      const name = `${title(node)} ${++elements}`;
      if (authoredLabel(node)) {
        authoredContexts.add(title(node));
        authoredContexts.add(name);
      }
      names.set(node.tagName, title(node));
      for (const attr of node.attrs) {
        if (attr.name === 'id') names.set(`#${attr.value}`, name);
        if (attr.name === 'class')
          for (const cls of attr.value.split(/\s+/)) names.set(`.${cls}`, name);
      }
    }
    if ('childNodes' in node) node.childNodes.forEach(index);
  };
  index(tree);
  const styleValue = (
    property: string,
    start: number,
    end: number,
    context: string,
    encoding?: 'attribute'
  ) => {
    const raw = source.slice(start, end);
    const token = raw
      .trim()
      .replace(/\s*!important\s*$/i, '')
      .trim();
    const value = encoding ? decodeAttribute(token) : token;
    if (!value || /\/\*|[<>]/.test(value) || (!encoding && value.includes('&'))) return;
    start += raw.indexOf(token);
    end = start + token.length;
    const range = { group: 'style' as const, context, start, end, encoding, cssProperty: property };
    if (
      isHtmlColor(value) &&
      (property.startsWith('--') || /color$|^(?:background|fill|stroke)$/.test(property))
    ) {
      add({ ...range, kind: 'color', label: labels[property] || '颜色', value });
    } else if (property === 'font-family' && !/[;{}<>]|var\(/i.test(value)) {
      add({ ...range, kind: 'font', label: '字体', value });
    } else if ((labels[property] || property.startsWith('--')) && numeric.test(value)) {
      const unit = value.match(/[a-z%]+$/i)?.[0] || '';
      add({
        ...range,
        kind: 'number',
        label: labels[property] || '数值',
        value: Number(value.slice(0, value.length - unit.length)),
        unit
      });
    }
  };
  const declarations = (start: number, end: number, context: string, encoding?: 'attribute') => {
    const tokens = delimiters(source, start, end, Boolean(encoding));
    // Nested rules are left intact rather than mistaken for declarations.
    if (tokens.some((token) => '{}'.includes(token.char))) return;
    for (const boundary of [
      ...tokens.filter((token) => token.char === ';'),
      { at: end, end, char: ';' }
    ]) {
      const colon = tokens.find(
        (token) => token.char === ':' && token.at >= start && token.at < boundary.at
      );
      if (colon)
        styleValue(
          clean(source.slice(start, colon.at)).toLowerCase(),
          colon.end,
          boundary.at,
          context,
          encoding
        );
      start = boundary.end;
    }
  };
  const stylesheet = (start: number, end: number) => {
    const tokens = delimiters(source, start, end);
    let ruleStart = start,
      ruleNumber = 0;
    for (let i = 0; i < tokens.length; i++) {
      const token = tokens[i];
      if (token.char === ';') {
        ruleStart = token.at + 1;
        continue;
      }
      if (token.char !== '{') continue;
      let depth = 1,
        j = i + 1;
      for (; j < tokens.length; j++) {
        if (tokens[j].char === '{') depth++;
        if (tokens[j].char === '}' && --depth === 0) break;
      }
      if (j === tokens.length) break;
      const selector = clean(source.slice(ruleStart, token.at));
      if (/^@(?:media|supports|container|layer)\b/i.test(selector))
        stylesheet(token.at + 1, tokens[j].at);
      else if (!selector.startsWith('@'))
        declarations(token.at + 1, tokens[j].at, names.get(selector) || `样式 ${++ruleNumber}`);
      ruleStart = tokens[j].at + 1;
      i = j;
    }
  };
  const visit = (node: DefaultTreeAdapterMap['node'], visible = false) => {
    if ('tagName' in node) {
      const tag = node.tagName;
      const loc = node.sourceCodeLocation;
      if (
        tag === 'style' &&
        loc?.startTag &&
        loc.endTag &&
        !parsedStyles.has(loc.startTag.endOffset)
      ) {
        parsedStyles.add(loc.startTag.endOffset);
        stylesheet(loc.startTag.endOffset, loc.endTag.startOffset);
      }
      if (['script', 'style', 'head', 'template', 'noscript'].includes(tag)) return;
      if (
        node.attrs.some(
          (attr) =>
            attr.name === 'hidden' ||
            (attr.name === 'aria-hidden' && attr.value === 'true') ||
            (attr.name === 'style' &&
              /(?:display\s*:\s*none|visibility\s*:\s*hidden)/i.test(attr.value))
        )
      )
        return;
      visible ||= tag === 'body';
      const context =
        names.get('#' + node.attrs.find((attr) => attr.name === 'id')?.value) || title(node);
      const attrLoc = loc?.attrs?.style;
      if (attrLoc) {
        const raw = source.slice(attrLoc.startOffset, attrLoc.endOffset);
        const match = /^style\s*=\s*(["'])([\s\S]*)\1$/i.exec(raw);
        if (match)
          declarations(
            attrLoc.endOffset - 1 - match[2].length,
            attrLoc.endOffset - 1,
            context,
            'attribute'
          );
      }
    }
    if (
      visible &&
      node.nodeName === '#text' &&
      'value' in node &&
      node.value.trim() &&
      node.sourceCodeLocation
    ) {
      const { startOffset: start, endOffset: end } = node.sourceCodeLocation;
      const parent = node.parentNode;
      add({
        group: 'text',
        kind: 'text',
        label: parent && 'tagName' in parent ? title(parent) : '文字',
        ...(parent && 'tagName' in parent && authoredLabel(parent)
          ? { labelIsAuthored: true }
          : {}),
        context: `文字 ${++textCount}`,
        value: node.value,
        start,
        end,
        encoding: 'text'
      });
    }
    if ('childNodes' in node) node.childNodes.forEach((child) => visit(child, visible));
  };
  // Styles in the head are parsed independently; head text never becomes editable content.
  const styles = (node: DefaultTreeAdapterMap['node']) => {
    if ('tagName' in node && node.tagName === 'style') {
      visit(node);
      return;
    }
    if ('childNodes' in node) node.childNodes.forEach(styles);
  };
  styles(tree);
  // Avoid parsing body styles twice.
  const body = tree.childNodes
    .flatMap((node) => ('childNodes' in node ? node.childNodes : []))
    .find((node) => 'tagName' in node && node.tagName === 'body');
  if (body) visit(body);
  for (const [key, value] of Object.entries(content.variables || {})) {
    const kind =
      typeof value === 'boolean'
        ? 'boolean'
        : typeof value === 'number'
          ? 'number'
          : !/^(?:title|subtitle|text|content)$/i.test(key) && isHtmlColor(value)
            ? 'color'
            : /font|字体/i.test(key)
              ? 'font'
              : 'text';
    add({
      group: 'variable',
      context: '动画参数',
      label:
        {
          title: '标题',
          subtitle: '副标题',
          text: '文字',
          color: '颜色',
          backgroundColor: '背景颜色',
          textColor: '文字颜色',
          eyebrow: '标签',
          footer: '页脚',
          chapter: '章节',
          pointOne: '要点一',
          pointTwo: '要点二',
          pointThree: '要点三',
          metricValue: '指标数值',
          metricLabel: '指标说明',
          statOne: '副指标一',
          statOneLabel: '副指标说明一',
          statTwo: '副指标二',
          statTwoLabel: '副指标说明二',
          stageOne: '阶段一',
          stageOneDescription: '阶段一说明',
          stageTwo: '阶段二',
          stageTwoDescription: '阶段二说明',
          stageThree: '阶段三',
          stageThreeDescription: '阶段三说明',
          caption: '装饰说明',
          displayNumber: '展示编号',
          pointOneLabel: '要点标签一',
          pointTwoLabel: '要点标签二',
          pointThreeLabel: '要点标签三',
          metricUnit: '指标单位',
          trend: '趋势说明',
          chartValueOne: '图表数值一',
          chartValueTwo: '图表数值二',
          chartValueThree: '图表数值三',
          chartValueFour: '图表数值四',
          chartLabelOne: '图表标签一',
          chartLabelTwo: '图表标签二',
          chartLabelThree: '图表标签三',
          chartLabelFour: '图表标签四',
          signature: '签名文案',
          stageOneLabel: '阶段标签一',
          stageTwoLabel: '阶段标签二',
          stageThreeLabel: '阶段标签三',
          productName: '产品名称',
          productTag: '产品短句',
          featureOne: '卖点一',
          featureOneDescription: '卖点说明一',
          featureTwo: '卖点二',
          featureTwoDescription: '卖点说明二',
          featureThree: '卖点三',
          featureThreeDescription: '卖点说明三',
          badge: '角标',
          offerLabel: '价格说明',
          price: '活动价格',
          originalPrice: '原价',
          cta: '行动文案',
          finePrint: '活动说明',
          discount: '优惠说明',
          beforeLabel: '对比前标签',
          afterLabel: '对比后标签',
          beforeValue: '对比前数值',
          afterValue: '对比后数值',
          beforeOne: '对比前要点一',
          beforeTwo: '对比前要点二',
          beforeThree: '对比前要点三',
          afterOne: '对比后要点一',
          afterTwo: '对比后要点二',
          afterThree: '对比后要点三',
          result: '结果文案',
          monogram: '人物缩写',
          fontFamily: '字体',
          fontSize: '字号'
        }[key] || key,
      kind,
      value,
      variable: key
    });
  }
  return fields;
}

export function applyHtmlProperties(content: HtmlContent, fields: HtmlProperty[]): HtmlContent {
  const patches: { start: number; end: number; value: string }[] = [];
  const variables = { ...content.variables };
  for (const field of fields) {
    if (field.value === field.original) continue;
    if (
      field.kind === 'number' &&
      (typeof field.value !== 'number' || !Number.isFinite(field.value))
    )
      throw new Error(`${field.label}需要是有效数字。`);
    if (field.kind === 'color' && !isHtmlColor(String(field.value)))
      throw new Error(`${field.label}需要是有效颜色。`);
    if (field.variable !== undefined) {
      variables[field.variable] = field.value;
      continue;
    }
    if (field.start === undefined || field.end === undefined) continue;
    let value = String(field.value) + (field.unit || '');
    if (
      field.kind === 'font' &&
      (/[;{}<>]/.test(value) || (typeof CSS !== 'undefined' && !CSS.supports('font-family', value)))
    )
      throw new Error('请输入有效字体名称。');
    if (field.cssProperty && typeof CSS !== 'undefined' && !CSS.supports(field.cssProperty, value))
      throw new Error(`${field.label}的值不适用于此属性。`);
    if (field.encoding === 'text') value = escapeHtml(value);
    if (field.encoding === 'attribute')
      value = escapeHtml(value).replace(/"/g, '&quot;').replace(/'/g, '&#39;');
    patches.push({ start: field.start, end: field.end, value });
  }
  let html = content.html,
    previous = html.length;
  for (const patch of patches.sort((a, b) => b.start - a.start)) {
    if (patch.end > previous) throw new Error('动画属性范围重叠，请重新选择片段。');
    html = html.slice(0, patch.start) + patch.value + html.slice(patch.end);
    previous = patch.start;
  }
  return { ...content, html, ...(content.variables ? { variables } : {}) };
}
