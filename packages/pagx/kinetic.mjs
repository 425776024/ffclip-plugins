// Native, editable kinetic compositions. Each template shares an absolute 30 fps clock.
const e = (s) =>
  String(s).replaceAll('&', '&amp;').replaceAll('"', '&quot;').replaceAll('<', '&lt;');
const n = (v) => +v.toFixed(4);
const L = (id, name, x, y, w, h, body, extra = '') =>
  `<Layer id="${id}" name="${name}" ${x ? `left="${x}"` : ''} ${y ? `top="${y}"` : ''} width="${w}" height="${h}" ${extra}>${body}</Layer>`;
const fill = (color) => `<Fill color="${color}"/>`;
const R = (id, x, y, w, h, color, radius = 0) =>
  L(
    id,
    '色块',
    x,
    y,
    w,
    h,
    `<Rectangle width="${w}" height="${h}" roundness="${radius}"/>${fill(color)}`
  );
const E = (id, x, y, w, h, color, stroke = '', sw = 2) =>
  L(
    id,
    '圆形',
    x,
    y,
    w,
    h,
    `<Ellipse width="${w}" height="${h}"/>${fill(color)}${stroke ? `<Stroke color="${stroke}" width="${sw}" align="inside"/>` : ''}`
  );
const P = (id, name, x, y, w, h, d, color) =>
  L(id, name, x, y, w, h, `<Path data="${d}"/>${fill(color)}`);
const T = (
  id,
  name,
  value,
  x,
  y,
  w,
  size,
  color,
  family = 'PingFang SC',
  style = 'Semibold',
  align = 'start'
) =>
  L(
    id,
    name,
    x,
    y,
    w,
    size * 1.32,
    `<TextBox width="${w}" height="${size * 1.32}" wordWrap="false" ${align === 'start' ? '' : `textAlign="${align}"`}><Text text="${e(value)}" fontFamily="${family}" fontStyle="${style}" fontSize="${size}" letterSpacing="-1"/>${fill(color)}</TextBox>`
  );
const small = (id, name, value, x, y, w, color, size = 28) =>
  T(id, name, value, x, y, w, size, color, 'Arial', 'Bold');
const channel = (name, keys, type = 'float') =>
  `<Channel name="${name}" type="${type}">${keys.map(([t, v]) => `<Key time="${t}" value="${v}"/>`).join('')}</Channel>`;
const matrix = (w, h, scale = 1, degree = 0, tx = 0, ty = 0) => {
  const a = (degree * Math.PI) / 180,
    c = Math.cos(a) * scale,
    s = Math.sin(a) * scale;
  return [
    c,
    s,
    -s,
    c,
    w / 2 - (c * w) / 2 + (s * h) / 2 + tx,
    h / 2 - (s * w) / 2 - (c * h) / 2 + ty
  ]
    .map(n)
    .join(',');
};
const burst = (id, x, y, size, color, points = 12, inner = 0.58) => {
  const radius = size / 2;
  const d =
    Array.from({ length: points * 2 }, (_, i) => {
      const angle = (i * Math.PI) / points - Math.PI / 2,
        r = radius * (i % 2 ? inner : 1);
      return `${i ? 'L' : 'M'} ${n(radius + Math.cos(angle) * r)} ${n(radius + Math.sin(angle) * r)}`;
    }).join(' ') + ' Z';
  return P(id, '星芒', x, y, size, size, d, color);
};
const arrow = (id, x, y, size, color) =>
  P(
    id,
    '箭头',
    x,
    y,
    size,
    size,
    `M 0 ${size * 0.76} L ${size * 0.56} ${size * 0.2} L ${size * 0.12} ${size * 0.2} L ${size * 0.12} 0 L ${size} 0 L ${size} ${size * 0.88} L ${size * 0.8} ${size * 0.88} L ${size * 0.8} ${size * 0.44} L ${size * 0.24} ${size} Z`,
    color
  );

export function createKineticPagx(id, locale) {
  const en = locale === 'en',
    copy = (zh, english) => (en ? english : zh);
  const title = (id, name, value, x, y, w, size, color, align = 'start') =>
    T(
      id,
      name,
      value,
      x,
      y,
      w,
      size,
      color,
      en ? 'Impact' : 'PingFang SC',
      en ? 'Regular' : 'Semibold',
      align
    );
  const motions = [];
  const animate = (id, ...channels) =>
    motions.push(`<Object target="${id}">${channels.join('')}</Object>`);
  const transform = (id, w, h, keys) =>
    animate(
      id,
      channel(
        'matrix',
        keys.map(([t, scale, angle = 0, x = 0, y = 0]) => [t, matrix(w, h, scale, angle, x, y)]),
        'matrix'
      )
    );
  const spin = (id, size, from, to) =>
    transform(
      id,
      size,
      size,
      Array.from({ length: 17 }, (_, i) => [i * 15, 1, from + ((to - from) * i) / 16])
    );
  const slam = (id, w, h, at, dx = 0, dy = 180) =>
    transform(id, w, h, [
      [0, 0.8, 0, dx, dy],
      [at, 0.8, 0, dx, dy],
      [at + 8, 1.045, -1.5, 0, -9],
      [at + 15, 1, 0],
      [240, 1, 0]
    ]);
  const scenes = (opening, hero, ending, color) => {
    // One-frame holds make scene cuts deterministic at any seek position.
    animate(
      'intro',
      channel('alpha', [
        [0, 1],
        [52, 1],
        [53, 0],
        [240, 0]
      ])
    );
    animate(
      'hero',
      channel('alpha', [
        [0, 0],
        [52, 0],
        [53, 1],
        [199, 1],
        [200, 0],
        [240, 0]
      ])
    );
    animate(
      'outro',
      channel('alpha', [
        [0, 0],
        [199, 0],
        [200, 1],
        [240, 1]
      ])
    );
    animate(
      'wipe',
      channel('alpha', [
        [0, 0],
        [42, 0],
        [43, 1],
        [72, 1],
        [73, 0],
        [189, 0],
        [190, 1],
        [219, 1],
        [220, 0],
        [240, 0]
      ])
    );
    let wipe = '';
    for (let i = 0; i < 8; i++) {
      wipe += R(`wipe-${i}`, i * 240, 0, 240, 1080, color);
      animate(
        `wipe-${i}`,
        channel('y', [
          [0, 1080],
          [43 + i, 1080],
          [51 + i, 0],
          [55 + i, 0],
          [63 + i, -1080],
          [190 + i, -1080],
          [198 + i, 0],
          [203 + i, 0],
          [211 + i, 1080],
          [240, 1080]
        ])
      );
    }
    return (
      L('intro', '开场', 0, 0, 1920, 1080, opening) +
      L('hero', '主画面', 0, 0, 1920, 1080, hero, 'alpha="0"') +
      L('outro', '收尾', 0, 0, 1920, 1080, ending, 'alpha="0"') +
      L('wipe', '节奏切换', 0, 0, 1920, 1080, wipe, 'clipToBounds="true" alpha="0"')
    );
  };
  let body;
  switch (id) {
    case 'prism-launch': {
      const violet = '#6635ff',
        lime = '#ddff00',
        pink = '#ff82d4',
        ink = '#17121f',
        cream = '#f7f4e9';
      let intro = R('intro-bg', 0, 0, 1920, 1080, lime);
      for (let i = 0; i < 3; i++) {
        intro += title(
          `intro-word-${i}`,
          '开场文字',
          copy('出格', 'LOUD'),
          80 + (i % 2) * 600,
          20 + i * 322,
          1170,
          270,
          i === 1 ? violet : ink
        );
        slam(`intro-word-${i}`, 1170, 356.4, i * 5, i % 2 ? 1000 : -1000, 0);
      }
      let hero = R('hero-bg', 0, 0, 1920, 1080, violet);
      hero += burst('hero-star', 1110, 65, 600, lime, 12, 0.54);
      hero += E('hero-orbit', 87, 540, 480, 400, pink) + E('hero-hole', 209, 640, 230, 200, violet);
      for (let i = 0; i < 7; i++) hero += R(`orbit-stripe-${i}`, 90, 580 + i * 46, 472, 15, violet);
      hero += title(
        'headline',
        '主标题',
        copy('灵感', 'MAKE'),
        68,
        35,
        1080,
        en ? 340 : 374,
        cream
      );
      hero += title(
        'headline-two',
        '第二行标题',
        copy('出格', 'NOISE'),
        en ? 680 : 827,
        en ? 552 : 488,
        1060,
        en ? 340 : 365,
        cream
      );
      hero += L(
        'hero-ribbon',
        '宣言条幅',
        86,
        444,
        1748,
        116,
        R('ribbon-color', 0, 0, 1748, 116, ink) +
          small(
            'description',
            '宣言',
            copy('打破常规，让灵感大声一点。', 'BREAK THE FRAME. MAKE YOURSELF HEARD.'),
            46,
            35,
            1660,
            lime,
            38
          ),
        `matrix="${matrix(1748, 116, 1, -6)}"`
      );
      hero +=
        small('edition', '角标', 'NEW IDEAS / NO RULES', 90, 983, 820, cream, 27) +
        small('brand', '品牌', 'VIDEOCUT®', 1520, 985, 320, lime, 29);
      slam('headline', 1080, (en ? 340 : 374) * 1.32, 56, -450, -90);
      slam('headline-two', 1060, (en ? 340 : 365) * 1.32, 62, 500, 60);
      spin('hero-star', 600, -18, 90);
      transform('hero-ribbon', 1748, 116, [
        [0, 1, -6],
        [90, 1, -6],
        [109, 1, -3],
        [129, 1, -6],
        [168, 1, -6],
        [181, 1, -8],
        [195, 1, -6],
        [240, 1, -6]
      ]);
      let ending =
        R('outro-bg', 0, 0, 1920, 1080, pink) + burst('outro-star', 680, 70, 860, lime, 12, 0.68);
      ending += title(
        'outro-title',
        '收尾文字',
        copy('就要出格', 'STAY LOUD.'),
        75,
        310,
        1770,
        en ? 295 : 350,
        ink
      );
      ending += small(
        'outro-caption',
        '收尾短句',
        'MAKE SOMETHING IMPOSSIBLE TO IGNORE.',
        96,
        934,
        1690,
        ink,
        31
      );
      slam('outro-title', 1770, (en ? 295 : 350) * 1.32, 208, 0, 260);
      spin('outro-star', 860, 0, -70);
      body = scenes(intro, hero, ending, ink);
      break;
    }
    case 'swiss-story': {
      const orange = '#ff5029',
        cream = '#f7eedc',
        ink = '#151513';
      let intro = R('intro-bg', 0, 0, 1920, 1080, ink);
      intro += title(
        'intro-word',
        '开场文字',
        copy('少即是多', 'LESS IS MORE'),
        66,
        324,
        1788,
        en ? 280 : 350,
        orange
      );
      for (let i = 0; i < 12; i++) {
        intro += R(`intro-bar-${i}`, i * 160, 0, 100, 1080, cream);
        animate(
          `intro-bar-${i}`,
          channel('y', [
            [0, 0],
            [3 + i * 2, 0],
            [14 + i * 2, i % 2 ? 1080 : -1080],
            [240, i % 2 ? 1080 : -1080]
          ])
        );
      }
      let hero = R('hero-bg', 0, 0, 1920, 1080, orange);
      // Four oversized words lock together as a moving typographic grid.
      const cells = [
        [0, 0, orange, ink, copy('少', 'LESS')],
        [960, 0, cream, ink, copy('即', 'NOISE')],
        [0, 540, ink, cream, copy('是', 'MORE')],
        [960, 540, orange, ink, copy('多', 'IMPACT')]
      ];
      for (const [i, [x, y, bg, fg, word]] of cells.entries()) {
        hero += L(
          `cell-${i}`,
          '文字画格',
          x,
          y,
          960,
          540,
          R(`cell-bg-${i}`, 0, 0, 960, 540, bg) +
            title(
              `headline-${i}`,
              ['主标题', '第二行标题', '标题三', '标题四'][i],
              word,
              40,
              9,
              880,
              en ? [365, 320, 365, 270][i] : 408,
              fg,
              'center'
            ),
          'clipToBounds="true"'
        );
        transform(`headline-${i}`, 880, (en ? [365, 320, 365, 270][i] : 408) * 1.32, [
          [0, 1, 0],
          [55 + i * 3, 1, 0, 0, 480],
          [68 + i * 3, 1, 0],
          [106 + i * 4, 1, 0],
          [118 + i * 4, 0.92, i % 2 ? 6 : -6],
          [133 + i * 4, 1, 0],
          [240, 1, 0]
        ]);
      }
      hero += L(
        'center-seal',
        '中心标记',
        842,
        422,
        236,
        236,
        E('seal-bg', 0, 0, 236, 236, cream, ink, 4) + arrow('seal-arrow', 64, 64, 108, ink)
      );
      spin('center-seal', 236, -60, 300);
      let ending = R('outro-bg', 0, 0, 1920, 1080, cream);
      ending += title(
        'outro-title',
        '收尾文字',
        copy('不止一点', 'MORE IMPACT.'),
        84,
        70,
        1752,
        en ? 260 : 350,
        ink
      );
      ending += title(
        'outro-line',
        '收尾强调',
        copy('是每一点', 'LESS NOISE.'),
        84,
        511,
        1752,
        en ? 280 : 350,
        orange
      );
      ending += small(
        'outro-caption',
        '收尾短句',
        copy('删去多余。留下态度。', 'CUT THE NOISE. KEEP THE ATTITUDE.'),
        95,
        1008,
        1700,
        ink,
        26
      );
      slam('outro-title', 1752, (en ? 260 : 350) * 1.32, 210, -300, 0);
      slam('outro-line', 1752, (en ? 280 : 350) * 1.32, 213, 300, 0);
      body = scenes(intro, hero, ending, ink);
      break;
    }
    case 'aurora-data': {
      const acid = '#d7ff00',
        ink = '#101914',
        mint = '#77ffbe',
        white = '#f5f5dd';
      let intro = R('intro-bg', 0, 0, 1920, 1080, ink);
      intro += small('intro-kicker', '开场短句', 'MOMENTUM / IN MOTION', 90, 70, 1740, mint, 34);
      for (const [i, value] of ['024', '048', '096'].entries()) {
        intro += T(
          `intro-count-${i}`,
          '开场数字',
          value,
          95,
          178,
          1490,
          650,
          acid,
          'Impact',
          'Regular'
        ).replace('name="开场数字"', `name="开场数字" alpha="${i === 0 ? 1 : 0}"`);
        animate(
          `intro-count-${i}`,
          channel('alpha', [
            [0, i === 0 ? 1 : 0],
            ...(i
              ? [
                  [i * 16 - 1, 0],
                  [i * 16, 1]
                ]
              : []),
            [i * 16 + 15, 1],
            [i * 16 + 16, 0],
            [240, 0]
          ])
        );
      }
      intro += arrow('intro-arrow', 1410, 340, 360, mint);
      let hero = R('hero-bg', 0, 0, 1920, 1080, acid);
      // Stacked chevrons supply direction; the statistic itself is the composition.
      for (let i = 0; i < 5; i++) {
        hero += P(
          `speed-${i}`,
          '速度线',
          1040 + i * 150,
          190,
          170,
          660,
          'M 0 0 L 70 0 L 170 330 L 70 660 L 0 660 L 100 330 Z',
          '#b8df00'
        );
        animate(
          `speed-${i}`,
          channel('x', [
            [0, 1040 + i * 150],
            [75, 1040 + i * 150],
            [180, 940 + i * 150],
            [240, 1040 + i * 150]
          ])
        );
      }
      hero += title(
        'headline',
        '主标题',
        copy('增长，不设限', 'GROWTH. NO LIMITS.'),
        89,
        61,
        1550,
        en ? 120 : 108,
        ink
      );
      hero += T('stat', '核心数值', '128', 58, 182, 1310, 652, ink, 'Impact', 'Regular');
      hero += T('unit', '指标单位', '%', 1360, 526, 410, 284, ink, 'Impact', 'Regular');
      hero += arrow('growth-arrow', 1455, 237, 290, ink);
      hero +=
        R('metric-strip', 0, 898, 1920, 112, ink) +
        small(
          'description',
          '指标说明',
          copy('同比增长 / 每一步，都算数', 'YEAR-ON-YEAR / EVERY MOVE COUNTS'),
          92,
          934,
          1610,
          acid,
          35
        );
      hero += small(
        'disclaimer',
        '数据说明',
        copy('创意演示 · 非真实业绩数据', 'ILLUSTRATIVE DATA / NOT ACTUAL RESULTS'),
        95,
        1030,
        1570,
        ink,
        24
      );
      for (let i = 0; i < 7; i++)
        hero += R(`metric-tick-${i}`, 1460 + i * 54, 923, 26, 62, i % 2 ? mint : acid);
      slam('stat', 1310, 652 * 1.32, 57, 0, 370);
      slam('unit', 410, 284 * 1.32, 67, 300, 0);
      transform('growth-arrow', 290, 290, [
        [0, 1],
        [77, 1],
        [89, 1.13, 0, 0, -24],
        [101, 1],
        [133, 1],
        [145, 1.13, 0, 0, -24],
        [157, 1],
        [189, 1],
        [201, 1.13],
        [240, 1]
      ]);
      let ending = R('outro-bg', 0, 0, 1920, 1080, mint);
      for (let i = 0; i < 4; i++) {
        ending += arrow(`outro-arrow-${i}`, 150 + i * 435, 145, 300, ink);
        slam(`outro-arrow-${i}`, 300, 300, 205 + i * 3, -150, 150);
      }
      ending += title(
        'outro-title',
        '收尾文字',
        copy('势不可挡', 'KEEP RISING.'),
        96,
        525,
        1728,
        en ? 290 : 330,
        ink
      );
      ending += small(
        'outro-caption',
        '收尾短句',
        copy('下一次突破，现在开始。', 'YOUR NEXT BREAKTHROUGH STARTS NOW.'),
        98,
        1000,
        1600,
        ink,
        30
      );
      body = scenes(intro, hero, ending, white);
      break;
    }
    case 'hologram-product': {
      const blue = '#1746ff',
        pink = '#ff98d8',
        cream = '#fff1d8',
        ink = '#12121b';
      let intro = R('intro-bg', 0, 0, 1920, 1080, pink);
      for (let i = 0; i < 22; i++) {
        const h = 100 + Math.abs(Math.sin(i * 1.9)) * 550;
        intro += R(`wave-${i}`, 68 + i * 82, 540 - h / 2, 42, n(h), blue, 21);
        transform(`wave-${i}`, 42, h, [
          [0, 0.15],
          [8 + (i % 6), 1.1],
          [22, 0.45],
          [34, 1],
          [47, 0.6],
          [240, 0.6]
        ]);
      }
      intro += title(
        'intro-word',
        '开场文字',
        copy('准备好了吗', 'READY TO PLAY?'),
        90,
        367,
        1740,
        en ? 242 : 286,
        ink,
        'center'
      );
      let hero = R('hero-bg', 0, 0, 1920, 1080, blue);
      // A native vector record: rotating off-center label and grooves remain crisp at any scale.
      let disc = E('vinyl-body', 0, 0, 934, 934, ink);
      for (let i = 0; i < 14; i++) {
        const inset = 30 + i * 19;
        disc += E(
          `groove-${i}`,
          inset,
          inset,
          934 - inset * 2,
          934 - inset * 2,
          '#00000000',
          i % 3 ? '#44435c' : '#787393',
          2
        );
      }
      disc += E('record-label', 300, 300, 334, 334, pink);
      disc += P(
        'label-half',
        '唱片配色',
        300,
        300,
        334,
        334,
        'M 167 0 A 167 167 0 0 1 167 334 Z',
        cream
      );
      disc += E('record-hole', 444, 444, 46, 46, ink);
      disc += small('record-name', '产品名称', 'SIDE / A', 395, 365, 230, ink, 34);
      hero += L('record', '唱片', 530, 63, 934, 934, disc);
      spin('record', 934, -20, 240);
      hero += title(
        'headline',
        '主标题',
        copy('热爱', 'TURN IT'),
        65,
        15,
        1530,
        en ? 285 : 296,
        cream
      );
      hero += title(
        'headline-two',
        '第二行标题',
        copy('全频上场', 'ALL THE WAY UP'),
        69,
        en ? 704 : 630,
        1782,
        en ? 217 : 310,
        cream
      );
      hero += L(
        'drop-sticker',
        '上新贴纸',
        1515,
        274,
        310,
        310,
        burst('sticker-shape', 0, 0, 310, pink, 16, 0.84) +
          small('badge', '角标', 'NEW', 75, 75, 180, ink, 58) +
          small('badge-two', '角标副文', 'DROP', 61, 139, 210, ink, 58),
        `matrix="${matrix(310, 310, 1, 12)}"`
      );
      for (let y = 0; y < 4; y++)
        for (let x = 0; x < 6; x++)
          hero += E(`dot-${x}-${y}`, 84 + x * 42, 362 + y * 42, 15, 15, pink);
      hero += small(
        'description',
        '产品短句',
        copy('让每一次心跳，都有自己的频率。', 'FIND YOUR FREQUENCY. FEEL EVERYTHING.'),
        87,
        1001,
        1640,
        pink,
        30
      );
      slam('headline', 1530, (en ? 285 : 296) * 1.32, 55, -300, -50);
      slam('headline-two', 1782, (en ? 217 : 310) * 1.32, 62, 300, 80);
      transform('drop-sticker', 310, 310, [
        [0, 0.8, 12],
        [70, 0.8, 12],
        [81, 1.1, -8],
        [93, 1, 12],
        [147, 1, 12],
        [159, 1.1, -8],
        [171, 1, 12],
        [240, 1, 12]
      ]);
      let ending = R('outro-bg', 0, 0, 1920, 1080, ink);
      for (let i = 0; i < 7; i++)
        ending += E(
          `end-ring-${i}`,
          525 + i * 42,
          120 + i * 42,
          870 - i * 84,
          870 - i * 84,
          '#00000000',
          i % 2 ? blue : pink,
          12
        );
      ending += title(
        'outro-title',
        '收尾文字',
        copy('现在，开场', 'PRESS PLAY.'),
        88,
        340,
        1744,
        en ? 310 : 304,
        cream,
        'center'
      );
      ending += small(
        'outro-caption',
        '收尾短句',
        'YOUR SOUND. YOUR RULES.',
        93,
        973,
        1700,
        pink,
        34
      );
      slam('outro-title', 1744, (en ? 310 : 304) * 1.32, 210, 0, 180);
      body = scenes(intro, hero, ending, pink);
      break;
    }
    default:
      return null;
  }
  return `<pagx width="1920" height="1080">${L('canvas', '画面', 0, 0, 1920, 1080, body, 'clipToBounds="true"')}<Animations><Animation id="main" duration="240" frameRate="30" loop="once">${motions.join('')}</Animation></Animations></pagx>`.replaceAll(
    '><',
    '>\n<'
  );
}
