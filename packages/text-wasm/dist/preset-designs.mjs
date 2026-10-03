// Redesigned appearances inherit the original template topology and layout.
// Reference artwork is never copied into the shipped resource closure.
const flower = (id, name, text, fill, outline, depth, extra = {}) => ({
  id,
  name,
  text,
  category: 'flower',
  tag: '多层材质 · 可编辑文字',
  fill,
  outline,
  depth,
  ...extra
});
const bubble = (id, name, text, shape, paper, edge, ink, accent) => ({
  id,
  name,
  text,
  category: 'bubble',
  tag: '自适应底板 · 可编辑文字',
  shape,
  paper,
  edge,
  fill: [ink],
  accent
});
const motion = (id, name, text, animation, fill, extra = {}) => ({
  id,
  name,
  text,
  category: 'animation',
  tag: '逐字动画 · 可编辑文字',
  animation,
  fill,
  outline: '#14283f',
  depth: '#14283f',
  ...extra
});
export const presetDesigns = [
  flower(
    'layered-flower',
    '蜜桃灯牌',
    '心动时刻',
    ['#fff2c6', '#ffab9e', '#ed6285'],
    '#fff7e9',
    '#893d67',
    { base: 'flower-style-03', stroke: 16, shadowX: 9, shadowY: 13 }
  ),
  flower(
    'pattern-flower',
    '薄荷拼贴',
    '灵感上线',
    ['#edfff3', '#6cdec3', '#2799a1'],
    '#153c53',
    '#efce73',
    { base: 'flower-style-38', stroke: 10, shadowX: -9, shadowY: 11 }
  ),
  flower(
    'flower-sunset',
    '热力火焰',
    '热爱生活',
    ['#ffec85', '#ffac61', '#ee654d'],
    '#512e47',
    '#fbd4a2',
    { stroke: 12, shadowX: 11, shadowY: 10 }
  ),
  flower(
    'flower-lilac',
    '紫雾软糖',
    '慢慢发光',
    ['#fff1ff', '#ceaafa', '#8a69cf'],
    '#f7edff',
    '#5e447d',
    { stroke: 18, shadowX: 4, shadowY: 14, glow: '#ac82e0' }
  ),
  flower(
    'flower-electric',
    '电波蓝',
    '即刻出发',
    ['#a8faff', '#5cbeed', '#6389df'],
    '#10294c',
    '#ef80af',
    { stroke: 10, shadowX: -13, shadowY: 4 }
  ),
  flower(
    'flower-gold',
    '香槟金箔',
    '高光时刻',
    ['#fff5d0', '#c89c56', '#f7dfa0', '#a97938'],
    '#533f33',
    '#302f38',
    { stroke: 5, shadowX: 7, shadowY: 9 }
  ),
  flower('flower-paper', '奶油纸刻', '好事发生', ['#794333'], '#fff6df', '#bc7959', {
    stroke: 5,
    shadowX: 9,
    shadowY: 15
  }),
  flower('flower-pop', '漫画爆点', '快乐加倍', ['#fff176', '#ffcc44'], '#3a2549', '#f26b8b', {
    stroke: 14,
    shadowX: 14,
    shadowY: 8
  }),
  flower('flower-ink', '墨色留白', '自在生长', ['#263d42'], '#f5ecda', '#93b7a5', {
    stroke: 12,
    shadowX: -6,
    shadowY: 9
  }),
  flower(
    'flower-frost',
    '冰川银',
    '保持热爱',
    ['#ffffff', '#afd1de', '#f5fcff', '#87a6bf'],
    '#304967',
    '#647b9d',
    { stroke: 7, shadowX: 5, shadowY: 12 }
  ),
  bubble('bubble-mint', '薄荷对话', '你好呀', 'speech', '#dcf7e9', '#285b56', '#204944', '#68c6a1'),
  bubble('bubble-cloud', '云朵心语', '想你啦', 'cloud', '#faf4ff', '#8b79a6', '#67567c', '#c8b3df'),
  bubble('bubble-signal', '电台讯号', '收到', 'signal', '#26394d', '#7cd1ca', '#effffc', '#edb963'),
  bubble('bubble-note', '杏色便签', '记得开心', 'note', '#fff0ce', '#b07949', '#78523a', '#e5b77c'),
  bubble(
    'bubble-ticket',
    '今日票券',
    '一起出发',
    'ticket',
    '#e5edf9',
    '#536f9b',
    '#354f7b',
    '#92a9cf'
  ),
  bubble(
    'bubble-bracket',
    '括号旁白',
    '小声说',
    'bracket',
    '#f7ecdf',
    '#986750',
    '#704f43',
    '#cd9d7f'
  ),
  bubble(
    'bubble-capsule',
    '桃桃胶囊',
    '赞一个',
    'pill',
    '#fbe0e4',
    '#af5c77',
    '#85485e',
    '#e59cad'
  ),
  bubble(
    'bubble-pixel',
    '像素消息',
    '能量满格',
    'pixel',
    '#e4f0c8',
    '#537149',
    '#405735',
    '#9abd6d'
  ),
  bubble(
    'bubble-frame',
    '蓝图标注',
    '重点来了',
    'frame',
    '#e8f7fc',
    '#3c788e',
    '#305f70',
    '#83bfcd'
  ),
  bubble(
    'bubble-ribbon',
    '莓红缎带',
    '好消息',
    'ribbon',
    '#96445f',
    '#f7c9ba',
    '#fff4df',
    '#c47888'
  ),
  motion('studio-glow', '柔光浮现', '温柔登场', 'rise', ['#fff4dc', '#edca86'], {
    base: 'anim-studio-time-transform-softglow',
    glow: '#e7b56c'
  }),
  motion('studio-radial', '聚拢成句', '灵感汇聚', 'gather', ['#e6fbff', '#7ed1dc'], {
    base: 'anim-studio-dual-selector-radial'
  }),
  motion('cube', '折页翻入', '翻开新篇', 'flip', ['#efe7ff', '#b49bdc'], { base: 'anim-lua-cube' }),
  motion('printer', '逐字轻打', '故事开始', 'type', ['#eef9e8'], {
    base: 'anim-lua-printer-multistage'
  }),
  motion('anim-bounce', '弹跳接力', '快乐登场', 'bounce', ['#fff2ac', '#f9bd60']),
  motion('anim-slide', '侧滑入场', '即刻开启', 'slide', ['#ffe6dc', '#f3a595']),
  motion('anim-spring', '缩放回弹', '惊喜来了', 'spring', ['#dcfff3', '#89d8bb']),
  motion('anim-wave', '轻波律动', '随心而动', 'wave', ['#e8f4ff', '#8fbbeb']),
  motion('anim-breathe', '微光呼吸', '慢慢呼吸', 'breathe', ['#f9eaff', '#d5a5da'], {
    glow: '#ba83c5'
  }),
  motion('anim-swing', '摇摆问候', '嗨你好呀', 'swing', ['#fff6e1', '#e7c78f'])
];
// Each design names the structure it inherits. Geometry, layer identities and
// resource types are copied without flattening them into plain text styles.
for (const [i, design] of presetDesigns.entries()) {
  design.source =
    design.base ||
    (design.category === 'flower'
      ? ['03', '38', '29', '24', '12', '14', '04', '10', '05', '33'].map(
          (style) => `flower-style-${style}`
        )[i]
      : design.category === 'bubble'
        ? i % 2
          ? 'bubble-tile'
          : 'bubble-nine-slice'
        : 'anim-lua-opacity');
  if (design.category === 'flower' && design.base) {
    design.backdrop = design.source === 'flower-style-38' ? 'bubble-tile' : 'bubble-nine-slice';
  }
  // The original layouts use large display type. Keep their two-character
  // fixture convention while leaving the inserted content fully editable.
  design.text = [
    '心动',
    '灵感',
    '热爱',
    '柔光',
    '出发',
    '高光',
    '好事',
    '快乐',
    '自在',
    '冰川',
    '你好',
    '想你',
    '收到',
    '记得',
    '一起',
    '小声',
    '点赞',
    '能量',
    '重点',
    '喜报',
    '温柔',
    '灵感',
    '翻开',
    '故事',
    '快乐',
    '开启',
    '惊喜',
    '随心',
    '呼吸',
    '问候'
  ][i];
}
export const authoredRecipes = presetDesigns.map((design) => ({
  id: design.id,
  name: design.name,
  tag: design.tag,
  category: design.category,
  base: design.base || `original-${design.id}`,
  text: design.text,
  ...(design.id === 'layered-flower' || design.id === 'pattern-flower'
    ? { backdrop: design.backdrop, animation: 'anim-lua-letter-transform' }
    : {}),
  ...(design.id.startsWith('studio-') ? { external: true } : {}),
  timeUs:
    design.category === 'flower'
      ? 350000
      : design.id === 'printer'
        ? 900000
        : design.id === 'studio-radial'
          ? 1800000
          : design.category === 'animation'
            ? 1500000
            : 0
}));
