import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { build } from 'esbuild';
import { HtmlFrameRenderer } from '../packages/server/html-renderer.mjs';
import { findChromium } from '../packages/server/chromium.mjs';
import { ticks, seconds, validateHtmlContent } from '../packages/core/project.mjs';

const bundle = await build({
  entryPoints: ['src/editor/html-presentation-presets.ts'],
  bundle: true,
  platform: 'node',
  format: 'esm',
  write: false,
  logLevel: 'silent'
});
const { PRESENTATION_PRESETS } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64')
);
const overlays = await build({
  entryPoints: ['src/editor/html-overlay-presets.ts'],
  bundle: true,
  platform: 'node',
  format: 'esm',
  write: false,
  logLevel: 'silent'
});
const { OVERLAY_PRESETS } = await import(
  'data:text/javascript;base64,' + Buffer.from(overlays.outputFiles[0].text).toString('base64')
);
const PRESETS = [...PRESENTATION_PRESETS, ...OVERLAY_PRESETS];
const localized = await build({
  entryPoints: ['src/editor/localize-html-preset.ts'],
  bundle: true,
  platform: 'node',
  format: 'esm',
  write: false,
  logLevel: 'silent'
});
const { localizeHtmlPreset } = await import(
  'data:text/javascript;base64,' + Buffer.from(localized.outputFiles[0].text).toString('base64')
);
const signature = (png) => createHash('sha256').update(png).digest('hex');
let chromium;
try {
  chromium = await findChromium();
} catch {}
const options = {
  skip: !chromium && 'Chrome/Chromium unavailable; template raster runtime was not tested',
  timeout: 60000
};

test('library covers ten distinct templates with localized editable content', () => {
  assert.equal(PRESETS.length, 10);
  assert.equal(new Set(PRESETS.map((preset) => preset.id)).size, 10);
  for (const preset of PRESETS) {
    validateHtmlContent(preset.html);
    const english = localizeHtmlPreset(preset.html, 'en');
    for (const [key, value] of Object.entries(english.variables)) {
      assert.ok(
        typeof value !== 'string' || !/\p{Script=Han}/u.test(value),
        `${preset.id}.${key}: English default content`
      );
    }
    assert.equal(english.width, 1920);
    assert.equal(english.height, 1080);
  }
});

test(
  'all ten templates animate and survive an exit followed by a backward seek',
  options,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    for (const preset of PRESETS) {
      validateHtmlContent(preset.html);
      const entrance = await renderer.capture(preset.html, ticks(0.25));
      const hold = await renderer.capture(preset.html, ticks(3));
      const page = [...renderer.pages.values()].at(-1);
      const layout = await renderer.evaluate(
        page.sessionId,
        `(() => {
      const title = document.querySelector('h1');
      const rect = title.getBoundingClientRect();
      return { title: title.textContent, left: rect.left, right: rect.right, top: rect.top, bottom: rect.bottom };
    })()`
      );
      assert.equal(layout.title, preset.html.variables.title, preset.id);
      assert.ok(
        layout.left >= 0 && layout.right <= 1920 && layout.top >= 0 && layout.bottom <= 1080,
        `${preset.id}: default title fits the stage`
      );
      const exit = await renderer.capture(preset.html, ticks(seconds(preset.html.duration) - 0.2));
      assert.notEqual(signature(entrance.png), signature(hold.png), `${preset.id}: entrance moves`);
      assert.notEqual(signature(exit.png), signature(hold.png), `${preset.id}: exit moves`);
      renderer.frames.clear();
      const backward = await renderer.capture(preset.html, ticks(0.25));
      assert.equal(backward.cached, false);
      assert.equal(
        signature(backward.png),
        signature(entrance.png),
        `${preset.id}: an uncached backward seek restores identical pixels`
      );
      t.diagnostic(
        `${preset.id}: entrance → hold → exit → entrance seconds, identical entrance raster`
      );
    }
  }
);

test(
  'English defaults fit and the practical fields appear in actual DOM content',
  options,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    for (const preset of PRESETS) {
      const content = localizeHtmlPreset(preset.html, 'en');
      await renderer.capture(content, ticks(3));
      const page = [...renderer.pages.values()].at(-1);
      const fields = await renderer.evaluate(
        page.sessionId,
        `Array.from(document.querySelectorAll('[data-variable]')).map(node => {
      const rect = node.getBoundingClientRect();
      return { key: node.dataset.variable, text: node.textContent, left: rect.left, right: rect.right, top: rect.top, bottom: rect.bottom };
    })`
      );
      for (const field of fields) {
        assert.equal(field.text, String(content.variables[field.key]), `${preset.id}.${field.key}`);
        assert.ok(
          field.left >= 0 && field.right <= 1920 && field.top >= 0 && field.bottom <= 1080,
          `${preset.id}.${field.key}: fits stage`
        );
      }
    }
  }
);

test(
  'chart values change geometry and transparent overlays leave the stage clear',
  options,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    const data = PRESETS.find((preset) => preset.id === 'aurora-data');
    const readChart = async (content) => {
      await renderer.capture(content, ticks(3));
      return renderer.evaluate(
        [...renderer.pages.values()].at(-1).sessionId,
        `({ path: document.querySelector('.curve').getAttribute('d'), endpoint: document.querySelector('.endpoint').getAttribute('cy'), labels: Array.from(document.querySelectorAll('.chart text')).map(node => node.textContent) })`
      );
    };
    const baseline = await readChart(data.html);
    const edited = structuredClone(data.html);
    Object.assign(edited.variables, {
      chartValueOne: 80,
      chartValueTwo: 60,
      chartValueThree: 40,
      chartValueFour: 20,
      chartLabelOne: 'A',
      chartLabelTwo: 'B',
      chartLabelThree: 'C',
      chartLabelFour: 'D'
    });
    const changed = await readChart(edited);
    assert.notEqual(changed.path, baseline.path);
    assert.equal(changed.endpoint, '222.5');
    assert.deepEqual(changed.labels, ['A', 'B', 'C', 'D']);
    for (const preset of OVERLAY_PRESETS) {
      await renderer.capture(preset.html, ticks(3));
      const backgrounds = await renderer.evaluate(
        [...renderer.pages.values()].at(-1).sessionId,
        `['html', 'body', '#stage'].map(selector => getComputedStyle(document.querySelector(selector)).backgroundColor)`
      );
      assert.deepEqual(backgrounds, Array(3).fill('rgba(0, 0, 0, 0)'), preset.id);
    }
  }
);

test(
  'template text, palette, typography and chart values drive the rendered frame',
  options,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    for (const preset of PRESETS) {
      const content = structuredClone(preset.html);
      Object.assign(content.variables, {
        title: 'A clearer story',
        subtitle: 'Made for your audience',
        color: '#123456',
        backgroundColor: '#f8f7f6',
        textColor: '#345678',
        fontFamily: 'Arial, sans-serif',
        fontSize: 76
      });
      if (preset.id === 'aurora-data') content.variables['metricValue'] = 42.7;
      await renderer.capture(content, ticks(3));
      const page = [...renderer.pages.values()].at(-1);
      const state = await renderer.evaluate(
        page.sessionId,
        `(() => {
      const title = document.querySelector('h1'), style = getComputedStyle(title);
      return {
        title: title.textContent, subtitle: document.querySelector('.subtitle').textContent,
        font: style.fontFamily, size: style.fontSize, color: style.color,
        background: getComputedStyle(document.querySelector('#stage')).backgroundColor,
        accent: getComputedStyle(document.documentElement).getPropertyValue('--accent'),
        metric: document.querySelector('#metric')?.textContent
      };
    })()`
      );
      assert.equal(state.title, content.variables.title, preset.id);
      assert.equal(state.subtitle, content.variables.subtitle, preset.id);
      assert.equal(state.font, 'Arial, sans-serif', preset.id);
      assert.equal(state.size, '76px', preset.id);
      assert.equal(state.color, 'rgb(52, 86, 120)', preset.id);
      assert.equal(
        state.background,
        preset.html.transparent ? 'rgba(0, 0, 0, 0)' : 'rgb(248, 247, 246)',
        preset.id
      );
      assert.equal(state.accent, '#123456', preset.id);
      if (preset.id === 'aurora-data') assert.equal(state.metric, '42.7');
    }
  }
);
