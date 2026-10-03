import { createApp, defineComponent, h, nextTick, ref, shallowRef } from 'vue';
import Inspector from '../src/editor/Inspector.vue';
import '../src/editor/editor.css';
import {
  addHtmlClip,
  createProject,
  findItem,
  CommandHistory,
  ticks,
  type EditorCommand
} from '../packages/core/project.mjs';
import { HtmlFrameClient } from '../packages/render/html';

const status = document.querySelector<HTMLPreElement>('#status')!;
const run = document.querySelector<HTMLButtonElement>('#run')!;
let app: ReturnType<typeof createApp> | undefined;
const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
const check = (value: unknown, message: string) => {
  if (!value) throw new Error(message);
};
async function wait(predicate: () => boolean, message: string) {
  const end = performance.now() + 15000;
  while (!predicate()) {
    if (performance.now() > end) throw new Error(message);
    await sleep(25);
  }
}
function input(selector: string, value: string, type = 'input') {
  const node = document.querySelector<HTMLInputElement | HTMLTextAreaElement>(selector)!;
  check(node, `Missing control ${selector}`);
  node.value = value;
  node.dispatchEvent(new Event(type, { bubbles: true }));
  return node;
}

run.onclick = async () => {
  run.disabled = true;
  app?.unmount();
  const rows: { name: string; passed: boolean }[] = [];
  const report = {
    complete: false,
    eventSource: 'Synthetic DOM events on the real Inspector; Chromium HTML raster pixels',
    rows,
    error: ''
  };
  const show = () => {
    status.textContent = JSON.stringify(report, null, 2);
  };
  const record = async (name: string, test: () => Promise<void>) => {
    await test();
    rows.push({ name, passed: true });
    show();
  };
  show();
  const initial = createProject('HTML properties fixture');
  initial.canvas = { width: 320, height: 180 };
  const script = '<script>window.tick = t => { document.body.dataset.time = t; };</script>';
  const source =
    '<style>html,body{margin:0;background:transparent}#shape{width:160px;height:100px;background:rgba(36,104,172,0.5);font-size:24px;font-family:Arial}</style><div id="shape">Original title</div>' +
    script;
  const first = addHtmlClip(initial, {
    html: {
      html: source,
      width: 320,
      height: 180,
      duration: ticks(6),
      transparent: true,
      variables: { accentColor: 'red', enabled: true }
    }
  });
  const second = addHtmlClip(initial, {
    html: { ...first.clip.html!, variables: { accentColor: 'blue', enabled: false } },
    start: ticks(6)
  });
  const project = shallowRef(initial),
    selected = ref(first.id),
    disabled = ref(false);
  const history = new CommandHistory();
  let commits = 0,
    latency = 30,
    failNext = false;
  let tail: Promise<unknown> = Promise.resolve();
  function commit(command: EditorCommand): Promise<boolean> {
    const result = tail.then(async () => {
      await sleep(latency);
      if (failNext) {
        failNext = false;
        return false;
      }
      const candidate = history.prepare(project.value, [command]);
      history.accept(candidate);
      project.value = candidate.project;
      commits++;
      await nextTick();
      return true;
    });
    tail = result.catch(() => false);
    return result;
  }
  const current = () => findItem(project.value, first.id).item;
  const Root = defineComponent({
    setup: () => () =>
      h(Inspector, {
        project: project.value,
        item: findItem(project.value, selected.value).item,
        selection: [selected.value],
        time: 0,
        disabled: disabled.value,
        commitHtml: commit
      })
  });
  app = createApp(Root);
  app.mount('#fixture');
  await nextTick();
  document.querySelector<HTMLButtonElement>('.inspector-tabs button:first-child')!.click();
  await nextTick();
  const shapeStyle = '.html-property-field:has(#html-property-4)';
  const styleColor = `${shapeStyle} .html-color-picker`;
  const styleText = `${shapeStyle} .html-color-input input[type="text"]`;
  const styleAlpha = `${shapeStyle} input[type="range"]`;
  const text = '.html-property-field textarea';
  const renderer = new HtmlFrameClient();
  try {
    await record('Picker accepts named, shorthand hex, HSL and alpha CSS colors', async () => {
      for (const [value, expected] of [
        ['red', '#ff0000'],
        ['#0f08', '#00ff00'],
        ['hsl(240 100% 50% / 50%)', '#0000ff'],
        ['rgba(36,104,172,0.5)', '#2468ac']
      ]) {
        input(styleText, value);
        await nextTick();
        check(
          document.querySelector<HTMLInputElement>(styleColor)!.value === expected,
          `Picker conversion: ${value}`
        );
      }
      await sleep(400);
      check(commits === 0, 'Unchanged final color produced a command');
    });
    await record(
      'Color input automatically updates authored HTML and preserves alpha and scripts',
      async () => {
        input(styleColor, '#00cc66');
        await wait(() => commits === 1, 'Color did not auto apply');
        check(current().clip.html!.html.includes('rgba(0, 204, 102, 0.5)'), 'RGB edit lost alpha');
        check(current().clip.html!.html.includes(script), 'Script changed');
        const frame = await renderer.frame(first.id, current().clip.html!, 0);
        try {
          const canvas = document.querySelector<HTMLCanvasElement>('#preview')!,
            ctx = canvas.getContext('2d')!;
          ctx.clearRect(0, 0, 320, 180);
          ctx.drawImage(frame.frame, 0, 0);
          const pixel = ctx.getImageData(80, 80, 1, 1).data;
          check(
            Math.abs(pixel[1] - 204) <= 1 && Math.abs(pixel[2] - 102) <= 1 && pixel[3] === 128,
            `Unexpected rendered RGBA: ${pixel}`
          );
        } finally {
          frame.close();
        }
      }
    );
    await record('Opacity slider and template variable color auto apply', async () => {
      const before = commits;
      input(styleAlpha, '25');
      input('.html-property-group[aria-label="动画参数"] .html-color-picker', '#abcdef');
      await wait(() => commits > before, 'Alpha and variable did not apply');
      check(current().clip.html!.html.includes('rgba(0, 204, 102, 0.25)'), 'Opacity not saved');
      check(current().clip.html!.variables!.accentColor === '#abcdef', 'Variable color not saved');
    });
    await record(
      'Text, dimensions, name, background and checkbox edits coalesce into one undoable command',
      async () => {
        const before = commits;
        input(text, 'An expanded title & <safe>');
        input('#html-property-5', '36');
        input('#html-property-6', 'Helvetica, sans-serif');
        input('[aria-label="HTML 动画宽度"]', '640');
        input('[aria-label="HTML 动画高度"]', '360');
        input('[aria-label="HTML 动画源时长"]', '8');
        input('[aria-label="HTML 片段名称"]', 'New animation');
        const checkbox = document.querySelector<HTMLInputElement>('.html-transparent input')!;
        checkbox.checked = false;
        checkbox.dispatchEvent(new Event('change', { bubbles: true }));
        const enabled = document.querySelector<HTMLInputElement>(
          '.html-property-group input[type="checkbox"]'
        )!;
        enabled.checked = false;
        enabled.dispatchEvent(new Event('change', { bubbles: true }));
        await wait(() => commits > before, 'Metadata did not apply');
        await sleep(400);
        check(commits === before + 1, 'Rapid edits created redundant commands');
        const saved = current();
        check(
          saved.name === 'New animation' &&
            saved.clip.html!.width === 640 &&
            saved.clip.html!.height === 360 &&
            saved.clip.html!.duration === ticks(8),
          'Metadata not saved'
        );
        check(
          !saved.clip.html!.transparent && saved.clip.html!.variables!.enabled === false,
          'Checkboxes not saved'
        );
        check(
          saved.clip.html!.html.includes('An expanded title &amp; &lt;safe&gt;'),
          'Text not escaped'
        );
        check(
          saved.clip.html!.html.includes('font-size:36px;font-family:Helvetica, sans-serif'),
          'Numeric units or font edits not saved'
        );
        await commit({ action: 'undo' });
        check(
          current().name === first.name && current().clip.html!.width === 320,
          'Undo did not restore edit'
        );
        await commit({ action: 'redo' });
        check(current().name === 'New animation', 'Redo did not restore edit');
      }
    );
    await record(
      'Slow save acknowledgements retain newer input and correct source ranges',
      async () => {
        latency = 650;
        const before = commits;
        input(text, 'First long save');
        await sleep(360);
        input(text, 'Latest content typed during save');
        input(styleColor, '#112233');
        await wait(() => commits >= before + 2, 'Newer draft was lost during save');
        check(
          current().clip.html!.html.includes('Latest content typed during save') &&
            current().clip.html!.html.includes('rgba(17, 34, 51, 0.25)'),
          'Newer values or source offsets were lost'
        );
        latency = 30;
      }
    );
    await record('Chinese composition waits until the input method commits', async () => {
      const node = document.querySelector<HTMLTextAreaElement>(text)!,
        before = commits;
      node.dispatchEvent(new CompositionEvent('compositionstart', { bubbles: true }));
      input(text, '组词中的内容');
      await sleep(450);
      check(commits === before, 'Composition was committed prematurely');
      node.dispatchEvent(new CompositionEvent('compositionend', { bubbles: true }));
      await wait(() => commits > before, 'Final Chinese text did not apply');
    });
    await record(
      'Invalid CSS and short source duration stay local and recover after correction',
      async () => {
        const before = commits;
        input(styleText, 'invalid-color');
        await sleep(400);
        check(
          commits === before && document.querySelector('[role="alert"]'),
          'Invalid color was committed or missing feedback'
        );
        input(styleText, '#445566');
        await wait(() => commits > before, 'Valid correction did not save');
        const next = commits;
        input('[aria-label="HTML 动画源时长"]', '1');
        await sleep(400);
        check(
          commits === next && document.querySelector('[role="alert"]'),
          'Short source duration committed'
        );
        input('[aria-label="HTML 动画源时长"]', '8');
        await sleep(400);
        check(
          !document.querySelector('[role="alert"]'),
          'Unchanged valid correction left an error'
        );
      }
    );
    await record('Failed saves can be retried without losing the draft', async () => {
      failNext = true;
      const before = commits;
      input(text, 'Retained after failed save');
      await wait(() => !!document.querySelector('[role="alert"]'), 'Failed save missing error');
      check(
        commits === before &&
          document.querySelector<HTMLTextAreaElement>(text)!.value === 'Retained after failed save',
        'Failed save lost draft'
      );
      document.querySelector<HTMLButtonElement>('[role="alert"] button')!.click();
      await wait(() => commits > before, 'Retry did not commit');
    });
    await record('Switching clips flushes a pending edit to its original clip', async () => {
      input(text, 'Saved before switching');
      selected.value = second.id;
      await nextTick();
      await wait(
        () => current().clip.html!.html.includes('Saved before switching'),
        'Selection change discarded pending edit'
      );
      check(
        findItem(project.value, second.id).item.clip.html!.html === source,
        'Edit leaked into the next clip'
      );
      selected.value = first.id;
      await nextTick();
      latency = 650;
      input(text, 'Save in flight');
      await sleep(360);
      input(text, 'Latest edit before switching');
      selected.value = second.id;
      await nextTick();
      await wait(
        () => current().clip.html!.html.includes('Latest edit before switching'),
        'In-flight selection change discarded newer edit'
      );
      check(
        findItem(project.value, second.id).item.clip.html!.html === source,
        'In-flight save leaked into the next clip'
      );
      latency = 30;
    });
    await record('Disabled inspector cannot automatically commit changes', async () => {
      disabled.value = true;
      await nextTick();
      const before = commits;
      input(text, 'Blocked synthetic edit');
      await sleep(400);
      check(commits === before, 'Disabled inspector submitted a change');
      disabled.value = false;
    });
    report.complete = true;
  } catch (error) {
    report.error = error instanceof Error ? error.message : String(error);
  } finally {
    renderer.dispose();
    show();
    run.disabled = false;
  }
};
if (new URLSearchParams(location.search).has('autorun')) run.click();
