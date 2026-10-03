import { createApp, nextTick } from 'vue';
import Editor from '../src/editor/Editor.vue';
import '../src/editor/editor.css';
import { followSystemLanguage } from '../src/editor/i18n';
import { browserLocale } from '../src/editor/i18n/runtime.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';

// Test-only navigator preferences simulate a computer language change without
// changing the user's OS/browser settings. The actual Editor is mounted below.
const initialLanguage = browserLocale();
const originalLanguages = Object.getOwnPropertyDescriptor(navigator, 'languages');
const stop = followSystemLanguage();
const app = createApp(Editor);
app.onUnmount(stop);
app.mount('#app');
const report = document.querySelector<HTMLPreElement>('#i18n-status')!;
const run = document.querySelector<HTMLButtonElement>('#run-i18n')!;
function systemLanguage(language: string) {
  Object.defineProperty(navigator, 'languages', { configurable: true, value: [language] });
  window.dispatchEvent(new Event('languagechange'));
}
document.querySelector<HTMLButtonElement>('#chinese')!.onclick = () => systemLanguage('zh-CN');
document.querySelector<HTMLButtonElement>('#english')!.onclick = () => systemLanguage('en-US');
const wait = async (predicate: () => boolean, label: string) => {
  const deadline = performance.now() + 15000;
  while (!predicate()) {
    if (performance.now() > deadline) throw new Error(label);
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
};
const assert = (condition: unknown, label: string) => {
  if (!condition) throw new Error(label);
};
const click = async (selector: string) => {
  const element = document.querySelector<HTMLButtonElement>(selector);
  assert(element, `Missing control: ${selector}`);
  element!.click();
  await nextTick();
};
run.onclick = async () => {
  run.disabled = true;
  const rows: string[] = [];
  try {
    await wait(() => !!document.querySelector('.connection.online'), 'Editor did not connect');
    const client = new VideoCutClient(location.origin);
    const before = await client.resolveSession(location.pathname);
    for (const [language, expected, exportLabel, canvasLabel, mediaLabel] of [
      ['zh-TW', 'zh-CN', '导出', '画布设置', '素材'],
      ['en-US', 'en', 'Export', 'Canvas settings', 'Media'],
      ['zh-CN', 'zh-CN', '导出', '画布设置', '素材']
    ]) {
      systemLanguage(language);
      await nextTick();
      assert(document.documentElement.lang === expected, `Document language: ${language}`);
      assert(
        document.querySelector('.export-button')?.textContent?.trim() === exportLabel,
        `Export label: ${language}`
      );
      assert(
        document.querySelector('.rail-main button')?.textContent?.trim() === mediaLabel,
        `Media label: ${language}`
      );
      assert(
        !!document.querySelector(`[aria-label="${canvasLabel}"]`),
        `Accessible canvas label: ${language}`
      );
      rows.push(`${language}: reactive controls, tooltips and accessibility labels passed`);
    }
    systemLanguage('en-US');
    await nextTick();
    const clip = document.querySelector<HTMLElement>('.timeline-clip')!;
    clip.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, pointerId: 7, button: 0, clientX: 250, clientY: 500 }));
    window.dispatchEvent(new PointerEvent('pointerup', { bubbles: true, pointerId: 7, button: 0, clientX: 250, clientY: 500 }));
    await nextTick();
    await click('.inspector-tabs button:first-child');
    const textField = document.querySelector<HTMLTextAreaElement>('[aria-label="Text content"]');
    assert(
      textField?.value === '保留中文字幕 / Keep my words',
      'Inspector translated or lost authored text'
    );
    rows.push('Inspector labels switched to English while authored text stayed unchanged');
    await click('.rail-main button:nth-child(2)');
    assert(
      document.querySelector('.panel-title')?.textContent === 'Text templates',
      'Text library title'
    );
    assert(
      !!document.querySelector('[aria-label="Add template: Pattern · Glow entrance"]'),
      'Translated template accessibility label'
    );
    await click('.library-categories button:last-child');
    assert(document.querySelector('.title-preset')?.textContent === 'Title', 'Basic text preset');
    await click('.rail-main button:nth-child(3)');
    assert(
      document.querySelector('.panel-title')?.textContent === 'Animation templates',
      'Animation library'
    );
    assert(
      !!document.querySelector('[aria-label="Add animation: Prism launch"]'),
      'Animation accessibility label'
    );
    await click('.rail-main button:nth-child(4)');
    await wait(() => !!document.querySelector('.tts-heading'), 'TTS panel did not render');
    assert(
      document.querySelector('.tts-heading strong')?.textContent === 'Local speech synthesis',
      'Speech synthesis label'
    );
    assert(!!document.querySelector('[aria-label="Audio tools"]'), 'Audio accessibility label');
    await click('.export-button');
    assert(
      !!document.querySelector('[aria-label="Export folder"]'),
      'Export folder accessibility label'
    );
    await click('.export-button');
    await click('.rail-bottom button');
    assert(
      document.querySelector('.help-dialog strong')?.textContent === 'Keyboard shortcuts',
      'Help label'
    );
    await click('[aria-label="Close help"]');
    const after = await client.getSession(before.id);
    assert(
      after.version === before.version &&
        JSON.stringify(after.project) === JSON.stringify(before.project),
      'Language switches modified authored content'
    );
    rows.push('Text, animation, TTS, export and help panels passed');
    rows.push('Project name, text clips and serialized content stayed identical');
    report.textContent = JSON.stringify(
      {
        passed: true,
        initialLanguage,
        rows,
        evidence:
          'Real Vue Editor in browser; synthetic navigator preferences and UI events; OS settings unchanged'
      },
      null,
      2
    );
    await click('.rail-main button:first-child');
  } catch (error) {
    report.textContent = JSON.stringify(
      { passed: false, rows, error: error instanceof Error ? error.message : String(error) },
      null,
      2
    );
  } finally {
    run.disabled = false;
  }
};
window.addEventListener(
  'pagehide',
  () => {
    if (originalLanguages) Object.defineProperty(navigator, 'languages', originalLanguages);
    else delete (navigator as unknown as Record<string, unknown>).languages;
  },
  { once: true }
);
