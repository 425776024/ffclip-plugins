import { presentation } from './html-preset-builder';
import type { HtmlPreset } from './html-presets';

export const OVERLAY_PRESETS: HtmlPreset[] = [
  presentation({
    id: 'lower-third',
    name: '人物介绍',
    duration: 6,
    transparent: true,
    variables: {
      title: '林悦',
      subtitle: '品牌主理人 · 创意分享',
      fontSize: 66,
      color: '#65f4d2',
      backgroundColor: '#101b2e',
      textColor: '#ffffff',
      eyebrow: 'CREATOR / SPOTLIGHT',
      monogram: 'LY',
      footer: '让好想法，被更多人看见'
    },
    css: `
.card { position: absolute; left: 110px; bottom: 108px; width: 1020px; min-height: 236px; padding: 30px 38px 29px 184px; border: 1px solid #c3fff438; border-radius: 23px; color: var(--ink); background: linear-gradient(120deg, color-mix(in srgb, var(--bg) 96%, transparent), color-mix(in srgb, var(--bg) 88%, transparent)); box-shadow: 0 18px 50px #0004; }
.accent-rail { position: absolute; left: 0; top: 32px; bottom: 32px; width: 5px; background: var(--accent); box-shadow: 0 0 18px color-mix(in srgb, var(--accent) 40%, transparent); transform-origin: center; }
.avatar { position: absolute; left: 36px; top: 58px; width: 110px; height: 110px; border-radius: 27px; background: linear-gradient(145deg, #ffffff18, color-mix(in srgb, var(--accent) 18%, transparent)); border: 1px solid #ffffff32; display: grid; place-items: center; font-size: 39px; letter-spacing: 2px; font-weight: 650; color: var(--accent); }
.eyebrow { font-size: 20px; color: var(--accent); letter-spacing: 4px; margin-bottom: 16px; }.card h1 { line-height: 1.05; letter-spacing: 0; }.subtitle { font-size: 28px; margin-top: 15px; color: #c6d9e6; }.caption { position: absolute; left: 146px; bottom: 65px; color: #f4f9ff; font-size: 22px; letter-spacing: 2px; text-shadow: 0 2px 10px #000a; }.caption:before { content: ''; display: inline-block; width: 24px; height: 2px; background: var(--accent); margin-right: 18px; vertical-align: middle; }
`,
    body: `<section class="card"><i class="accent-rail"></i><div class="avatar" data-variable="monogram"></div><div class="eyebrow" data-variable="eyebrow"></div><h1 data-variable="title"></h1><p class="subtitle" data-variable="subtitle"></p></section><div class="caption" data-variable="footer"></div>`,
    motion: `
tl.fromTo('.card', { x: -120, clipPath: 'inset(0 100% 0 0)', opacity: 0 }, { x: 0, clipPath: 'inset(0 0% 0 0)', opacity: 1, duration: .85, ease: 'power4.out' }, 0)
  .fromTo('.avatar', { scale: .6, rotation: -15, opacity: 0 }, { scale: 1, rotation: 0, opacity: 1, duration: .65, ease: 'back.out(1.5)' }, .2)
  .fromTo('.eyebrow, h1, .subtitle', { y: 24, opacity: 0 }, { y: 0, opacity: 1, duration: .55, stagger: .1, ease: 'power3.out' }, .3)
  .fromTo('.accent-rail', { scaleY: 0 }, { scaleY: 1, duration: .65, ease: 'expo.out' }, .3)
  .fromTo('.caption', { x: -30, opacity: 0 }, { x: 0, opacity: 1, duration: .7 }, .8);
`,
    tick: `document.querySelector('.avatar').style.boxShadow = '0 0 ' + (12 + Math.sin(t * 1.6) * 6) + 'px color-mix(in srgb, var(--accent) 14%, transparent)';`
  }),
  presentation({
    id: 'tick-title',
    name: '呼吸标题',
    duration: 6,
    transparent: true,
    variables: {
      title: '让创意动起来',
      subtitle: '你的下一段精彩，从这里开始',
      fontSize: 108,
      color: '#a3afff',
      backgroundColor: '#121b32',
      textColor: '#ffffff',
      eyebrow: 'CREATE IN MOTION',
      chapter: '01 / OPENING',
      footer: 'VIDEOCUT STUDIO'
    },
    css: `
.title-wrap { position: absolute; left: 180px; right: 180px; top: 304px; height: 445px; display: flex; flex-direction: column; justify-content: center; align-items: center; text-align: center; }
.title-surface { position: absolute; inset: 0; border-radius: 32px; background: radial-gradient(ellipse, color-mix(in srgb, var(--bg) 92%, transparent), color-mix(in srgb, var(--bg) 38%, transparent) 55%, transparent 75%); }
.title-content { position: relative; z-index: 1; width: 1400px; }.eyebrow { color: var(--accent); font-size: 24px; letter-spacing: 10px; margin-bottom: 32px; }h1 { font-weight: 850; line-height: 1.2; letter-spacing: 4px; text-shadow: 0 0 30px color-mix(in srgb, var(--accent) 50%, transparent), 0 5px 10px #0008; }.subtitle { color: #d3dafa; font-size: 29px; margin-top: 29px; text-shadow: 0 2px 8px #000a; }
.frame { position: absolute; inset: 0; width: 100%; height: 100%; overflow: visible; }.frame path { fill: none; stroke: var(--accent); stroke-width: 2; stroke-dasharray: 1000; filter: drop-shadow(0 0 8px var(--accent)); }
.line { position: relative; width: 268px; height: 4px; margin: 35px auto 0; background: linear-gradient(90deg, transparent, var(--accent), transparent); }.line:after { content: ''; position: absolute; left: 50%; top: -3px; width: 10px; height: 10px; border: 1px solid var(--accent); background: var(--bg); transform: rotate(45deg); }
.title-meta { position: absolute; left: 216px; right: 216px; top: 805px; display: flex; justify-content: space-between; color: var(--accent); font-size: 20px; letter-spacing: 3px; text-shadow: 0 2px 8px #000a; }
`,
    body: `<div class="title-wrap"><div class="title-surface"></div><svg class="frame" viewBox="0 0 1560 445"><path pathLength="1000" d="M54 0H0V84 M1506 0H1560V84 M0 361V445H54 M1560 361V445H1506"/></svg><section class="title-content"><div class="eyebrow" data-variable="eyebrow"></div><h1 data-variable="title"></h1><p class="subtitle" data-variable="subtitle"></p><div class="line"></div></section></div><div class="title-meta"><span data-variable="chapter"></span><span data-variable="footer"></span></div>`,
    motion: `
tl.fromTo('.title-wrap', { scale: .92, opacity: 0 }, { scale: 1, opacity: 1, duration: 1, ease: 'expo.out' }, 0)
  .fromTo('.eyebrow, h1, .subtitle', { y: 45, opacity: 0 }, { y: 0, opacity: 1, duration: .75, stagger: .13, ease: 'power3.out' }, .15)
  .fromTo('.line', { scaleX: 0 }, { scaleX: 1, duration: 1, ease: 'expo.out' }, .45)
  .fromTo('.title-meta', { opacity: 0 }, { opacity: 1, duration: .8 }, .85);
`,
    tick: `document.querySelector('.frame path').style.strokeDashoffset = String(1000 * (1 - Math.min(1, Math.max(0, t / 1.4)))); document.querySelector('.title-content').style.transform = 'scale(' + (1 + Math.sin(t * 1.4) * .012) + ')';`
  })
];
