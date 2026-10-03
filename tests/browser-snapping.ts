import { createApp, defineComponent, h, shallowRef, ref, nextTick } from 'vue';
import Preview from '../src/editor/Preview.vue';
import Timeline from '../src/editor/Timeline.vue';
import '../src/editor/editor.css';
import {
  createProject,
  addText,
  editTimeline,
  findItem,
  ticks,
  seconds,
  CommandHistory,
  type Project,
  type EditorCommand,
  type Item
} from '../packages/core/project.mjs';

const baseline = createProject('Snapping regression');
baseline.canvas = { width: 640, height: 360 };
const item = addText(baseline, {
  content: '吸附测试',
  fontSize: 40,
  start: ticks(2),
  length: ticks(2)
});
item.clip.visual.positionX = 80;
item.clip.visual.positionY = 30;
const target = addText(baseline, { content: '对齐目标', start: ticks(6), length: ticks(2) });
addText(baseline, { content: '多选对齐目标', start: ticks(10), length: ticks(2) });
const project = shallowRef<Project>(structuredClone(baseline)),
  time = ref(ticks(3));
const zoom = ref(80),
  snap = ref(true),
  selection = ref([item.id]),
  version = ref(0);
let commands: EditorCommand[] = [],
  commits = 0;
const history = new CommandHistory(),
  errors: string[] = [];
function apply(operations: EditorCommand[]) {
  const prepared = history.prepare(project.value, operations);
  history.accept(prepared);
  if (prepared.patches.length) {
    project.value = prepared.project;
    version.value++;
  }
}
function reset() {
  commands = [];
  project.value = structuredClone(baseline);
  time.value = ticks(3);
  zoom.value = 80;
  snap.value = true;
  selection.value = [item.id];
}
createApp(
  defineComponent({
    setup() {
      return () =>
        h('div', [
          h('label', [
            h('input', {
              type: 'checkbox',
              checked: snap.value,
              'aria-label': '启用吸附',
              onChange: (e: Event) => {
                snap.value = (e.target as HTMLInputElement).checked;
              }
            }),
            ' 启用吸附'
          ]),
          h(
            'output',
            { id: 'values' },
            `卡针 ${seconds(time.value).toFixed(6)}s · X ${findItem(project.value, item.id).item.clip.visual.positionX.toFixed(6)} · Y ${findItem(project.value, item.id).item.clip.visual.positionY.toFixed(6)} · 版本 ${version.value}`
          ),
          h(Preview, {
            project: project.value,
            time: time.value,
            playing: false,
            selected: item.id,
            disabled: false,
            snap: snap.value,
            quality: 'half',
            mediaUrl: (id: string) => `/unused/${id}`,
            onError: (message: string) => errors.push(message),
            onTransform: (
              _id: string,
              changes: Partial<Item['clip']['visual']>,
              settle: () => void
            ) => {
              apply([{ action: 'set_transform', itemId: item.id, ...changes }]);
              void nextTick().then(settle);
            }
          }),
          h(Timeline, {
            project: project.value,
            time: time.value,
            selected: item.id,
            selection: selection.value,
            zoom: zoom.value,
            snap: snap.value,
            busy: false,
            mediaUrl: (id: string) => `/unused/${id}`,
            onSeek: (value: number) => {
              time.value = value;
            },
            onDraft: (value: EditorCommand[]) => {
              commands = value;
            },
            onCancel: () => {
              commands = [];
            },
            onCommit: () => {
              if (commands.length) {
                commits++;
                apply(commands);
                commands = [];
              }
            }
          })
        ]);
    }
  })
).mount('#fixture');

const assert = (condition: unknown, message: string) => {
  if (!condition) throw new Error(message);
};
const wait = async (predicate: () => boolean, message: string) => {
  const deadline = performance.now() + 20000;
  while (!predicate()) {
    if (performance.now() > deadline) throw new Error(`${message}; ${errors.join('; ')}`);
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
};
const pointer = (node: EventTarget, type: string, x: number, y: number, ctrlKey = false) =>
  node.dispatchEvent(
    new PointerEvent(type, {
      bubbles: true,
      cancelable: true,
      pointerId: 1,
      pointerType: 'mouse',
      isPrimary: true,
      button: 0,
      buttons: type === 'pointerup' ? 0 : 1,
      clientX: x,
      clientY: y,
      ctrlKey
    })
  );
const ruler = () => document.querySelector<HTMLElement>('.ruler')!;
const guide = () => document.querySelector<HTMLElement>('.timeline-snap-guide');
const previewGuide = () => document.querySelector<HTMLElement>('.preview-snap-guide.vertical');
const hit = () => document.querySelector<HTMLElement>('.unified-preview-hit.selected')!;
const clip = () => document.querySelector<HTMLElement>('.timeline-clip[title^="吸附测试"]')!;
window.addEventListener('pointerup', (event) => {
  if (event.isTrusted)
    document.querySelector('#trusted')!.textContent =
      `可信指针松手（isTrusted=true）：${event.clientX}, ${event.clientY}`;
});
function start(node: HTMLElement) {
  const rect = node.getBoundingClientRect(),
    x = rect.left + rect.width / 2,
    y = rect.top + rect.height / 2;
  pointer(node, 'pointerdown', x, y);
  return {
    x,
    y,
    move: (dx: number, dy = 0, ctrl = false) =>
      pointer(window, 'pointermove', x + dx, y + dy, ctrl),
    end: (dx: number, dy = 0, ctrl = false) => pointer(window, 'pointerup', x + dx, y + dy, ctrl)
  };
}
function seekAt(value: number, offset: number, type = 'pointerdown', ctrl = false) {
  const rect = ruler().getBoundingClientRect();
  pointer(
    type === 'pointerdown' ? ruler() : window,
    type,
    rect.left + value * zoom.value + offset,
    rect.top + 15,
    ctrl
  );
}
document.querySelector<HTMLButtonElement>('#reset')!.onclick = reset;
document.querySelector<HTMLButtonElement>('#run')!.onclick = async () => {
  const button = document.querySelector<HTMLButtonElement>('#run')!;
  button.disabled = true;
  const rows: unknown[] = [],
    report = {
      eventSource: 'Synthetic PointerEvent, isTrusted=false',
      complete: false,
      rows,
      errors
    };
  const show = () => {
    document.querySelector('#status')!.textContent = JSON.stringify(report, null, 2);
  };
  const record = async (name: string, test: () => Promise<unknown>) => {
    reset();
    await nextTick();
    await wait(
      () =>
        document.querySelector('.preview-stage')?.getAttribute('data-preview-current') === 'true',
      'Reset frame did not settle'
    );
    rows.push({ name, passed: true, details: await test() });
    show();
  };
  try {
    await wait(() => !!hit(), 'Preview did not render');
    await record(
      'Playhead captures, holds and releases clip edges at 15/80/220px per second',
      async () => {
        for (const value of [15, 80, 220]) {
          zoom.value = value;
          await nextTick();
          seekAt(4, 4);
          await nextTick();
          assert(
            time.value === ticks(4) && guide()?.dataset.snapTime === String(ticks(4)),
            `Capture failed at zoom ${value}`
          );
          seekAt(4, 7, 'pointermove');
          await nextTick();
          assert(time.value === ticks(4), 'Sticky capture jittered');
          window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Control', bubbles: true }));
          await nextTick();
          assert(time.value !== ticks(4) && !guide(), 'Stationary Ctrl did not release playhead');
          window.dispatchEvent(new KeyboardEvent('keyup', { key: 'Control', bubbles: true }));
          // A cleared latch reacquires only within the original capture distance.
          seekAt(4, 4, 'pointermove');
          await nextTick();
          seekAt(4, 9, 'pointermove');
          await nextTick();
          assert(time.value !== ticks(4) && !guide(), 'Did not release capture');
          seekAt(4, 9, 'pointerup');
          await nextTick();
          assert(!guide(), 'Guide remained after release');
        }
        return { zooms: [15, 80, 220], capturePx: 5, releasePx: 8 };
      }
    );
    await record('Ctrl and the snap switch bypass playhead attraction', async () => {
      seekAt(4, 3, 'pointerdown', true);
      await nextTick();
      assert(time.value !== ticks(4) && !guide(), 'Ctrl did not bypass');
      seekAt(4, 3, 'pointerup', true);
      snap.value = false;
      await nextTick();
      seekAt(4, 3);
      await nextTick();
      assert(time.value !== ticks(4) && !guide(), 'Switch did not bypass');
      seekAt(4, 3, 'pointerup');
      return { time: time.value };
    });
    await record(
      'Moving clip snaps its trailing edge and commits once; undo restores it',
      async () => {
        const before = commits,
          motion = start(clip());
        motion.move(164);
        await nextTick();
        assert(guide()?.dataset.snapTime === String(ticks(6)), 'Trailing edge guide missing');
        motion.end(164);
        await nextTick();
        assert(
          findItem(project.value, item.id).item.placement.end === ticks(6),
          'Trailing edge not authored exactly'
        );
        assert(commits === before + 1 && !guide(), 'Release did not produce one clean commit');
        apply([{ action: 'undo' }]);
        await nextTick();
        assert(findItem(project.value, item.id).item.placement.begin === ticks(2), 'Undo failed');
        return { commits: commits - before };
      }
    );
    await record('Multi-selection snaps an edge of another selected clip', async () => {
      selection.value = [item.id, target.id];
      await nextTick();
      const motion = start(clip());
      motion.move(164);
      await nextTick();
      assert(guide()?.dataset.snapTime === String(ticks(10)), 'Group probe guide missing');
      motion.end(164);
      await nextTick();
      assert(
        findItem(project.value, target.id).item.placement.end === ticks(10),
        'Group edge did not align'
      );
      assert(
        findItem(project.value, item.id).item.placement.begin === ticks(4),
        'Group spacing changed'
      );
      return { begin: ticks(4), groupEnd: ticks(10) };
    });
    await record('Trim captures the opposite clip edge', async () => {
      const motion = start(clip().querySelector<HTMLElement>('.trim-handle.right')!);
      motion.move(164);
      await nextTick();
      assert(guide()?.dataset.snapTime === String(ticks(6)), 'Trim guide missing');
      motion.end(164);
      await nextTick();
      assert(
        findItem(project.value, item.id).item.placement.end === ticks(6),
        'Trim did not author exact edge'
      );
      return { end: ticks(6) };
    });
    await record(
      'Escape and external project replacement remove sticky gesture state',
      async () => {
        const before = commits,
          motion = start(clip());
        motion.move(164);
        await nextTick();
        window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
        motion.end(164);
        await nextTick();
        assert(commits === before && !guide(), 'Escape committed or retained guide');
        const next = start(clip());
        next.move(164);
        await nextTick();
        project.value = editTimeline(project.value, [
          { action: 'configure_project', name: 'External replacement' }
        ]).project;
        await nextTick();
        next.end(164);
        await nextTick();
        assert(commits === before && !guide(), 'Stale gesture committed');
        return { commits: commits - before };
      }
    );
    await record(
      'Preview aligns its presented center exactly and retains it after release',
      async () => {
        await wait(() => !!hit(), 'Preview unavailable');
        const rect = hit().getBoundingClientRect(),
          stage = document.querySelector('.preview-stage')!.getBoundingClientRect();
        const dx = stage.left + stage.width / 2 - (rect.left + rect.width / 2);
        const motion = start(hit());
        motion.move(dx + 4);
        await nextTick();
        assert(previewGuide()?.dataset.snapX === '320', 'Preview center guide missing');
        window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Control', bubbles: true }));
        await nextTick();
        assert(!previewGuide(), 'Stationary Ctrl did not release preview guide');
        window.dispatchEvent(new KeyboardEvent('keyup', { key: 'Control', bubbles: true }));
        await nextTick();
        assert(previewGuide()?.dataset.snapX === '320', 'Ctrl release did not reacquire preview');
        const old = findItem(project.value, item.id).item.clip.visual.positionX;
        motion.end(dx + 4);
        await nextTick();
        const expected = old + (dx * 640) / stage.width;
        assert(
          Math.abs(findItem(project.value, item.id).item.clip.visual.positionX - expected) < 1e-6,
          'Snapped coordinate not committed exactly'
        );
        await wait(() => {
          const presented = hit().getBoundingClientRect();
          return (
            Math.abs(presented.left + presented.width / 2 - stage.left - stage.width / 2) < 0.1
          );
        }, 'Rendered center differs from guide');
        assert(!previewGuide(), 'Preview guide remained after release');
        return { positionX: expected };
      }
    );
    await record('Preview canvas edge capture survives viewport scaling', async () => {
      await nextTick();
      const stage = document.querySelector<HTMLElement>('.preview-stage')!;
      // DOM sizing here belongs to the test fixture, not browser automation.
      stage.style.width = '320px';
      const rect = hit().getBoundingClientRect(),
        canvas = stage.getBoundingClientRect();
      const dx = canvas.left - rect.left,
        motion = start(hit());
      motion.move(dx + 4);
      await nextTick();
      assert(previewGuide()?.dataset.snapX === '0', 'Scaled edge guide missing');
      motion.end(dx + 4);
      await nextTick();
      await wait(
        () => Math.abs(hit().getBoundingClientRect().left - canvas.left) < 0.1,
        'Scaled edge was not exact'
      );
      stage.style.width = '640px';
      return { stageWidth: canvas.width, edge: 0 };
    });
    await record(
      'Preview Ctrl bypass and Escape discard the snap without authoring a transform',
      async () => {
        const before = version.value,
          rect = hit().getBoundingClientRect(),
          stage = document.querySelector('.preview-stage')!.getBoundingClientRect();
        const dx = stage.left + stage.width / 2 - rect.left - rect.width / 2;
        const motion = start(hit());
        motion.move(dx + 4, 0, true);
        await nextTick();
        assert(!previewGuide(), 'Ctrl retained preview guide');
        window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
        motion.end(dx + 4);
        await nextTick();
        assert(
          version.value === before && !previewGuide(),
          'Cancelled preview wrote authored state'
        );
        return { version: version.value };
      }
    );
    assert(!errors.length, 'Renderer reported errors');
    report.complete = true;
  } catch (error) {
    rows.push({ passed: false, error: error instanceof Error ? error.message : String(error) });
  } finally {
    reset();
    await nextTick();
    show();
    button.disabled = false;
  }
};
