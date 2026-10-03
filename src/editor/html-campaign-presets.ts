import { presentation } from './html-preset-builder';
import type { HtmlPreset } from './html-presets';

export const CAMPAIGN_PRESETS: HtmlPreset[] = [
  presentation({
    id: 'hologram-product',
    name: '全息产品',
    variables: {
      title: '下一代体验\n现在登场',
      subtitle: '把产品优势，变成观众记得住的亮点。',
      fontSize: 108,
      color: '#83f6d2',
      backgroundColor: '#080e1c',
      textColor: '#f3f7ff',
      eyebrow: 'PRODUCT / SPOTLIGHT',
      productName: 'NOVA',
      productTag: '为创作者而生',
      featureOne: '轻巧设计',
      featureOneDescription: '随时随地，灵感随行',
      featureTwo: '强劲性能',
      featureTwoDescription: '复杂任务，也能从容完成',
      featureThree: '持久陪伴',
      featureThreeDescription: '专注创作，尽兴表达',
      footer: '探索更多可能',
      chapter: 'DESIGNED FOR WHAT’S NEXT'
    },
    css: `
.mesh { position: absolute; inset: 0; background: radial-gradient(ellipse at 73% 46%, color-mix(in srgb, var(--accent) 18%, transparent), transparent 54%); }
.floor { position: absolute; left: 800px; top: 615px; width: 1120px; height: 500px; background-image: linear-gradient(#83f6d21a 1px, transparent 1px), linear-gradient(90deg, #83f6d21a 1px, transparent 1px); background-size: 62px 62px; transform: perspective(520px) rotateX(61deg); mask-image: radial-gradient(ellipse, black, transparent 70%); }
.copy { position: absolute; left: 112px; top: 124px; width: 980px; z-index: 2; }.eyebrow { color: var(--accent); margin-bottom: 48px; }.copy h1 { letter-spacing: -4px; }.copy .subtitle { margin-top: 30px; color: #a8b7cd; width: 940px; }
.device-wrap { position: absolute; right: 137px; top: 160px; width: 510px; height: 570px; }
.orbit-ring { position: absolute; left: -160px; top: 96px; width: 790px; height: 350px; border: 2px solid color-mix(in srgb, var(--accent) 50%, transparent); border-radius: 50%; transform: rotate(-28deg); box-shadow: 0 0 40px #83f6d21a; }
.device { position: absolute; left: 85px; top: 0; width: 320px; height: 494px; border: 2px solid #a9fff06b; border-radius: 42px; padding: 14px; background: linear-gradient(135deg, #cad8f748, #122235 35%, #73d7c530 85%, #f4ffff70); box-shadow: -20px 30px 0 #020814, -21px 31px 0 #698384, 0 42px 110px #0009; transform: rotate(-12deg) skewY(4deg); }
.screen { height: 100%; position: relative; overflow: hidden; border-radius: 30px; border: 1px solid #b7ffe329; background: radial-gradient(circle at 75% 30%, #48649b70, #0b152a 70%); display: flex; flex-direction: column; justify-content: flex-end; padding: 34px 26px; }
.screen:before { content: ''; position: absolute; width: 66px; height: 7px; border-radius: 8px; background: #020813; top: 18px; left: 50%; transform: translateX(-50%); }
.orb { position: absolute; top: 95px; left: 41px; width: 207px; height: 207px; border-radius: 45% 55% 38% 62%; border: 1px solid #d0fff785; background: conic-gradient(from 30deg, #667fff, var(--accent), #e4fbff, #5269ca, var(--accent)); box-shadow: inset 12px 8px 34px #fff9, inset -20px -10px 32px #020b4466, 0 0 55px #83f6d230; }
.product-name { font-size: 54px; font-weight: 700; letter-spacing: 5px; }.product-tag { margin-top: 12px; font-size: 21px; color: #b1c6d9; }
.satellite { position: absolute; right: -34px; bottom: 97px; width: 95px; height: 95px; border: 1px solid #bcfff37a; background: #172c38ed; border-radius: 25px; display: grid; place-items: center; font-size: 51px; color: var(--accent); transform: rotate(12deg); box-shadow: 0 18px 50px #0006; }
.features { position: absolute; left: 112px; right: 112px; top: 742px; display: grid; grid-template-columns: repeat(3, 1fr); gap: 26px; }
.feature { min-width: 0; border: 1px solid #8eabd43b; background: linear-gradient(130deg, #23354885, #111b2e); border-radius: 22px; padding: 30px 32px; display: flex; gap: 26px; align-items: center; }.feature-icon { flex: 0 0 64px; height: 64px; border: 1px solid #83f6d259; border-radius: 18px; display: grid; place-items: center; font-size: 33px; color: var(--accent); }.feature h2 { font-size: 34px; font-weight: 600; }.feature p { color: #a6b9cb; font-size: 24px; line-height: 1.5; margin-top: 12px; }.footer { color: #95aabf; bottom: 58px; }
`,
    body: `
<div class="mesh"></div><div class="floor"></div><section class="copy"><div class="eyebrow" data-variable="eyebrow"></div><h1 data-variable="title"></h1><p class="subtitle" data-variable="subtitle"></p></section>
<div class="device-wrap"><div class="orbit-ring"></div><div class="device"><div class="screen"><div class="orb"></div><strong class="product-name" data-variable="productName"></strong><span class="product-tag" data-variable="productTag"></span></div></div><div class="satellite">✧</div></div>
<section class="features"><article class="feature"><span class="feature-icon">◇</span><div><h2 data-variable="featureOne"></h2><p data-variable="featureOneDescription"></p></div></article><article class="feature"><span class="feature-icon">↯</span><div><h2 data-variable="featureTwo"></h2><p data-variable="featureTwoDescription"></p></div></article><article class="feature"><span class="feature-icon">∞</span><div><h2 data-variable="featureThree"></h2><p data-variable="featureThreeDescription"></p></div></article></section>
<footer class="footer meta"><span data-variable="footer"></span><span data-variable="chapter"></span></footer>
`,
    motion: `
tl.fromTo('.copy .eyebrow, h1, .subtitle', { y: 65, opacity: 0 }, { y: 0, opacity: 1, duration: .9, stagger: .14, ease: 'power4.out' }, .1)
  .fromTo('.device-wrap', { x: 220, scale: .8, opacity: 0 }, { x: 0, scale: 1, opacity: 1, duration: 1.4, ease: 'expo.out' }, .1)
  .fromTo('.orbit-ring', { scale: .4, opacity: 0 }, { scale: 1, opacity: 1, duration: 1.2, ease: 'power3.out' }, .55)
  .fromTo('.feature', { y: 70, opacity: 0 }, { y: 0, opacity: 1, duration: .8, stagger: .16, ease: 'power3.out' }, .75)
  .fromTo('.footer', { opacity: 0 }, { opacity: 1, duration: .7 }, 1.4);
`,
    tick: `document.querySelector('.device').style.transform = 'rotate(' + (-12 + Math.sin(t * .65) * 2) + 'deg) skewY(4deg) translateY(' + (Math.sin(t * .9) * 12) + 'px)'; document.querySelector('.orb').style.transform = 'rotate(' + (t * 16) + 'deg)'; document.querySelector('.satellite').style.transform = 'rotate(12deg) translateY(' + (Math.sin(t * 1.2 + 1) * 16) + 'px)';`
  }),
  presentation({
    id: 'kinetic-offer',
    name: '动感促销',
    variables: {
      title: '好物上新\n限时心动',
      subtitle: '新品首发礼遇，把喜欢带回家。',
      fontSize: 120,
      color: '#dfff65',
      backgroundColor: '#6026ed',
      textColor: '#ffffff',
      eyebrow: 'NEW DROP / SPECIAL OFFER',
      badge: '限时特惠',
      offerLabel: '新品尝鲜价',
      price: '¥199',
      originalPrice: '¥299',
      cta: '立即抢购',
      finePrint: '活动时间与优惠规则以店铺说明为准',
      footer: '你的品牌名称',
      chapter: 'MAKE YOUR NEXT MOVE',
      discount: '新品首发'
    },
    css: `
.spotlight { position: absolute; width: 1650px; height: 1650px; right: -360px; top: -380px; background: radial-gradient(circle, #b074ff88, transparent 65%); }
.stripe { position: absolute; right: -170px; top: -200px; width: 590px; height: 1500px; background: #ffffff08; transform: rotate(28deg); }.stripe.second { right: 470px; width: 65px; background: var(--accent); opacity: .13; }
.copy { position: absolute; left: 112px; top: 114px; width: 1020px; }.eyebrow { font-size: 22px; color: var(--accent); margin-bottom: 43px; }.copy h1 { font-weight: 900; line-height: 1.17; }.copy .subtitle { margin-top: 35px; width: 970px; color: #e4d9ff; }
.offer-card { position: absolute; right: 131px; top: 199px; width: 550px; height: 512px; background: var(--accent); color: #201035; border-radius: 30px; padding: 56px 46px; box-shadow: 18px 22px 0 #321071, 0 25px 70px #16003755; transform: rotate(7deg); }
.offer-label { font-size: 29px; font-weight: 650; }.price { margin-top: 38px; font-size: 117px; font-weight: 900; letter-spacing: -8px; line-height: 1; overflow-wrap: anywhere; }.original { font-size: 33px; text-decoration: line-through; opacity: .55; margin-top: 20px; }.ticket-rule { border-top: 2px dashed #20103550; margin-top: 33px; padding-top: 28px; display: flex; justify-content: space-between; align-items: center; font-size: 26px; font-weight: 700; }.ticket-rule b { font-size: 44px; }
.badge { position: absolute; right: 71px; top: 115px; min-width: 230px; height: 95px; border: 3px solid #201035; border-radius: 50%; background: #fff; color: #31185f; display: grid; place-items: center; padding: 0 25px; font-size: 30px; font-weight: 750; transform: rotate(-12deg); }
.spark { position: absolute; right: 732px; top: 581px; font-size: 154px; line-height: 1; color: var(--accent); }.cta { position: absolute; left: 112px; top: 733px; width: 596px; min-height: 106px; border-radius: 18px; padding: 24px 35px; background: var(--accent); color: #261047; display: flex; align-items: center; justify-content: space-between; gap: 24px; font-size: 42px; font-weight: 800; }.cta b { font-size: 54px; }.fine { position: absolute; left: 112px; top: 880px; font-size: 23px; color: #dfd1f5; }.footer { bottom: 51px; border-top: 1px solid #ffffff35; padding-top: 27px; color: #e4d9ff; }
`,
    body: `
<div class="spotlight"></div><div class="stripe"></div><div class="stripe second"></div><section class="copy"><div class="eyebrow" data-variable="eyebrow"></div><h1 data-variable="title"></h1><p class="subtitle" data-variable="subtitle"></p></section>
<div class="offer-card"><div class="offer-label" data-variable="offerLabel"></div><div class="price" data-variable="price"></div><div class="original" data-variable="originalPrice"></div><div class="ticket-rule"><span data-variable="discount"></span><b>↗</b></div></div><div class="badge" data-variable="badge"></div><div class="spark">✳</div>
<div class="cta"><span data-variable="cta"></span><b>↗</b></div><p class="fine" data-variable="finePrint"></p><footer class="footer meta"><span data-variable="footer"></span><span data-variable="chapter"></span></footer>
`,
    motion: `
tl.fromTo('.eyebrow, h1, .subtitle', { x: -130, opacity: 0 }, { x: 0, opacity: 1, duration: .8, stagger: .13, ease: 'power4.out' }, .05)
  .fromTo('.offer-card', { y: 160, rotation: -18, scale: .7, opacity: 0 }, { y: 0, rotation: 7, scale: 1, opacity: 1, duration: 1.1, ease: 'back.out(1.2)' }, .35)
  .fromTo('.badge', { scale: 0, rotation: -80 }, { scale: 1, rotation: -12, duration: .7, ease: 'back.out(2)' }, .9)
  .fromTo('.spark', { scale: 0, rotation: -120 }, { scale: 1, rotation: 0, duration: .8, ease: 'back.out(1.5)' }, .7)
  .fromTo('.cta', { y: 70, opacity: 0 }, { y: 0, opacity: 1, duration: .75, ease: 'expo.out' }, 1)
  .fromTo('.fine, .footer', { opacity: 0 }, { opacity: 1, duration: .6 }, 1.5);
`,
    tick: `document.querySelector('.spotlight').style.transform = 'translateX(' + (Math.sin(t * .6) * 60) + 'px)'; document.querySelector('.cta b').style.transform = 'translate(' + (Math.sin(t * 2) * 5) + 'px,' + (-Math.sin(t * 2) * 5) + 'px)';`
  }),
  presentation({
    id: 'split-comparison',
    name: '前后对比',
    variables: {
      title: '改变，一眼看见',
      subtitle: '用清晰的对比，讲明白升级的价值。',
      fontSize: 96,
      color: '#a2f5cb',
      backgroundColor: '#141924',
      textColor: '#f6f8ff',
      eyebrow: 'BEFORE / AFTER',
      beforeLabel: '升级之前',
      afterLabel: '升级之后',
      beforeValue: '30 分钟',
      afterValue: '3 分钟',
      metricLabel: '单次完成时间',
      beforeOne: '重复操作',
      beforeTwo: '流程分散',
      beforeThree: '反复等待',
      afterOne: '一键完成',
      afterTwo: '高效协作',
      afterThree: '即刻交付',
      result: '让时间，留给更重要的事',
      footer: '用你的真实结果替换示例',
      chapter: 'LESS FRICTION. MORE IMPACT.'
    },
    css: `
.ambient { position: absolute; inset: 0; background: radial-gradient(ellipse at 76% 70%, #53bda017, transparent 58%); }
.header { position: absolute; left: 112px; top: 98px; right: 112px; }.eyebrow { color: var(--accent); margin-bottom: 32px; }.header h1 { letter-spacing: -3px; }.subtitle { color: #a8b4c8; margin-top: 25px; }
.panels { position: absolute; left: 112px; right: 112px; top: 405px; height: 442px; display: grid; grid-template-columns: 1fr 1fr; gap: 44px; }.comparison-panel { border-radius: 26px; padding: 35px 44px; border: 1px solid #c6d4ec2b; background: #202737; min-width: 0; position: relative; }.comparison-panel.after { border-color: color-mix(in srgb, var(--accent) 55%, transparent); background: linear-gradient(120deg, #243d3c, #1c2c36); box-shadow: 0 16px 64px #70ffc311; }
.panel-label { font-size: 28px; color: #a8b4c8; font-weight: 600; display: flex; justify-content: space-between; }.after .panel-label { color: var(--accent); }.panel-label small { font-size: 22px; letter-spacing: 3px; }
.value { font-size: 92px; letter-spacing: -4px; font-weight: 750; line-height: 1.1; margin-top: 29px; }.after .value { color: var(--accent); }.metric-label { font-size: 23px; color: #a8b4c8; margin-top: 12px; }.items { border-top: 1px solid #d4e1ff25; margin-top: 28px; padding-top: 26px; display: flex; flex-wrap: wrap; gap: 15px 22px; }.item { font-size: 26px; display: flex; align-items: center; gap: 11px; }.item i { font-style: normal; color: #8794aa; }.after .item i { color: var(--accent); }
.bridge { position: absolute; left: 50%; top: 590px; transform: translateX(-50%); width: 78px; height: 78px; border-radius: 50%; background: var(--accent); border: 7px solid var(--bg); display: grid; place-items: center; font-size: 38px; color: #16372a; z-index: 2; }
.result { position: absolute; left: 112px; right: 112px; top: 896px; display: flex; justify-content: center; gap: 20px; align-items: center; font-size: 30px; }.result i { width: 38px; height: 3px; background: var(--accent); }.footer { bottom: 45px; color: #94a6be; font-size: 21px; }
`,
    body: `
<div class="ambient"></div><header class="header"><div class="eyebrow" data-variable="eyebrow"></div><h1 data-variable="title"></h1><p class="subtitle" data-variable="subtitle"></p></header>
<section class="panels"><article class="comparison-panel before"><div class="panel-label"><span data-variable="beforeLabel"></span><small>01</small></div><div class="value" data-variable="beforeValue"></div><div class="metric-label" data-variable="metricLabel"></div><div class="items"><span class="item"><i>−</i><span data-variable="beforeOne"></span></span><span class="item"><i>−</i><span data-variable="beforeTwo"></span></span><span class="item"><i>−</i><span data-variable="beforeThree"></span></span></div></article><article class="comparison-panel after"><div class="panel-label"><span data-variable="afterLabel"></span><small>02</small></div><div class="value" data-variable="afterValue"></div><div class="metric-label" data-variable="metricLabel"></div><div class="items"><span class="item"><i>✓</i><span data-variable="afterOne"></span></span><span class="item"><i>✓</i><span data-variable="afterTwo"></span></span><span class="item"><i>✓</i><span data-variable="afterThree"></span></span></div></article></section>
<div class="bridge">→</div><div class="result"><i></i><span data-variable="result"></span><i></i></div><footer class="footer meta"><span data-variable="footer"></span><span data-variable="chapter"></span></footer>
`,
    motion: `
tl.fromTo('.header .eyebrow, h1, .subtitle', { y: 55, opacity: 0 }, { y: 0, opacity: 1, duration: .8, stagger: .12, ease: 'power3.out' }, .05)
  .fromTo('.before', { x: -140, opacity: 0 }, { x: 0, opacity: 1, duration: .9, ease: 'power4.out' }, .35)
  .fromTo('.after', { x: 140, opacity: 0 }, { x: 0, opacity: 1, duration: .9, ease: 'power4.out' }, .55)
  .fromTo('.value', { y: 32, opacity: 0 }, { y: 0, opacity: 1, duration: .65, stagger: .2 }, .7)
  .fromTo('.item', { y: 22, opacity: 0 }, { y: 0, opacity: 1, duration: .5, stagger: .09 }, .95)
  .fromTo('.bridge', { scale: 0 }, { scale: 1, duration: .6, ease: 'back.out(1.7)' }, 1)
  .fromTo('.result, .footer', { y: 16, opacity: 0 }, { y: 0, opacity: 1, duration: .7, stagger: .1 }, 1.5);
`,
    tick: `document.querySelector('.bridge').style.boxShadow = '0 0 ' + (18 + Math.sin(t * 1.8) * 8) + 'px color-mix(in srgb, var(--accent) 22%, transparent)';`
  })
];
