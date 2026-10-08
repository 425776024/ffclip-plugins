import { createApp, defineComponent, h, ref, shallowRef, computed, nextTick } from 'vue';
import Inspector from '../src/editor/Inspector.vue';
import {
  createProject,
  addPagxClip,
  createPagxContent,
  CommandHistory,
  findItem,
  ticks,
  type EditorCommand
} from '../packages/core/project.mjs';
import { pagxFields } from '../packages/core/pagx.mjs';
import { SceneRenderer } from '../packages/render/renderer';
import '../src/editor/editor.css';
const xml =
  '<pagx width="320" height="180"><Layer name="色块" left="10" top="20" width="40" height="30"><Rectangle width="40" height="30" roundness="2"/><Fill color="#ff0000"/></Layer><Layer id="title" name="标题" left="70" top="20" width="230" height="50"><Text text="Title" fontSize="28" fontFamily="system"/><Fill color="#ffffff"/></Layer><Animations><Animation duration="60" frameRate="30" loop="once"><Object target="title"><Channel name="alpha" type="float"><Key time="0" value="0.5"/><Key time="60" value="1"/></Channel></Object></Animation></Animations></pagx>';
const project = shallowRef(createProject());
project.value.canvas = { width: 320, height: 180 };
const first = addPagxClip(project.value, { pagx: createPagxContent(xml), name: 'First' });
const second = addPagxClip(project.value, {
  pagx: createPagxContent(xml),
  name: 'Second',
  start: ticks(2),
  trackId: project.value.timeline.tracks[0].id
});
const selected = ref('');
const item = computed(() => (selected.value ? findItem(project.value, selected.value).item : null));
const history = new CommandHistory();
let commits = 0;
async function commit(op: EditorCommand) {
  await new Promise((r) => setTimeout(r, 30));
  const result = history.prepare(project.value, [op]);
  history.accept(result);
  project.value = result.project;
  commits++;
  await nextTick();
  return true;
}
createApp(
  defineComponent({
    setup() {
      return () =>
        h('div', { style: 'width:420px;height:100vh;display:flex;flex-direction:column' }, [
          h('button', { id: 'select-first', onClick: () => (selected.value = first.id) }, 'First'),
          h(
            'button',
            { id: 'select-second', onClick: () => (selected.value = second.id) },
            'Second'
          ),
          h(Inspector, {
            project: project.value,
            item: item.value,
            selection: selected.value ? [selected.value] : [],
            time: 0,
            disabled: false,
            commitContent: commit
          })
        ]);
    }
  })
).mount('#app');
const pause = (ms: number) => new Promise((r) => setTimeout(r, ms));
const check = (value: unknown, message: string) => {
  if (!value) throw Error(message);
};
function field(attr: string, group?: string) {
  const f = pagxFields(item.value!.clip.pagx!.xml).find(
    (f) => f.key.endsWith(':' + attr) && (!group || f.group === group)
  );
  check(f, 'Missing parsed field ' + attr);
  const el = document.querySelector<HTMLInputElement | HTMLTextAreaElement>(
    `[data-pagx-field="${f!.key}"] input:not([type=color]),[data-pagx-field="${f!.key}"] textarea`
  )!;
  check(el, 'Missing property control ' + attr);
  return el;
}
function input(el: HTMLInputElement | HTMLTextAreaElement, value: string) {
  el.value = value;
  el.dispatchEvent(new Event('input', { bubbles: true }));
}
(window as any).runProperties = async () => {
  const renderer = new SceneRenderer(new OffscreenCanvas(320, 180), () => '');
  try {
    document.querySelector<HTMLButtonElement>('#select-first')!.click();
    await nextTick();
    await nextTick();
    check(
      document.querySelector('.pagx-content-fields'),
      'Selecting PAGX must open its content panel'
    );
    await renderer.render(project.value, ticks(1), 320, 180);
    const before = await renderer.readPixels();
    input(field('color', '色块'), '#00cc66');
    input(field('fontSize', '标题'), '36');
    await pause(450);
    check(item.value!.clip.pagx!.xml.includes('#00cc66'), 'Color must commit');
    check(
      item.value!.clip.pagx!.xml.includes('fontSize="36"'),
      'Rapid property edits must not lose values'
    );
    await renderer.render(project.value, ticks(1), 320, 180);
    const after = await renderer.readPixels();
    const index = (30 * 320 + 20) * 4;
    check(
      before[index] > 200 && after[index + 1] > 150 && after[index] < 20,
      'Property color must change actual composed pixels'
    );
    const control = field('text', '标题');
    control.dispatchEvent(new CompositionEvent('compositionstart', { bubbles: true }));
    input(control, '输入中');
    const count = commits;
    await pause(380);
    check(commits === count, 'IME must not commit intermediate text');
    control.dispatchEvent(new CompositionEvent('compositionend', { bubbles: true }));
    await pause(430);
    check(item.value!.clip.pagx!.xml.includes('输入中'), 'IME completion must persist');
    await commit({ action: 'undo' });
    await nextTick();
    check(field('text', '标题').value === 'Title', 'Undo must update parsed controls');
    await commit({ action: 'redo' });
    await nextTick();
    check(field('text', '标题').value === '输入中', 'Redo must update parsed controls');
    input(field('fontFamily', '标题'), 'Arial');
    await pause(430);
    await renderer.render(project.value, ticks(1), 320, 180);
    await renderer.readPixels();
    check(
      item.value!.clip.pagx!.xml.includes('fontFamily="Arial"'),
      'Font family must commit and render'
    );
    input(field('text', '标题'), 'Switch flush');
    document.querySelector<HTMLButtonElement>('#select-second')!.click();
    await pause(430);
    check(
      findItem(project.value, first.id).item.clip.pagx!.xml.includes('Switch flush'),
      'Selection change must flush outgoing draft'
    );
    check(field('text', '标题').value === 'Title', 'Second clip must keep its own content');
    const duration = document.querySelector<HTMLInputElement>(
      'input[aria-label="PAGX 动画源时长"],input[aria-label="PAGX source duration"]'
    )!;
    input(duration, '0.2');
    await pause(430);
    check(
      document.querySelector('[role=alert]')?.textContent,
      'Invalid duration must show an error'
    );
    check(item.value!.clip.pagx!.duration === ticks(2), 'Invalid draft must not corrupt content');
    input(duration, '4');
    await pause(430);
    check(item.value!.clip.pagx!.duration === ticks(4), 'Valid duration must apply');
    check(
      item.value!.clip.pagx!.xml.includes('duration="120"'),
      'Duration edits must rescale internal timeline'
    );
    return {
      commits,
      project: project.value,
      pixelBefore: Array.from(before.slice(index, index + 4)),
      pixelAfter: Array.from(after.slice(index, index + 4)),
      verified: [
        'click',
        'color',
        'font',
        'size',
        'IME',
        'undo',
        'redo',
        'switch-flush',
        'validation',
        'duration'
      ]
    };
  } catch (error) {
    return { error: error instanceof Error ? error.stack : String(error) };
  } finally {
    renderer.dispose();
  }
};
