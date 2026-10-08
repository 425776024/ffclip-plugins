import { createPagxContent } from '../core/pagx.mjs';
import { createKineticPagx } from './kinetic.mjs';
const esc = (value) =>
  String(value)
    .replaceAll('&', '&amp;')
    .replaceAll('"', '&quot;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('\n', '&#10;');
const n = (value) => Number(value.toFixed(4));
const layer = (id, name, x, y, w, h, body, extra = '') =>
  `<Layer id="${id}" name="${esc(name)}" ${x ? `left="${x}"` : ''} ${y ? `top="${y}"` : ''} width="${w}" height="${h}" ${extra}>${body}</Layer>`;
const stops = (colors) =>
  colors
    .map((color, i) => `<ColorStop offset="${n(i / (colors.length - 1))}" color="${color}"/>`)
    .join('');
const gradient = (colors, start = '0,0', end = '1,1') =>
  `<LinearGradient startPoint="${start}" endPoint="${end}">${stops(colors)}</LinearGradient>`;
const radial = (colors) => `<RadialGradient>${stops(colors)}</RadialGradient>`;
const fill = (color) =>
  color.startsWith('<') ? `<Fill>${color}</Fill>` : `<Fill color="${color}"/>`;
const shadow = (alpha = '40', blur = 32, y = 18) =>
  `<DropShadowStyle offsetY="${y}" blurX="${blur}" blurY="${blur}" color="#000000${alpha}"/>`;
const rect = (id, name, x, y, w, h, color, r = 0, border = '', effect = '') =>
  layer(
    id,
    name,
    x,
    y,
    w,
    h,
    `<Rectangle width="${w}" height="${h}" roundness="${r}"/>${fill(color)}${border ? `<Stroke color="${border}" width="1.5"/>` : ''}${effect}`
  );
// Text remains real editable text. Avoid faux-bold outline seams in the pinned WASM.
const text = (id, name, copy, x, y, w, size, color = '#ffffff', emphasis = false) =>
  layer(
    id,
    name,
    x,
    y,
    w,
    size * 1.35,
    `<TextBox width="${w}" height="${size * 1.35}" wordWrap="false"><Text text="${esc(copy)}" fontFamily="system" fontSize="${size}" fauxBold="false" letterSpacing="${emphasis ? -0.5 : 0}"/>${fill(color)}</TextBox>`
  );
const label = (id, name, copy, x, y, w, color, size = 22) =>
  text(id, name, copy, x, y, w, size, color);
const line = (id, x, y, w, color, thickness = 2) => rect(id, '装饰线', x, y, w, thickness, color);
const ellipse = (id, x, y, w, h, color, border = '', stroke = 1.5) =>
  layer(
    id,
    '圆形',
    x,
    y,
    w,
    h,
    `<Ellipse width="${w}" height="${h}"/>${fill(color)}${border ? `<Stroke color="${border}" width="${stroke}"/>` : ''}`
  );
const path = (id, name, x, y, w, h, d, color, stroke = 0) =>
  layer(
    id,
    name,
    x,
    y,
    w,
    h,
    `<Path data="${d}"/>${stroke ? `<Stroke color="${color}" width="${stroke}" cap="round" join="round"/>` : fill(color)}`
  );
const arrow = (id, x, y, color, size = 40) =>
  path(
    id,
    '箭头',
    x,
    y,
    size,
    size,
    `M 2 ${size / 2} L ${size - 3} ${size / 2} M ${size * 0.56} ${size * 0.16} L ${size - 3} ${size / 2} L ${size * 0.56} ${size * 0.84}`,
    color,
    3
  );
const channel = (name, keys, type = 'float') =>
  `<Channel name="${name}" type="${type}">${keys.map(([t, v]) => `<Key time="${t}" value="${v}"/>`).join('')}</Channel>`;
const object = (target, ...channels) => `<Object target="${target}">${channels.join('')}</Object>`;
const rotate = (w, h, degrees) => {
  const angle = (degrees * Math.PI) / 180,
    c = Math.cos(angle),
    s = Math.sin(angle);
  // Layer.matrix is additional to its layout position; unlike x/y channels.
  return [c, s, -s, c, w / 2 - (c * w) / 2 + (s * h) / 2, h / 2 - (s * w) / 2 - (c * h) / 2]
    .map(n)
    .join(',');
};
const easeKeys = (delay, span, from, to) => [
  [0, from],
  ...(delay ? [[delay, from]] : []),
  ...[0.2, 0.4, 0.6, 0.8, 1].map((t) => [
    Math.round(delay + span * t),
    n(from + (to - from) * (1 - (1 - t) ** 3))
  ]),
  [240, to]
];
const definitions = [
  ['prism-launch', '出格开场', 'Make noise', 'presentation', '#6635ff', '#ddff00'],
  ['swiss-story', '大字切片', 'Type cuts', 'presentation', '#ff5029', '#151513'],
  ['aurora-data', '冲刺数字', 'Momentum', 'presentation', '#d7ff00', '#101914'],
  ['velvet-keynote', '暮色演讲', 'Velvet keynote', 'presentation', '#302030', '#e8b78d'],
  ['blueprint-roadmap', '蓝图路线', 'Blueprint roadmap', 'presentation', '#ecf0f4', '#3868ea'],
  ['hologram-product', '节拍上新', 'Fresh frequency', 'presentation', '#1746ff', '#ff98d8'],
  ['kinetic-offer', '动感促销', 'Kinetic offer', 'presentation', '#202748', '#eaff87'],
  ['split-comparison', '前后对比', 'Before and after', 'presentation', '#121f27', '#9fe4c4'],
  ['lower-third', '人物介绍', 'Lower third', 'overlay', '#132c35', '#96e4d2'],
  ['tick-title', '呼吸标题', 'Breathing title', 'overlay', '#1b2537', '#e5c397']
];
export const PAGX_TEMPLATES = definitions.map(
  ([id, name, nameEn, category, background, accent]) => ({
    id,
    name,
    nameEn,
    category,
    background,
    accent,
    width: 1920,
    height: 1080,
    duration: 960000,
    transparent: category === 'overlay'
  })
);

/** Art-directed vector templates: native text, gradients, geometry and absolute-time animation. */
export function createPagxTemplate(id, { locale = 'zh' } = {}) {
  const meta = PAGX_TEMPLATES.find((t) => t.id === id);
  if (!meta) throw Error('PAGX 模板不存在');
  if (!['zh', 'en'].includes(locale)) throw Error('模板语言必须为 zh 或 en');
  const kinetic = createKineticPagx(id, locale);
  if (kinetic)
    return {
      id,
      name: locale === 'en' ? meta.nameEn : meta.name,
      pagx: createPagxContent(kinetic, { transparent: false })
    };
  const en = locale === 'en',
    copy = (zh, eng) => (en ? eng : zh);
  let background = '',
    body = '';
  const moving = [];
  const enter = (target, x, y, delay = 0, dy = 30, dx = 0) =>
    moving.push(
      object(
        target,
        channel('alpha', easeKeys(delay, 26, 0, 1)),
        ...(dx ? [channel('x', easeKeys(delay, 32, x + dx, x))] : []),
        ...(dy ? [channel('y', easeKeys(delay, 32, y + dy, y))] : [])
      )
    );
  const float = (target, y, amount = 12) =>
    moving.push(
      object(
        target,
        channel(
          'y',
          Array.from({ length: 17 }, (_, i) => [
            i * 15,
            n(y + Math.sin((i * Math.PI) / 8) * amount)
          ])
        )
      )
    );
  const turn = (target, w, h, from, to) =>
    moving.push(
      object(
        target,
        channel(
          'matrix',
          Array.from({ length: 17 }, (_, i) => [
            i * 15,
            rotate(w, h, from + ((to - from) * (1 - Math.cos((i * Math.PI) / 8))) / 2)
          ]),
          'matrix'
        )
      )
    );
  const backdrop = (color) => {
    background += rect('background', '背景', 0, 0, 1920, 1080, color);
  };
  const header = (kicker, color, ruleColor) =>
    label('brand', '品牌', 'VIDEOCUT / STUDIO', 104, 66, 760, color, 21) +
    label('eyebrow', '栏目', kicker, 1320, 66, 496, color, 21) +
    line('header-rule', 104, 120, 1712, ruleColor, 1);
  const footer = (caption, color, ruleColor) =>
    line('footer-rule', 104, 951, 1712, ruleColor, 1) +
    label('footer', '页脚', caption, 104, 982, 1380, color, 20) +
    label(
      'number',
      '序号',
      `${String(definitions.findIndex((d) => d[0] === id) + 1).padStart(2, '0')} / 10`,
      1710,
      982,
      130,
      color,
      20
    );
  const tag = (target, caption, x, y, w, color, ink) =>
    layer(
      target,
      '标签',
      x,
      y,
      w,
      48,
      rect(target + '-fill', '标签底色', 0, 0, w, 48, color, 24) +
        label(target + '-text', '标签文字', caption, 20, 10, w - 40, ink, 19).replace(
          '<TextBox ',
          '<TextBox textAlign="center" '
        )
    );
  switch (id) {
    case 'velvet-keynote': {
      backdrop(gradient(['#2a1b2c', '#523137', '#281c2b'], '0,0', '1,1'));
      background += ellipse(
        'sun-glow',
        953,
        102,
        840,
        806,
        radial(['#cb785835', '#6d453219', '#331e2b00'])
      );
      body += header(
        copy('灵感盛会 / 年度演讲', 'CREATIVE SUMMIT / KEYNOTE'),
        '#d8b597',
        '#d8b59730'
      );
      body += label('event-date', '日期', 'OCT 24 — 2026', 110, 207, 920, '#e7be9c', 25);
      body +=
        text(
          'headline',
          '主标题',
          copy('不止于此，', 'Beyond the'),
          103,
          324,
          1080,
          118,
          '#f5e9d9',
          true
        ) +
        text(
          'headline2',
          '副标题',
          copy('始于想象。', 'ordinary.'),
          103,
          465,
          1080,
          118,
          '#e7b895',
          true
        );
      body += label(
        'description',
        '说明',
        copy('让每一次表达，都拥有自己的光芒。', 'A gathering of minds. A world of possibilities.'),
        112,
        655,
        1020,
        '#c2a8a0',
        28
      );
      body +=
        rect('event-rule', '日期分隔', 112, 757, 62, 3, '#d5a785') +
        label(
          'event-location',
          '地点',
          copy('上海 · 创意艺术中心', 'SHANGHAI / ARTS & IDEAS'),
          201,
          741,
          860,
          '#e1c6b0',
          23
        );
      let sculpture = '';
      for (let i = 0; i < 15; i++) {
        const inset = i * 12;
        sculpture += ellipse(
          'halo' + i,
          54 + inset,
          14 + inset,
          616 - inset * 2,
          704 - inset * 2,
          '#00000000',
          i % 3 === 0 ? '#d4a779b8' : '#b8856948',
          i % 3 === 0 ? 2.5 : 1
        );
      }
      sculpture += ellipse(
        'eclipse',
        180,
        190,
        420,
        420,
        gradient(['#dbad88', '#95664f', '#412b35'], '0,0', '1,1')
      );
      sculpture += ellipse('eclipse-cut', 249, 150, 420, 420, '#392431');
      sculpture += ellipse('orb', 42, 382, 47, 47, radial(['#fff4d5', '#dbad76', '#79533c']));
      body += layer('sculpture', '天体装置', 1110, 151, 760, 780, sculpture);
      turn('sculpture', 760, 780, -8, 7);
      enter('sculpture', 1110, 151, 4, 40);
      body += footer(
        copy('在想象的边界，遇见新的可能。', 'AT THE EDGE OF IMAGINATION, POSSIBILITY BEGINS.'),
        '#b89a8d',
        '#d8b59730'
      );
      enter('headline', 103, 324, 0, 32);
      enter('headline2', 103, 465, 7, 32);
      enter('description', 112, 655, 17, 20);
      break;
    }
    case 'blueprint-roadmap': {
      backdrop('#edf1f5');
      for (let i = 0; i < 18; i++)
        background += line('paper-grid-x' + i, 104 + i * 100, 170, 1, '#5275920b', 710);
      for (let i = 0; i < 8; i++)
        background += line('paper-grid-y' + i, 104, 176 + i * 100, 1712, '#5275920b', 1);
      body += header(
        copy('行动蓝图 / 从 0 到 1', 'THE ROADMAP / FROM ZERO TO ONE'),
        '#55708b',
        '#47647d2c'
      );
      body += text(
        'headline',
        '主标题',
        copy('每一步，都更接近。', 'Make the next move.'),
        102,
        213,
        1680,
        en ? 98 : 110,
        '#1c3551',
        true
      );
      body += label(
        'description',
        '说明',
        copy('把宏大的想法，变成清晰的下一步。', 'Turn a bold ambition into a clear next step.'),
        112,
        361,
        1510,
        '#6a7d91',
        29
      );
      const titles = [
        copy('发现方向', 'Discover'),
        copy('构建答案', 'Build'),
        copy('让它发生', 'Launch')
      ];
      const notes = [
        copy('洞察需求，找到值得做的事。', 'Find what is worth making.'),
        copy('连接创意与可行的解决方案。', 'Bring the right ideas together.'),
        copy('带着作品，走向更大的世界。', 'Put your work into the world.')
      ];
      for (let i = 0; i < 3; i++) {
        const x = 112 + i * 580,
          main = i === 1,
          ink = main ? '#f3f6ff' : '#244260',
          muted = main ? '#cedbfa' : '#778c9e';
        let card = rect(
          'card-fill' + i,
          '卡片底色',
          0,
          0,
          536,
          327,
          main ? gradient(['#4276e8', '#294bac']) : '#ffffff',
          24,
          main ? '#789aed' : '#cfdbe6',
          shadow('13', 22, 12)
        );
        card += ellipse('icon-disc' + i, 34, 31, 64, 64, main ? '#ffffff19' : '#eaf0fd');
        const icons = [
          'M 18 8 A 15 15 0 1 0 18 38 A 15 15 0 1 0 18 8 M 29 34 L 43 48',
          'M 8 13 L 25 4 L 43 13 L 25 23 Z M 8 25 L 25 35 L 43 25 M 8 38 L 25 48 L 43 38',
          'M 10 42 L 40 12 M 13 12 L 40 12 L 40 39'
        ];
        card += path(
          'step-icon' + i,
          '步骤图标',
          44,
          37,
          50,
          54,
          icons[i],
          main ? '#e0ecff' : '#5272be',
          2.5
        );
        card += label(
          'step' + i,
          '步骤编号',
          '0' + (i + 1),
          418,
          38,
          88,
          main ? '#b8cbff' : '#9aaac0',
          33
        );
        card +=
          text('card-title' + i, '步骤标题', titles[i], 36, 120, 480, 51, ink, true) +
          label('card-copy' + i, '步骤说明', notes[i], 38, 215, 466, muted, en ? 24 : 25);
        card += line('card-rule' + i, 38, 286, 459, main ? '#cad9ff35' : '#cbd8e655', 1);
        body += layer('card' + i, '步骤卡片 ' + (i + 1), x, 503, 536, 327, card);
        enter('card' + i, x, 503, 12 + i * 10, 56);
      }
      body += line('journey-line', 142, 889, 1606, '#bdcbdf', 2);
      for (let i = 0; i < 3; i++)
        body += ellipse(
          'journey-dot' + i,
          130 + i * 810,
          878,
          22,
          22,
          i === 1 ? '#4978e4' : '#edf1f5',
          '#7892be',
          2
        );
      body += footer(
        copy('从灵感出发，让进步持续发生。', 'A CLEAR DIRECTION. MEANINGFUL PROGRESS.'),
        '#6e8093',
        '#47647d2c'
      );
      enter('headline', 102, 213, 0, 30);
      enter('description', 112, 361, 9, 20);
      break;
    }
    case 'kinetic-offer': {
      backdrop('#202748');
      background += path(
        'offer-stripe',
        '背景斜带',
        0,
        0,
        1920,
        1080,
        'M 1240 0 L 1920 0 L 1920 1080 L 780 1080 Z',
        '#273158'
      );
      body += header(
        copy('新品限时 / 心动即刻发生', 'FRESH DROPS / GOOD THINGS DON’T WAIT'),
        '#bbc5dc',
        '#abb5d92e'
      );
      body += tag(
        'edition',
        copy('新品首发 · 限时礼遇', 'NEW SEASON / LIMITED OFFER'),
        112,
        211,
        en ? 414 : 310,
        '#eaff87',
        '#29314c'
      );
      body +=
        text(
          'headline',
          '主标题',
          copy('好物上新，', 'Fresh drops.'),
          100,
          319,
          1130,
          en ? 125 : 132,
          '#f2f1df',
          true
        ) +
        text(
          'headline2',
          '副标题',
          copy('心动趁现在。', 'Good timing.'),
          100,
          477,
          1110,
          en ? 115 : 125,
          '#e8ff99',
          true
        );
      body += label(
        'description',
        '说明',
        copy('把喜欢带回家，把日常过成期待。', 'Bring a little more good into your everyday.'),
        111,
        662,
        1000,
        '#b3bed2',
        29
      );
      body += layer(
        'cta',
        '行动按钮',
        112,
        760,
        410,
        88,
        rect('cta-fill', '按钮底色', 0, 0, 410, 88, '#eaff87', 44) +
          label(
            'cta-label',
            '按钮文字',
            copy('即刻探索', 'SHOP THE COLLECTION'),
            32,
            en ? 26 : 23,
            342,
            '#25334a',
            en ? 21 : 29
          ) +
          arrow('cta-arrow', 343, 26, '#25334a', 35)
      );
      let ticket = '';
      ticket += rect(
        'offer-fill',
        '优惠卡底色',
        0,
        0,
        492,
        580,
        gradient(['#f5e8c5', '#e9d3a4'], '0,0', '1,1'),
        22,
        '#fcf0d1',
        shadow('38', 24, 18)
      );
      ticket += label(
        'offer-kicker',
        '优惠说明',
        copy('新品尝鲜价', 'THE LAUNCH EDITION'),
        40,
        35,
        420,
        '#84724d',
        24
      );
      ticket += line('offer-rule', 40, 90, 412, '#88764d42', 1);
      ticket +=
        label('currency', '币种', 'CNY', 44, 126, 300, '#82714f', 22) +
        text('price', '价格', '199', 29, 170, 458, 176, '#293b39', true);
      for (let i = 0; i < 20; i++)
        ticket += line('perforation' + i, 38 + i * 22, 389, 10, '#84724d58', 2);
      ticket +=
        ellipse('ticket-notch-left', 0, 376, 28, 28, '#263052') +
        ellipse('ticket-notch-right', 464, 376, 28, 28, '#273158');
      ticket += label(
        'offer-copy',
        '促销说明',
        copy('限时惊喜 · 现在入场', 'YOUR NEXT GOOD THING'),
        42,
        423,
        416,
        '#746445',
        23
      );
      ticket +=
        label('offer-foot', '卡片页脚', 'MADE FOR THE MOMENT', 42, 497, 360, '#9b875e', 18) +
        arrow('offer-arrow', 412, 491, '#475642', 35);
      body += layer(
        'ticket-reveal',
        '优惠卡片',
        1245,
        266,
        544,
        626,
        rect('offer-shadow-card', '优惠卡背面', 0, 23, 492, 580, '#638584', 22) +
          layer('ticket-turn', '优惠卡动态', 24, 0, 492, 580, ticket)
      );
      turn('ticket-turn', 492, 580, 6, -4);
      enter('ticket-reveal', 1245, 266, 5, 65, 40);
      body += footer(
        copy('遇见喜欢的生活 / 价格为示例。', 'FIND YOUR EVERYDAY FAVORITES / ILLUSTRATIVE OFFER.'),
        '#94a1bc',
        '#abb5d92e'
      );
      enter('headline', 100, 319, 0, 35);
      enter('headline2', 100, 477, 7, 35);
      enter('description', 111, 662, 15, 22);
      enter('cta', 112, 760, 23, 15);
      break;
    }
    case 'split-comparison': {
      backdrop('#121f27');
      background += rect('light-half', '明亮背景', 974, 0, 946, 1080, '#e6eee4');
      body +=
        label('brand', '品牌', 'VIDEOCUT / STUDIO', 104, 66, 700, '#9db7bb', 21) +
        label(
          'eyebrow',
          '栏目',
          copy('改变，就在此刻', 'A BETTER WAY FORWARD'),
          1204,
          66,
          600,
          '#638574',
          21
        );
      body +=
        line('header-rule-left', 104, 120, 790, '#cbe0da29', 1) +
        line('header-rule-right', 1053, 120, 760, '#385e4838', 1);
      body +=
        text(
          'headline',
          '主标题',
          copy('把时间，', 'Less friction.'),
          101,
          211,
          810,
          en ? 91 : 105,
          '#eff0e7',
          true
        ) +
        text(
          'headline2',
          '副标题',
          copy('留给创作。', 'More creation.'),
          1045,
          211,
          820,
          en ? 91 : 105,
          '#294d3f',
          true
        );
      body +=
        label(
          'before',
          '左侧标签',
          copy('过去 / 重复与等待', 'BEFORE / BUSYWORK'),
          111,
          421,
          720,
          '#809da5',
          23
        ) +
        label(
          'after',
          '右侧标签',
          copy('现在 / 专注与创造', 'AFTER / CREATIVE FLOW'),
          1054,
          421,
          730,
          '#729281',
          23
        );
      body +=
        text('before-value', '原先数值', '30', 97, 482, 580, 212, '#b0c1c5', true) +
        label('before-unit', '原先单位', copy('分钟', 'MINUTES'), 393, 632, 400, '#839ea6', 29);
      body +=
        text('after-value', '现在数值', '3', 1042, 482, 390, 212, '#315f49', true) +
        label('after-unit', '现在单位', copy('分钟', 'MINUTES'), 1213, 632, 430, '#729281', 29);
      const beforeItems = [
        copy('反复切换工具', 'Switching between tools'),
        copy('重复调整细节', 'Repeating the small steps'),
        copy('等待漫长渲染', 'Waiting for the render')
      ];
      const afterItems = [
        copy('创意与素材，一处连接', 'Ideas and media, together'),
        copy('轻松调整，自由掌控', 'Easy to refine. Yours to shape.'),
        copy('从灵感，更快抵达作品', 'From inspiration to finished work')
      ];
      for (let i = 0; i < 3; i++) {
        body +=
          line('before-dot' + i, 115, 782 + i * 47, 16, '#6f8992', 2) +
          label(
            'before-copy' + i,
            '原先流程',
            beforeItems[i],
            155,
            766 + i * 47,
            710,
            '#8ca6ae',
            25
          );
        body +=
          path(
            'after-check' + i,
            '对勾',
            1060,
            775 + i * 47,
            28,
            23,
            'M 0 11 L 8 19 L 25 2',
            '#4d8063',
            2.5
          ) +
          label(
            'after-copy' + i,
            '现在流程',
            afterItems[i],
            1107,
            766 + i * 47,
            688,
            '#5e806d',
            25
          );
      }
      body += layer(
        'bridge',
        '对比桥梁',
        902,
        507,
        144,
        144,
        ellipse('bridge-disc', 0, 0, 144, 144, '#a7d8b4', '#ccedce', 2) +
          arrow('bridge-arrow', 41, 40, '#315f49', 64),
        ''
      );
      float('bridge', 507, 9);
      body +=
        line('footer-left', 104, 951, 790, '#cbe0da29', 1) +
        line('footer-right', 1053, 951, 760, '#385e4838', 1) +
        label(
          'footer',
          '页脚',
          copy('流程示意 / 用变化呈现价值', 'ILLUSTRATIVE WORKFLOW / THE VALUE OF CHANGE'),
          109,
          982,
          1490,
          '#819b9b',
          19
        );
      enter('headline', 101, 211, 0, 26);
      enter('headline2', 1045, 211, 9, 26);
      enter('before-value', 97, 482, 8, 30);
      enter('after-value', 1042, 482, 20, 30);
      break;
    }
    case 'lower-third': {
      let plate = rect(
        'plate',
        '介绍卡片',
        0,
        0,
        1040,
        238,
        gradient(['#173640f2', '#172a38ee'], '0,0', '1,1'),
        20,
        '#bcebe037',
        shadow('28', 22, 13)
      );
      plate += rect(
        'accent',
        '强调条',
        0,
        26,
        5,
        184,
        gradient(['#9decd4', '#6fadc8'], '0,0', '0,1'),
        2
      );
      plate += ellipse(
        'portrait-disc',
        37,
        40,
        150,
        150,
        gradient(['#437778', '#244958'], '0,0', '1,1'),
        '#b2dbd174',
        1
      );
      plate += path(
        'monogram',
        '人物标记',
        82,
        86,
        62,
        70,
        'M 4 51 L 25 7 L 36 29 L 48 7 L 59 51 M 14 35 L 48 35',
        '#d5f3e5',
        3
      );
      plate += label(
        'eyebrow',
        '栏目',
        copy('人物 / 创作背后', 'PEOPLE / BEHIND THE STORY'),
        223,
        27,
        767,
        '#93c3bd',
        19
      );
      plate += text(
        'headline',
        '姓名',
        copy('林川', 'Alex Chen'),
        217,
        55,
        770,
        64,
        '#f1f3e8',
        true
      );
      plate += label(
        'description',
        '介绍',
        copy('独立创作者 · 用镜头记录日常', 'Independent filmmaker / Stories from everyday life'),
        224,
        174,
        760,
        '#b9cdd0',
        en ? 25 : 28
      );
      plate += line('identity-rule', 225, 158, 747, '#b9dbd22e', 1);
      body += layer('identity', '人物介绍', 112, 721, 1040, 238, plate);
      body += layer(
        'identity-top',
        '身份标记',
        148,
        680,
        282,
        41,
        rect('identity-tag-fill', '标记底色', 0, 0, 282, 41, '#96dfc9', 8) +
          label(
            'identity-tag',
            '标记文字',
            'THE CREATOR SERIES',
            17,
            8,
            248,
            '#23434a',
            17
          ).replace('<TextBox ', '<TextBox textAlign="center" ')
      );
      enter('identity', 112, 721, 0, 0, -74);
      enter('identity-top', 148, 680, 10, 15);
      enter('headline', 217, 55, 10, 14);
      enter('description', 224, 174, 17, 12);
      break;
    }
    case 'tick-title': {
      let frame = rect(
        'title-panel',
        '标题底板',
        0,
        0,
        1416,
        510,
        gradient(['#162838d9', '#182131bb'], '0,0', '1,1'),
        28,
        '#e8d1ac40',
        shadow('22', 28, 12)
      );
      // Open corner brackets retain contrast without competing with the typography.
      const corners = [
        ['tl', 32, 32, 'M 0 68 L 0 0 L 92 0'],
        ['tr', 1292, 32, 'M 0 0 L 92 0 L 92 68'],
        ['bl', 32, 410, 'M 0 0 L 0 68 L 92 68'],
        ['br', 1292, 410, 'M 0 68 L 92 68 L 92 0']
      ];
      for (const [suffix, x, y, d] of corners)
        frame += path('corner-' + suffix, '边角', x, y, 94, 70, d, '#e6c895', 2);
      frame += label('eyebrow', '栏目', 'STORIES WORTH TELLING', 119, 72, 1200, '#c2b796', 24);
      frame += text(
        'headline',
        '主标题',
        copy('此刻，让故事发生。', 'Make this moment matter.'),
        111,
        151,
        1270,
        en ? 88 : 103,
        '#f8f1df',
        true
      );
      frame +=
        line('title-rule', 124, 322, 106, '#e1c493', 3) +
        label(
          'description',
          '副标题',
          copy('让每一个平凡瞬间，都值得被看见', 'Find the extraordinary in the everyday.'),
          261,
          303,
          1050,
          '#bccbd1',
          en ? 30 : 32
        );
      frame += label(
        'footer',
        '小标题',
        copy('由你定义，下一个精彩。', 'YOUR NEXT STORY STARTS HERE.'),
        126,
        406,
        1130,
        '#8caaa9',
        20
      );
      body += layer('title-frame', '标题画框', 252, 280, 1416, 510, frame);
      enter('title-frame', 252, 280, 0, 28);
      enter('headline', 111, 151, 9, 22);
      enter('description', 261, 303, 20, 15);
      moving.push(
        object(
          'title-frame',
          channel(
            'matrix',
            [
              [0, '0.97,0,0,0.97,21.24,7.65'],
              [45, '1,0,0,1,0,0'],
              [120, '1.006,0,0,1.006,-4.248,-1.53'],
              [216, '1,0,0,1,0,0'],
              [240, '1,0,0,1,0,0']
            ],
            'matrix'
          )
        )
      );
      break;
    }
  }
  const anim =
    object(
      'stage',
      channel('alpha', [
        [0, 0],
        [6, 1],
        [219, 1],
        [239, 0]
      ])
    ) + moving.join('');
  return {
    id,
    name: en ? meta.nameEn : meta.name,
    pagx: createPagxContent(
      `<pagx width="1920" height="1080">${background}${layer('stage', '画面', 0, 0, 1920, 1080, body)}<Animations><Animation id="main" frameRate="30" duration="240" loop="once">${anim}</Animation></Animations></pagx>`.replaceAll(
        '><',
        '>\n<'
      ),
      { transparent: meta.transparent }
    )
  };
}

/** Companion to the packaged 18-second narrated example; titles/media remain separate editable tracks. */
export function createStarterPagx(copy) {
  const en = copy.locale === 'en',
    accent = '#a4e0d2',
    ink = '#f7f2e8';
  let body =
    rect('shade', '背景遮罩', 0, 0, 1280, 720, '#07171bb8') +
    text('brand', '品牌', 'ffclip', 64, 48, 600, 31, ink, true) +
    text('edition', '栏目', copy.edition, 760, 59, 470, 14, accent) +
    line('rule', 64, 101, 1152, '#ffffff35', 1);
  let animation = '';
  for (let i = 0; i < 3; i++) {
    const id = 'chapter' + i,
      offset = i * 180;
    const scene =
      text('chapter-label' + i, '章节', copy['chapter' + (i + 1)], 64, 176, 1100, 19, accent) +
      text(
        'chapter-title' + i,
        '主标题',
        copy['headline' + (i + 1)],
        64,
        226,
        1150,
        en ? 76 : 82,
        ink,
        true
      ) +
      text(
        'chapter-description' + i,
        '说明',
        copy['description' + (i + 1)],
        64,
        528,
        1130,
        en ? 24 : 25,
        '#d5e1e1'
      );
    body += layer(id, '章节 ' + (i + 1), 0, 0, 1280, 720, scene, 'alpha="0"');
    animation += object(
      id,
      channel('alpha', [
        [0, 0],
        ...(offset ? [[offset, 0]] : []),
        [offset + 18, 1],
        [offset + 162, 1],
        [offset + 179, 0],
        [540, 0]
      ]),
      channel('y', [[0, 20], ...(offset ? [[offset, 20]] : []), [offset + 24, 0], [540, 0]])
    );
  }
  body += text('footer', '页脚', copy.footer, 64, 674, 1160, 15, '#b8cbd0');
  return createPagxContent(
    `<pagx width="1280" height="720">${body}<Animations><Animation id="main" frameRate="30" duration="540" loop="once">${animation}</Animation></Animations></pagx>`,
    { transparent: true }
  );
}
