import { createApp, nextTick } from 'vue';
import Editor from '../src/editor/Editor.vue';
import { followSystemLanguage } from '../src/editor/i18n';
import { VideoCutClient } from '../packages/client/index.mjs';
import '../src/editor/editor.css';
Object.defineProperty(navigator, 'languages', { configurable: true, value: ['zh-CN'] });
followSystemLanguage();
const id = new URLSearchParams(location.search).get('session')!;
createApp(Editor).mount('#app');
const wait = async (predicate: () => boolean, label: string) => {
  const deadline = performance.now() + 20000;
  while (!predicate()) {
    if (performance.now() > deadline) throw Error(label);
    await new Promise((r) => setTimeout(r, 50));
  }
};
const click = async (selector: string) => {
  const e = document.querySelector<HTMLElement>(selector);
  if (!e) throw Error('Missing ' + selector);
  e.click();
  await nextTick();
};
(window as any).runEditor = async () => {
  try {
    await wait(() => !!document.querySelector('.connection.online'), 'Editor connection');
    const client = new VideoCutClient(location.origin);
    await client.connect();
    await click('.rail-main button:nth-child(3)');
    if (!document.querySelector('.pagx-library')) throw Error('PAGX must be the default library');
    const count = () => document.querySelectorAll('[data-pagx-template]').length;
    if (count() !== 10 || document.querySelector('.animation-source-tabs'))
      throw Error('Native-only catalog');
    await Promise.all(
      [...document.querySelectorAll<HTMLImageElement>('.pagx-library img')].map((image) =>
        image.decode()
      )
    );
    Object.defineProperty(navigator, 'languages', { configurable: true, value: ['en-US'] });
    window.dispatchEvent(new Event('languagechange'));
    await nextTick();
    const posters = [...document.querySelectorAll<HTMLImageElement>('.pagx-library img')];
    if (posters.length !== 10 || posters.some((image) => !image.src.includes('-en.png')))
      throw Error('English poster source');
    await Promise.all(posters.map((image) => image.decode()));
    Object.defineProperty(navigator, 'languages', { configurable: true, value: ['zh-CN'] });
    window.dispatchEvent(new Event('languagechange'));
    await nextTick();
    await click('.pagx-library button[title="叠加动画"]');
    if (count() !== 2) throw Error('Overlay filter');
    await click('.pagx-library button[title="全部动画"]');
    const search = document.querySelector<HTMLInputElement>('.pagx-library input[type="search"]')!;
    search.value = '人物';
    search.dispatchEvent(new Event('input', { bubbles: true }));
    await nextTick();
    if (count() !== 1) throw Error('Template search');
    await click('.pagx-library .search-clear');
    await click('[data-pagx-template="lower-third"]');
    await wait(() => !!document.querySelector('.timeline-clip.pagx-clip'), 'Insert native PAGX');
    const clip = document.querySelector<HTMLElement>('.timeline-clip.pagx-clip')!;
    const r = clip.getBoundingClientRect();
    clip.dispatchEvent(
      new PointerEvent('pointerdown', {
        bubbles: true,
        pointerId: 7,
        button: 0,
        clientX: r.x + 30,
        clientY: r.y + 12
      })
    );
    window.dispatchEvent(
      new PointerEvent('pointerup', {
        bubbles: true,
        pointerId: 7,
        button: 0,
        clientX: r.x + 30,
        clientY: r.y + 12
      })
    );
    await wait(
      () => !!document.querySelector('.pagx-content-fields textarea'),
      'PAGX selection opens parsed settings'
    );
    const field = document.querySelector<HTMLTextAreaElement>('.pagx-content-fields textarea')!;
    field.value = '编辑器中修改';
    field.dispatchEvent(new Event('input', { bubbles: true }));
    await new Promise((r) => setTimeout(r, 700));
    const snapshot = await client.getSession(id);
    if (
      !snapshot.project.timeline.tracks
        .flatMap((t) => t.items)
        .some((i) => i.clip.pagx?.xml.includes('编辑器中修改'))
    )
      throw Error('Inspector save missing');
    for (const [templateId, name, content] of [
      ['prism-launch', '出格开场', '新意'],
      ['swiss-story', '大字切片', '简'],
      ['aurora-data', '冲刺数字', '增长加速'],
      ['hologram-product', '节拍上新', '热浪']
    ]) {
      await click(`[data-pagx-template="${templateId}"]`);
      const selector = `.timeline-clip.pagx-clip[title^="${name}"]`;
      await wait(() => !!document.querySelector(selector), `Insert ${name}`);
      const target = document.querySelector<HTMLElement>(selector)!;
      const bounds = target.getBoundingClientRect();
      const pointer = {
        bubbles: true,
        pointerId: 8,
        button: 0,
        clientX: bounds.x + 30,
        clientY: bounds.y + 12
      };
      target.dispatchEvent(new PointerEvent('pointerdown', pointer));
      window.dispatchEvent(new PointerEvent('pointerup', pointer));
      await wait(
        () =>
          document.querySelector<HTMLInputElement>('[aria-label="PAGX 片段名称"]')?.value === name,
        `Select ${name}`
      );
      const heading = document.querySelector<HTMLTextAreaElement>('[aria-label="主标题 · 文字"]')!;
      if (document.querySelector('.pagx-content-fields textarea') !== heading)
        throw Error('Main editable title must come first');
      heading.value = content;
      heading.dispatchEvent(new Event('input', { bubbles: true }));
      const color = document.querySelector<HTMLInputElement>('[aria-label="主标题 · 选择颜色"]')!;
      color.value = '#ff00a8';
      color.dispatchEvent(new Event('input', { bubbles: true }));
      await new Promise((r) => setTimeout(r, 700));
      const edited = (await client.getSession(id)).project.timeline.tracks
        .flatMap((t) => t.items)
        .find((item) => item.name === name);
      if (!edited?.clip.pagx?.xml.includes(content) || !edited.clip.pagx.xml.includes('#ff00a8'))
        throw Error(`${name} headline/color edit missing`);
    }
    const after = await client.getSession(id);
    return {
      passed: true,
      types: after.project.timeline.tracks.flatMap((t) => t.items).map((i) => i.clip.type),
      rows: [
        'Ten native templates, native-only catalog, search and category filters',
        'Native template insertion',
        'Real timeline click opens parsed inspector',
        'Inspector edit persisted through API',
        'All four kinetic templates open their main title first and persist title/color edits'
      ]
    };
  } catch (error) {
    return { error: error instanceof Error ? error.stack : String(error) };
  }
};
