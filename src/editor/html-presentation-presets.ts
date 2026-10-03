import type { HtmlPreset } from './html-presets';
import { presentation } from './html-preset-builder';
import { CAMPAIGN_PRESETS } from './html-campaign-presets';

export const PRESENTATION_PRESETS: HtmlPreset[] = [
  presentation({
    id: 'prism-launch',
    name: '棱镜发布',
    variables: {
      title: '让想法\n闪耀全场',
      subtitle: '把灵感变成值得被看见的作品。',
      color: '#395fea',
      backgroundColor: '#eef0fa',
      textColor: '#18233f',
      eyebrow: 'IDEAS IN MOTION',
      footer: 'DESIGN THE NEXT',
      chapter: '01 / CREATIVE KEYNOTE',
      caption: 'FORM / LIGHT / POSSIBILITY'
    },
    css: `
.halo { position: absolute; width: 1130px; height: 1130px; right: -80px; top: -40px; border-radius: 50%; background: radial-gradient(circle, #d1d9fb 0, #dfe5fa88 46%, transparent 68%); }
.corner { position: absolute; left: 112px; top: 80px; width: 54px; height: 54px; display: grid; grid-template-columns: 1fr 1fr; gap: 5px; transform: rotate(-10deg); }
.corner i { background: var(--accent); border-radius: 4px; }
.intro { position: absolute; left: 112px; top: 251px; width: 1020px; z-index: 2; }
.intro .eyebrow { color: var(--accent); margin-bottom: 34px; }
.intro h1 { font-size: var(--title-size); line-height: 1.21; }
.intro .subtitle { margin-top: 40px; color: #58637f; }
.intro .rule { width: 106px; height: 7px; background: var(--accent); margin-top: 53px; }
.sculpture { position: absolute; width: 780px; height: 780px; right: 10px; top: 154px; }
.disc { position: absolute; border-radius: 50%; border: 2px solid #ffffffaa; box-shadow: inset 15px 13px 28px #ffffffa8, inset -12px -16px 28px #5267b948, 12px 28px 42px #6375ba26; }
.disc-a { width: 650px; height: 650px; left: 44px; top: 6px; background: linear-gradient(135deg, #e5eaffcc 2%, #aec6ffb0 38%, var(--accent) 100%); }
.disc-b { width: 520px; height: 520px; left: 106px; top: 64px; background: linear-gradient(145deg, #f5f7ffdd, #bec7f080 61%, #829ae0b5); }
.disc-c { width: 390px; height: 390px; left: 169px; top: 125px; background: linear-gradient(140deg, #f2f4ffe0, #e2e9ff88 50%, #d1b9e1dd); }
.core { position: absolute; left: 231px; top: 184px; width: 265px; height: 265px; border-radius: 50%; background: radial-gradient(circle at 30% 25%, #ffedce, #ffcbb4 40%, #e3a2c2 75%, #8999c9); box-shadow: inset 6px 8px 18px #fff9, 14px 24px 35px #49599240; }
.orbit { position: absolute; width: 730px; height: 278px; left: 0; top: 208px; border: 3px solid #ffffffc8; border-radius: 50%; transform: rotate(-28deg); }
.chip { position: absolute; right: 151px; top: 260px; width: 90px; height: 90px; border-radius: 20px; background: #fafbffe0; box-shadow: 0 14px 35px #43579625; display: grid; place-items: center; font-size: 45px; color: var(--accent); transform: rotate(13deg); }
.caption { position: absolute; right: 118px; bottom: 190px; color: #68748f; font-size: 23px; letter-spacing: 3px; }
.footer { border-top: 2px solid #d3d9e8; padding-top: 29px; color: #5f6a85; }
`,
    body: `
<div class="halo"></div><div class="corner"><i></i><i></i><i></i><i></i></div>
<section class="intro"><div class="eyebrow" data-variable="eyebrow"></div><h1 data-variable="title"></h1><p class="subtitle" data-variable="subtitle"></p><div class="rule"></div></section>
<div class="sculpture"><div class="disc disc-a"></div><div class="disc disc-b"></div><div class="disc disc-c"></div><div class="core"></div><div class="orbit"></div></div><div class="chip">↗</div>
<div class="caption" data-variable="caption"></div><footer class="footer meta"><span data-variable="footer"></span><span data-variable="chapter"></span></footer>
`,
    motion: `
tl.fromTo('.disc', { x: 200, scale: 0.65, opacity: 0 }, { x: 0, scale: 1, opacity: 1, duration: 1.25, stagger: 0.13, ease: 'expo.out' }, 0)
  .fromTo('.core', { scale: 0.3, opacity: 0 }, { scale: 1, opacity: 1, duration: 0.95, ease: 'back.out(1.3)' }, 0.35)
  .fromTo('.eyebrow, h1, .subtitle', { y: 80, opacity: 0 }, { y: 0, opacity: 1, duration: 0.8, stagger: 0.15, ease: 'power3.out' }, 0.15)
  .fromTo('.orbit', { rotation: -65, opacity: 0 }, { rotation: -28, opacity: 1, duration: 1.1, ease: 'power2.out' }, 0.5)
  .fromTo('.rule', { scaleX: 0 }, { scaleX: 1, duration: 0.7, ease: 'expo.out' }, 0.8)
  .fromTo('.chip, .caption, .footer, .corner', { opacity: 0 }, { opacity: 1, duration: 0.7 }, 0.8);
`,
    tick: `document.querySelector('.sculpture').style.transform = 'translateY(' + (Math.sin(t * 0.8) * 18) + 'px) rotate(' + (Math.sin(t * 0.45) * 3) + 'deg)';`
  }),
  presentation({
    id: 'swiss-story',
    name: '瑞士叙事',
    variables: {
      title: '把复杂\n讲得漂亮',
      subtitle: '清晰的结构，让好的观点自然发生。',
      color: '#e85b3e',
      backgroundColor: '#f2efe6',
      textColor: '#202925',
      eyebrow: 'LESS, BUT BETTER.',
      pointOne: '发现问题',
      pointTwo: '建立框架',
      pointThree: '创造价值',
      footer: 'A NEW WAY TO TELL YOUR STORY',
      displayNumber: '03',
      chapter: 'STORY / 001',
      pointOneLabel: '01 — EXPLORE',
      pointTwoLabel: '02 — DEFINE',
      pointThreeLabel: '03 — CREATE'
    },
    css: `
.topbar { position: absolute; top: 70px; left: 104px; right: 104px; display: flex; justify-content: space-between; border-bottom: 3px solid var(--ink); padding-bottom: 34px; }
.mark { display: flex; gap: 8px; align-items: center; }.mark i { width: 18px; height: 18px; background: var(--accent); }
.lead { position: absolute; left: 104px; top: 254px; width: 1070px; }
.lead h1 { font-size: var(--title-size); font-weight: 850; letter-spacing: -7px; line-height: 1.2; }
.lead .subtitle { width: 900px; margin-top: 42px; color: #616761; }
.index { position: absolute; right: 104px; top: 250px; width: 558px; height: 373px; background: var(--accent); color: #fff4de; overflow: hidden; }
.index-number { font-size: 335px; line-height: 1; font-weight: 850; letter-spacing: -30px; padding-left: 35px; transform: translateY(-5px); }
.index-circle { position: absolute; right: -96px; top: 39px; width: 270px; height: 270px; border-radius: 50%; border: 3px solid #fff4deaa; }
.points { position: absolute; left: 104px; right: 104px; top: 737px; display: grid; grid-template-columns: repeat(3, 1fr); border-top: 3px solid var(--ink); }
.point { position: relative; padding: 34px 36px 36px 0; border-right: 2px solid #a8aca2; }
.point:not(:first-child) { padding-left: 45px; }.point:last-child { border-right: 0; }
.point small { color: var(--accent); font-size: 24px; font-weight: 700; }.point h2 { margin-top: 13px; font-size: 42px; font-weight: 600; letter-spacing: -1px; }
.dot { position: absolute; width: 28px; height: 28px; background: var(--accent); border-radius: 50%; right: 35px; top: 55px; }
.footer { left: 104px; right: 104px; color: #646d64; bottom: 60px; }
`,
    body: `
<header class="topbar meta"><div class="mark"><i></i><span data-variable="eyebrow"></span></div><span data-variable="chapter"></span></header>
<section class="lead"><h1 data-variable="title"></h1><p class="subtitle" data-variable="subtitle"></p></section>
<div class="index"><div class="index-number" data-variable="displayNumber"></div><div class="index-circle"></div></div>
<section class="points"><div class="point"><small data-variable="pointOneLabel"></small><h2 data-variable="pointOne"></h2><i class="dot"></i></div><div class="point"><small data-variable="pointTwoLabel"></small><h2 data-variable="pointTwo"></h2><i class="dot"></i></div><div class="point"><small data-variable="pointThreeLabel"></small><h2 data-variable="pointThree"></h2><i class="dot"></i></div></section>
<footer class="footer meta"><span data-variable="footer"></span><span>THINK CLEAR. MAKE BOLD.</span></footer>
`,
    motion: `
tl.fromTo('.topbar', { scaleX: 0, transformOrigin: 'left' }, { scaleX: 1, duration: 0.75, ease: 'expo.out' }, 0)
  .fromTo('h1', { x: -90, opacity: 0 }, { x: 0, opacity: 1, duration: 0.8, ease: 'power3.out' }, 0.15)
  .fromTo('.subtitle', { y: 38, opacity: 0 }, { y: 0, opacity: 1, duration: 0.7, ease: 'power2.out' }, 0.4)
  .fromTo('.index', { clipPath: 'inset(0 0 100% 0)' }, { clipPath: 'inset(0 0 0% 0)', duration: 0.9, ease: 'power4.out' }, 0.2)
  .fromTo('.index-number', { y: 100 }, { y: -5, duration: 0.85, ease: 'expo.out' }, 0.3)
  .fromTo('.point', { y: 65, opacity: 0 }, { y: 0, opacity: 1, duration: 0.75, stagger: 0.15, ease: 'power3.out' }, 0.65)
  .fromTo('.footer', { opacity: 0 }, { opacity: 1, duration: 0.5 }, 1.25);
`,
    tick: `document.querySelector('.index-circle').style.transform = 'translateX(' + (Math.sin(t * 0.65) * 22) + 'px)';`
  }),
  presentation({
    id: 'aurora-data',
    name: '极光数据',
    variables: {
      title: '增长，看得见',
      subtitle: '每一份投入，都在走向更好的结果。',
      fontSize: 98,
      color: '#85e4ba',
      backgroundColor: '#0e1928',
      textColor: '#f0f4f6',
      eyebrow: 'PERFORMANCE / YEAR IN REVIEW',
      metricValue: 96.8,
      metricUnit: '%',
      chartValueOne: 12,
      chartValueTwo: 28,
      chartValueThree: 52,
      chartValueFour: 96.8,
      chartLabelOne: '第一季度',
      chartLabelTwo: '第二季度',
      chartLabelThree: '第三季度',
      chartLabelFour: '第四季度',
      trend: '↗ 持续增长',
      footer: 'MADE TO GROW ↗',
      metricLabel: '目标达成率',
      statOne: '3.2×',
      statOneLabel: '创作效率',
      statTwo: '12.8k',
      statTwoLabel: '灵感时刻'
    },
    css: `
.glow { position: absolute; left: 580px; top: -400px; width: 1450px; height: 1450px; background: radial-gradient(ellipse, #75b8a81f, #739cc209 42%, transparent 68%); }
.grid { position: absolute; inset: 0; opacity: 0.3; background-image: linear-gradient(#8499b015 1px, transparent 1px), linear-gradient(90deg, #8499b015 1px, transparent 1px); background-size: 96px 96px; mask-image: linear-gradient(90deg, transparent, black); }
.header { position: absolute; top: 102px; left: 112px; }.header .eyebrow { color: var(--accent); margin-bottom: 37px; }.header h1 { letter-spacing: -4px; }.header .subtitle { color: #b1bfcd; margin-top: 26px; }
.metric { position: absolute; left: 104px; top: 438px; }.metric-number { font-size: 204px; font-weight: 650; letter-spacing: -13px; line-height: 1.08; font-variant-numeric: tabular-nums; }.metric-number small { font-size: 80px; letter-spacing: -3px; color: var(--accent); }.metric-label { font-size: 32px; color: #b1bfcd; margin-top: 10px; }
.delta { display: inline-block; font-size: 22px; letter-spacing: 2px; color: var(--accent); padding: 13px 20px; border: 2px solid #85e4ba35; border-radius: 99px; margin-top: 28px; }
.chart { position: absolute; top: 428px; right: 112px; width: 910px; height: 335px; overflow: visible; }.chart .guideline { stroke: #52647560; stroke-width: 2; stroke-dasharray: 5 12; }.chart .curve { stroke: var(--accent); stroke-width: 7; stroke-linecap: round; fill: none; stroke-dasharray: 1000; filter: drop-shadow(0 0 15px #85e4ba50); }.chart .endpoint { fill: var(--accent); stroke: #f0f4f6; stroke-width: 3; }.chart text { fill: #9bacba; font-size: 21px; font-family: var(--font); letter-spacing: 2px; }
.stats { position: absolute; bottom: 103px; left: 112px; right: 112px; border-top: 2px solid #354454; padding-top: 38px; display: flex; gap: 180px; }.stat strong { font-size: 56px; font-weight: 550; letter-spacing: -2px; }.stat span { margin-left: 24px; font-size: 26px; color: #9bacba; }.stat-note { margin-left: auto; font-size: 23px; letter-spacing: 3px; align-self: center; color: var(--accent); }
`,
    body: `
<div class="glow"></div><div class="grid"></div><header class="header"><div class="eyebrow" data-variable="eyebrow"></div><h1 data-variable="title"></h1><p class="subtitle" data-variable="subtitle"></p></header>
<div class="metric"><div class="metric-number"><span id="metric">0.0</span><small data-variable="metricUnit"></small></div><div class="metric-label" data-variable="metricLabel"></div><span class="delta" data-variable="trend"></span></div>
<svg class="chart" viewBox="0 0 910 335"><defs><linearGradient id="chart-fill" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="currentColor" stop-opacity=".22"/><stop offset="1" stop-color="currentColor" stop-opacity="0"/></linearGradient></defs><g class="guideline"><path d="M0 36H910 M0 136H910 M0 236H910"/></g><path class="area" style="color:var(--accent)" fill="url(#chart-fill)" d="M0 280 C90 275 108 208 190 211 S306 239 380 164 S495 186 570 107 S706 145 760 69 S830 29 910 14 L910 300 L0 300Z"/><path class="curve" pathLength="1000" d="M0 280 C90 275 108 208 190 211 S306 239 380 164 S495 186 570 107 S706 145 760 69 S830 29 910 14"/><circle class="endpoint" cx="910" cy="14" r="10"/><text x="0" y="334" data-variable="chartLabelOne"></text><text x="303" y="334" text-anchor="middle" data-variable="chartLabelTwo"></text><text x="607" y="334" text-anchor="middle" data-variable="chartLabelThree"></text><text x="910" y="334" text-anchor="end" data-variable="chartLabelFour"></text></svg>
<section class="stats"><div class="stat"><strong data-variable="statOne"></strong><span data-variable="statOneLabel"></span></div><div class="stat"><strong data-variable="statTwo"></strong><span data-variable="statTwoLabel"></span></div><span class="stat-note" data-variable="footer"></span></section>
`,
    motion: `
tl.fromTo('.header .eyebrow, .header h1, .header .subtitle', { y: 52, opacity: 0 }, { y: 0, opacity: 1, duration: 0.8, stagger: 0.13, ease: 'power3.out' }, 0.05)
  .fromTo('.metric', { x: -50, opacity: 0 }, { x: 0, opacity: 1, duration: 1, ease: 'expo.out' }, 0.4)
  .fromTo('.chart', { y: 45, opacity: 0 }, { y: 0, opacity: 1, duration: 0.8, ease: 'power2.out' }, 0.5)
  .fromTo('.area', { opacity: 0 }, { opacity: 1, duration: 1.4 }, 0.8)
  .fromTo('.endpoint', { scale: 0, transformOrigin: 'center' }, { scale: 1, duration: 0.45, ease: 'back.out(1.8)' }, 1.8)
  .fromTo('.stat, .stat-note', { y: 35, opacity: 0 }, { y: 0, opacity: 1, duration: 0.7, stagger: 0.15, ease: 'power3.out' }, 0.85);
`,
    setup: `
const values = ['chartValueOne', 'chartValueTwo', 'chartValueThree', 'chartValueFour'].map(key => Math.max(0, Number(v[key]) || 0));
const maximum = Math.max(1, ...values);
const points = values.map((value, index) => [index * (910 / 3), 290 - value / maximum * 270]);
let path = 'M' + points[0].join(' ');
for (let i = 1; i < points.length; i++) { const a = points[i - 1], b = points[i], mid = (a[0] + b[0]) / 2; path += ' C' + mid + ' ' + a[1] + ' ' + mid + ' ' + b[1] + ' ' + b.join(' '); }
document.querySelector('.curve').setAttribute('d', path);
document.querySelector('.area').setAttribute('d', path + ' L910 300 L0 300Z');
document.querySelector('.endpoint').setAttribute('cy', String(points[3][1]));
`,
    tick: `const progress = Math.max(0, Math.min(1, (t - 0.4) / 1.8)); const eased = 1 - Math.pow(1 - progress, 3); document.querySelector('#metric').textContent = (Number(v['metricValue']) * eased).toFixed(1); document.querySelector('.curve').style.strokeDashoffset = String(1000 * (1 - eased)); document.querySelector('.glow').style.transform = 'translateX(' + (Math.sin(t * 0.4) * 55) + 'px)';`
  }),
  presentation({
    id: 'velvet-keynote',
    name: '暮色演讲',
    variables: {
      title: '不止于此\n始于想象',
      subtitle: '让每一次表达，都拥有自己的光芒。',
      fontSize: 120,
      color: '#f8c29b',
      backgroundColor: '#251d2b',
      textColor: '#f8ede2',
      eyebrow: 'THE NEXT CHAPTER',
      footer: 'BEYOND THE ORDINARY',
      chapter: 'CREATIVE VISION / 05',
      signature: 'Make it remarkable.'
    },
    css: `
.atmosphere { position: absolute; right: -260px; top: -140px; width: 1500px; height: 1500px; background: radial-gradient(circle, #a259693f, #63404d0a 51%, transparent 65%); }
.toprule { position: absolute; top: 110px; left: 116px; right: 116px; height: 2px; background: #e1bca939; }
.eyebrow { position: absolute; left: 116px; top: 158px; color: var(--accent); }
.copy { position: absolute; left: 108px; top: 338px; width: 1100px; z-index: 2; }.copy h1 { font-weight: 500; letter-spacing: -6px; line-height: 1.2; }.copy .subtitle { margin-top: 52px; color: #d2bdbb; }
.ribbons { position: absolute; right: 20px; top: 212px; width: 1010px; height: 700px; overflow: visible; }
.ribbon-back { fill: none; stroke: #8d607a; stroke-width: 32; opacity: .55; }.ribbon-front { fill: none; stroke: url(#silk); stroke-width: 45; stroke-linecap: round; filter: drop-shadow(0 22px 16px #100c1990); }.ribbon-line { fill: none; stroke: var(--accent); stroke-width: 2; opacity: .7; }
.seal { position: absolute; right: 129px; top: 162px; width: 77px; height: 77px; border: 2px solid #f8c29b66; border-radius: 50%; display: grid; place-items: center; font-size: 42px; color: var(--accent); }
.italic { position: absolute; right: 129px; bottom: 186px; font-family: Georgia, 'Times New Roman', serif; font-size: 52px; font-style: italic; letter-spacing: -2px; color: #e7c6b5; }
.footer { left: 116px; right: 116px; bottom: 68px; border-top: 2px solid #e1bca939; padding-top: 28px; color: #ba9d9f; }
`,
    body: `
<div class="atmosphere"></div><div class="toprule rule"></div><div class="eyebrow" data-variable="eyebrow"></div><section class="copy"><h1 data-variable="title"></h1><p class="subtitle" data-variable="subtitle"></p></section>
<svg class="ribbons" viewBox="0 0 1010 700"><defs><linearGradient id="silk" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#79506c"/><stop offset=".3" stop-color="var(--accent)"/><stop offset=".52" stop-color="#fff1cf"/><stop offset=".73" stop-color="#bd827b"/><stop offset="1" stop-color="#705272"/></linearGradient></defs><path class="ribbon-back" d="M156 445 C-10 196 578 28 759 272 S781 660 597 544 S178 116 322 106"/><path class="ribbon-front" d="M80 454 C47 574 222 598 438 430 S815 152 890 276 S559 737 368 609 S298 334 416 236 S623 152 674 212"/><path class="ribbon-line" d="M26 327 C-11 101 706 -67 948 290 S743 737 496 638"/></svg>
<div class="seal">✳</div><div class="italic" data-variable="signature"></div><footer class="footer meta"><span data-variable="footer"></span><span data-variable="chapter"></span></footer>
`,
    motion: `
tl.fromTo('.toprule', { scaleX: 0 }, { scaleX: 1, duration: 1.1, ease: 'expo.out' }, 0)
  .fromTo('.eyebrow, .copy h1, .copy .subtitle', { y: 70, opacity: 0 }, { y: 0, opacity: 1, duration: 1, stagger: 0.16, ease: 'power3.out' }, 0.18)
  .fromTo('.ribbons', { x: 140, rotation: 15, opacity: 0 }, { x: 0, rotation: 0, opacity: 1, duration: 1.6, ease: 'expo.out' }, 0.1)
  .fromTo('.seal', { scale: 0.5, rotation: -60, opacity: 0 }, { scale: 1, rotation: 0, opacity: 1, duration: 1, ease: 'back.out(1.2)' }, 0.7)
  .fromTo('.italic, .footer', { opacity: 0 }, { opacity: 1, duration: 1 }, 1);
`,
    tick: `document.querySelector('.atmosphere').style.transform = 'scale(' + (1 + Math.sin(t * 0.55) * 0.07) + ')'; document.querySelector('.ribbon-line').style.transform = 'translate(' + (Math.sin(t * 0.5) * 16) + 'px,' + (Math.cos(t * 0.5) * 12) + 'px)';`
  }),
  presentation({
    id: 'blueprint-roadmap',
    name: '蓝图路线',
    variables: {
      title: '从灵感，到落地',
      subtitle: '好的路径，让每一步都更接近答案。',
      fontSize: 98,
      color: '#315cda',
      backgroundColor: '#edf2f5',
      textColor: '#193140',
      eyebrow: 'A CLEAR PATH FORWARD',
      stageOne: '探索方向',
      stageOneDescription: '洞察需求，找到值得解决的问题。',
      stageTwo: '构建方案',
      stageTwoDescription: '让想法成形，让细节经得起推敲。',
      stageThree: '发布作品',
      stageThreeDescription: '把成果带到真实世界，持续迭代。',
      stageOneLabel: '01 / DISCOVER',
      stageTwoLabel: '02 / BUILD',
      stageThreeLabel: '03 / LAUNCH',
      footer: 'VISION → ACTION → IMPACT',
      chapter: 'ONE STEP CLOSER.'
    },
    css: `
.blueprint-grid { position: absolute; inset: 0; background-image: linear-gradient(#53798b0d 1px, transparent 1px), linear-gradient(90deg, #53798b0d 1px, transparent 1px); background-size: 54px 54px; }
.header { position: absolute; left: 112px; top: 105px; }.header .eyebrow { color: var(--accent); margin-bottom: 34px; }.header h1 { letter-spacing: -4px; }.header .subtitle { color: #647787; margin-top: 26px; }
.compass { position: absolute; right: 119px; top: 124px; width: 148px; height: 148px; border: 2px solid #315cda40; border-radius: 50%; display: grid; place-items: center; }.compass:before, .compass:after { content: ''; position: absolute; background: #315cda30; }.compass:before { width: 182px; height: 2px; }.compass:after { height: 182px; width: 2px; }.compass span { font-size: 71px; color: var(--accent); }
.steps { position: absolute; left: 112px; right: 112px; top: 472px; display: grid; grid-template-columns: 1fr 1.12fr 1fr; gap: 29px; align-items: end; }
.step { position: relative; padding: 42px; height: 316px; border: 2px solid #cbd7df; border-radius: 20px; background: #f7fafbe8; box-shadow: 0 18px 30px #45677e0c; }.step:nth-child(2) { height: 350px; background: var(--accent); color: #f4f7ff; border-color: #ffffff28; box-shadow: 0 24px 44px #315cda25; }.step small { font-size: 25px; letter-spacing: 3px; color: var(--accent); }.step:nth-child(2) small { color: #d7e0ff; }.step h2 { font-size: 48px; margin-top: 33px; font-weight: 600; letter-spacing: -2px; }.step p { margin-top: 19px; font-size: 26px; line-height: 1.6; color: #627585; }.step:nth-child(2) p { color: #e1e9ff; }.step-icon { position: absolute; right: 35px; top: 35px; font-size: 42px; color: var(--accent); }.step:nth-child(2) .step-icon { color: #f3f7ff; }
.route { position: absolute; left: 145px; right: 145px; bottom: 162px; height: 4px; background: #c3d1db; }.route .progress { height: 4px; background: var(--accent); transform-origin: left; }.node { position: absolute; top: -10px; width: 24px; height: 24px; border-radius: 50%; background: var(--accent); border: 5px solid var(--bg); outline: 2px solid var(--accent); }.node:nth-child(2) { left: 0; }.node:nth-child(3) { left: 50%; }.node:nth-child(4) { right: 0; }
.footer { color: #748591; bottom: 60px; }
`,
    body: `
<div class="blueprint-grid"></div><header class="header"><div class="eyebrow" data-variable="eyebrow"></div><h1 data-variable="title"></h1><p class="subtitle" data-variable="subtitle"></p></header><div class="compass"><span>↗</span></div>
<section class="steps"><article class="step"><small data-variable="stageOneLabel"></small><span class="step-icon">◎</span><h2 data-variable="stageOne"></h2><p data-variable="stageOneDescription"></p></article><article class="step"><small data-variable="stageTwoLabel"></small><span class="step-icon">◈</span><h2 data-variable="stageTwo"></h2><p data-variable="stageTwoDescription"></p></article><article class="step"><small data-variable="stageThreeLabel"></small><span class="step-icon">↗</span><h2 data-variable="stageThree"></h2><p data-variable="stageThreeDescription"></p></article></section>
<div class="route"><div class="progress"></div><i class="node"></i><i class="node"></i><i class="node"></i></div><footer class="footer meta"><span data-variable="footer"></span><span data-variable="chapter"></span></footer>
`,
    motion: `
tl.fromTo('.header .eyebrow, .header h1, .header .subtitle', { x: -70, opacity: 0 }, { x: 0, opacity: 1, duration: 0.85, stagger: 0.13, ease: 'power3.out' }, 0.05)
  .fromTo('.compass', { rotation: -100, scale: 0.5, opacity: 0 }, { rotation: 0, scale: 1, opacity: 1, duration: 1.2, ease: 'expo.out' }, 0.15)
  .fromTo('.step', { y: 100, opacity: 0 }, { y: 0, opacity: 1, duration: 0.9, stagger: 0.18, ease: 'power4.out' }, 0.4)
  .fromTo('.progress', { scaleX: 0 }, { scaleX: 1, duration: 1.55, ease: 'power2.inOut' }, 0.7)
  .fromTo('.node', { scale: 0 }, { scale: 1, duration: 0.4, stagger: 0.4, ease: 'back.out(1.6)' }, 0.75)
  .fromTo('.footer', { opacity: 0 }, { opacity: 1, duration: 0.8 }, 1.2);
`,
    tick: `document.querySelector('.compass span').style.transform = 'translate(' + (Math.sin(t * 0.7) * 5) + 'px,' + (-Math.sin(t * 0.7) * 5) + 'px)';`
  }),
  ...CAMPAIGN_PRESETS
];
