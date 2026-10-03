import { ticks } from '../../packages/core/project.mjs';
import type { HtmlPreset } from './html-presets';

interface PresentationDesign {
  duration?: number;
  transparent?: boolean;
  setup?: string;
  id: string;
  name: string;
  variables: Record<string, string | number | boolean>;
  css: string;
  body: string;
  motion: string;
  tick?: string;
}

export function presentation(design: PresentationDesign): HtmlPreset {
  return {
    id: design.id,
    name: design.name,
    html: {
      width: 1920,
      height: 1080,
      duration: ticks(design.duration ?? 8),
      transparent: design.transparent ?? false,
      variables: {
        fontFamily: '-apple-system, BlinkMacSystemFont, "PingFang SC", sans-serif',
        fontSize: 112,
        ...design.variables
      },
      html: `<!doctype html><html><head><meta charset="utf-8"><style>
* { box-sizing: border-box; }
html, body { margin: 0; width: 100%; height: 100%; overflow: hidden; }
body { background: ${design.transparent ? 'transparent' : 'var(--bg)'}; color: var(--ink); font-family: var(--font); -webkit-font-smoothing: antialiased; }
#stage { position: absolute; width: 1920px; height: 1080px; overflow: hidden; transform-origin: 0 0; background: ${design.transparent ? 'transparent' : 'var(--bg)'}; }
h1, h2, p { margin: 0; }
h1 { font-size: var(--title-size); font-weight: 750; line-height: 1.18; letter-spacing: -5px; white-space: pre-line; overflow-wrap: anywhere; }
.subtitle { font-size: 32px; line-height: 1.6; white-space: pre-line; overflow-wrap: anywhere; }
.eyebrow { font-size: 24px; font-weight: 600; letter-spacing: 5px; }
.meta { font-size: 22px; letter-spacing: 2px; }
.rule { transform-origin: left center; }
.footer { position: absolute; left: 112px; right: 112px; bottom: 66px; display: flex; justify-content: space-between; align-items: center; }
${design.css}
</style></head><body><main id="stage">${design.body}</main><script>
const v = window.variables || window.__videocutVariables || {};
const root = document.documentElement;
root.style.setProperty('--font', v.fontFamily);
root.style.setProperty('--title-size', Math.max(40, Math.min(160, Number(v.fontSize) || 112)) + 'px');
root.style.setProperty('--accent', v.color);
root.style.setProperty('--bg', v.backgroundColor);
root.style.setProperty('--ink', v.textColor);
document.querySelectorAll('[data-variable]').forEach(node => {
  const key = node.dataset.variable;
  if (v[key] !== undefined) node.textContent = String(v[key]);
});
const stage = document.querySelector('#stage');
stage.style.transform = 'scale(' + (innerWidth / 1920) + ')';
${design.setup || ''}
gsap.defaults({ force3D: false });
const tl = gsap.timeline({ paused: true });
${design.motion}
tl.to('#stage', { opacity: 0, y: -16, duration: 0.55, ease: 'power2.in' }, ${(design.duration ?? 8) - 0.6});
window.__timelines = { main: tl };
window.tick = function(t) { ${design.tick || ''} };
</script></body></html>`
    }
  };
}
