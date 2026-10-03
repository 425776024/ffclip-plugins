import { createApp, defineComponent, h, nextTick, ref, shallowRef } from 'vue';
import Timeline from '../src/editor/Timeline.vue';
import Preview from '../src/editor/Preview.vue';
import '../src/editor/editor.css';
import { createProject, addText, editTimeline, CommandHistory, EditTransaction, type EditorCommand, type Project, type Item } from '../packages/core/project.mjs';

const button = document.querySelector<HTMLButtonElement>('#run')!;
const status = document.querySelector<HTMLElement>('#status')!;
let activeApp: ReturnType<typeof createApp> | undefined;
type Row = { name: string; passed: boolean; details: unknown };
const wait = async (predicate: () => boolean, message: string, timeout = 20000) => {
  const start = performance.now();
  while (!predicate()) {
    if (performance.now() - start > timeout) throw new Error(message);
    await new Promise((resolve) => setTimeout(resolve, 30));
  }
};
const assert = (condition: unknown, message: string) => { if (!condition) throw new Error(message); };
function pointer(target: EventTarget, type: string, x: number, y: number) {
  target.dispatchEvent(new PointerEvent(type, { bubbles: true, cancelable: true, pointerId: 1, pointerType: 'mouse', isPrimary: true, button: 0, buttons: type === 'pointerup' ? 0 : 1, clientX: x, clientY: y, view: window }));
}
function startDrag(selector: string) {
  const node = document.querySelector<HTMLElement>(selector);
  assert(node, `Missing component hit target: ${selector}`);
  const rect = node!.getBoundingClientRect(), x = rect.left + Math.min(rect.width / 2, 90), y = rect.top + rect.height / 2;
  pointer(node!, 'pointerdown', x, y);
  return { x, y, move: (delta = 80) => pointer(window, 'pointermove', x + delta, y), end: (delta = 80) => pointer(window, 'pointerup', x + delta, y) };
}

button.onclick = async () => {
  if (button.disabled) return;
  button.disabled = true;
  activeApp?.unmount();
  const rows: Row[] = [], errors: string[] = [];
  const report = { eventSource: 'Synthetic PointerEvent and KeyboardEvent, isTrusted=false', scope: 'Real Vue Timeline + Preview component integration; not physical input acceptance', startedAt: new Date().toISOString(), userAgent: navigator.userAgent, rows, errors, complete: false };
  const show = () => { status.textContent = JSON.stringify(report, null, 2); };
  show();
  const initial = createProject('Synthetic component fixture');
  initial.canvas = { width: 640, height: 360 };
  const item = addText(initial, { content: 'Gesture fixture', length: 600000, fontSize: 36 });
  const project = shallowRef<Project>(initial), draft = shallowRef<Project | null>(null), version = ref(0), selected = ref(item.id);
  const transaction = new EditTransaction(), history = new CommandHistory();
  let commitEvents = 0, cancelEvents = 0, transformEvents = 0, draftEvents = 0;
  const apply = (operations: EditorCommand[]) => {
    const result = history.prepare(project.value, operations); history.accept(result);
    if (result.patches.length) { project.value = result.project; version.value++; }
  };
  const replace = () => {
    const external = editTimeline(project.value, [{ action: 'configure_project', name: `External ${version.value}` }]).project;
    version.value++;
    if (transaction.invalidate(version.value)) draft.value = null;
    project.value = external;
  };
  const shell = defineComponent({ setup() { return () => h('div', [
    h(Preview, { project: draft.value || project.value, time: 120000, playing: false, selected: selected.value, disabled: false, quality: 'half', mediaUrl: (id: string) => `/unused-media/${id}`, onError: (message: string) => errors.push(message), onSelect: (id: string) => { selected.value = id; }, onTransform: (id: string, changes: Partial<Item['clip']['visual']> & { layoutWidth?: number }) => { transformEvents++; const { layoutWidth, ...visual } = changes; apply([{ action: 'set_transform', itemId: id, ...visual }, ...(layoutWidth === undefined ? [] : [{ action: 'set_text', itemId: id, layoutWidth } as EditorCommand])]); } }),
    h(Timeline, { project: project.value, time: 120000, selected: selected.value, selection: [selected.value], zoom: 80, snap: false, busy: false, mediaUrl: (id: string) => `/unused-media/${id}`, onSelect: (id: string) => { selected.value = id; }, onDraft: (operations: EditorCommand[]) => { draftEvents++; if (!transaction.active) transaction.begin({ project: project.value, version: version.value }); draft.value = transaction.update(operations); }, onCancel: () => { cancelEvents++; transaction.cancel(); draft.value = null; }, onCommit: () => { commitEvents++; if (!transaction.active) return; const receipt = transaction.commit(); draft.value = null; if (receipt.changed) apply(receipt.operations); } })
  ]); } });
  activeApp = createApp(shell); activeApp.mount('#component-fixture');
  const record = async (name: string, test: () => Promise<unknown>) => { const details = await test(); rows.push({ name, passed: true, details }); show(); };
  try {
    await wait(() => !!document.querySelector('.timeline-clip'), 'Timeline did not mount');
    await record('Timeline Escape cancels candidate and leaves no history', async () => {
      const baseline = project.value, previousCommits = commitEvents, previousVersion = version.value;
      const drag = startDrag('.timeline-clip'); drag.move(); await nextTick();
      assert(transaction.active && draft.value, 'Pointer move did not create a candidate');
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })); drag.end(); await nextTick();
      assert(!transaction.active && !draft.value && project.value === baseline, 'Escape did not discard candidate');
      assert(commitEvents === previousCommits && version.value === previousVersion && !history.state.canUndo, 'Escape published a change');
      return { draftEvents, cancelEvents, commitEvents, version: version.value };
    });
    await record('Timeline external version replacement cancels an in-flight drag', async () => {
      const drag = startDrag('.timeline-clip'); drag.move(); await nextTick();
      assert(transaction.active, 'No active candidate before external update');
      const beforeCommits = commitEvents; replace(); await nextTick(); drag.end(); await nextTick();
      assert(!transaction.active && !draft.value && commitEvents === beforeCommits, 'External update allowed stale pointer-up commit');
      assert(project.value.timeline.tracks[0].items[0].placement.begin === 0, 'Stale drag moved the clip');
      return { version: version.value, commitEvents };
    });
    await record('Timeline completed drag produces one commit and one reversible entry', async () => {
      const beforeCommits = commitEvents;
      const drag = startDrag('.timeline-clip'); drag.move(40); drag.move(80); drag.end(80); await nextTick();
      assert(commitEvents === beforeCommits + 1 && history.state.undoCount === 1, 'Drag produced multiple commits');
      assert(project.value.timeline.tracks[0].items[0].placement.begin === 120000, 'Drag did not move exactly one second');
      apply([{ action: 'undo' }]); await nextTick();
      assert(project.value.timeline.tracks[0].items[0].placement.begin === 0, 'One undo did not restore clip');
      return { commitEvents, undoCount: history.state.undoCount, redoCount: history.state.redoCount };
    });
    await wait(() => !!document.querySelector('.unified-preview-hit'), `Preview hit target unavailable: ${errors.join('; ')}`);
    await record('Preview Escape discards presentation and emits no authored transform', async () => {
      const previousTransforms = transformEvents, previousVersion = version.value;
      const drag = startDrag('.unified-preview-hit'); drag.move(35); await nextTick();
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })); drag.end(35); await nextTick();
      assert(transformEvents === previousTransforms && version.value === previousVersion, 'Preview Escape emitted a transform');
      assert(project.value.timeline.tracks[0].items[0].clip.visual.positionX === 0, 'Preview mutated authored values');
      return { transformEvents, version: version.value };
    });
    await record('Preview external project replacement removes stale gesture handlers', async () => {
      const previousTransforms = transformEvents;
      const drag = startDrag('.unified-preview-hit'); drag.move(35); replace(); await nextTick(); drag.end(35); await nextTick();
      assert(transformEvents === previousTransforms, 'Preview published a stale transform after external replacement');
      assert(project.value.timeline.tracks[0].items[0].clip.visual.positionX === 0, 'External replacement retained stale presentation');
      return { transformEvents, version: version.value };
    });
    await record('Preview completed gesture emits one transform and one undo restores it', async () => {
      const previousTransforms = transformEvents;
      const drag = startDrag('.unified-preview-hit'); drag.move(20); drag.move(35); drag.end(35); await nextTick();
      assert(transformEvents === previousTransforms + 1 && history.state.undoCount === 1, 'Preview gesture did not form one command');
      assert(project.value.timeline.tracks[0].items[0].clip.visual.positionX !== 0, 'Preview transform was not committed');
      apply([{ action: 'undo' }]); await nextTick();
      assert(project.value.timeline.tracks[0].items[0].clip.visual.positionX === 0, 'One undo failed to restore Preview transform');
      return { transformEvents, undoCount: history.state.undoCount, redoCount: history.state.redoCount };
    });
    await record('Text corner resizing presents live scale, keeps the opposite corner fixed and commits one reversible edit', async () => {
      await wait(() => document.querySelector('.preview-stage')?.getAttribute('data-preview-current') === 'true', 'Preview geometry was not current');
      const target = document.querySelector<HTMLElement>('.unified-preview-hit.selected')!;
      const before = target.getBoundingClientRect(), previousTransforms = transformEvents;
      const handle = document.querySelector<HTMLElement>('.selection-handle.br')!, point = handle.getBoundingClientRect();
      const x = point.left + point.width / 2, y = point.top + point.height / 2;
      pointer(handle, 'pointerdown', x, y);
      pointer(window, 'pointermove', x + 45, y + 25);
      await wait(() => target.getBoundingClientRect().width > before.width + 10, 'Resize did not present intermediate pixels and bounds');
      const intermediate = target.getBoundingClientRect();
      assert(Math.abs(intermediate.left - before.left) < 1 && Math.abs(intermediate.top - before.top) < 1, 'Opposite corner moved during scaling');
      assert(transformEvents === previousTransforms, 'Pointer move prematurely authored a resize');
      pointer(window, 'pointerup', x + 45, y + 25);
      await nextTick();
      const visual = project.value.timeline.tracks[0].items[0].clip.visual;
      assert(visual.scaleX > 1 && Math.abs(visual.scaleX - visual.scaleY) < 1e-9, 'Scale was not committed proportionally');
      assert(transformEvents === previousTransforms + 1 && history.state.undoCount === 1, 'Resize did not form one history entry');
      apply([{ action: 'undo' }]);
      await wait(() => Math.abs(target.getBoundingClientRect().width - before.width) < 1, 'Undo did not restore resize pixels');
      assert(project.value.timeline.tracks[0].items[0].clip.visual.scaleX === 1, 'Undo did not restore authored scale');
      return { beforeWidth: before.width, intermediateWidth: intermediate.width, transformEvents, undoCount: history.state.undoCount };
    });
    await record('Text corner Escape and external document replacement cancel uncommitted scale', async () => {
      const previousTransforms = transformEvents, baseline = project.value;
      const drag = startDrag('.selection-handle.br'); drag.move(45);
      await wait(() => Number(document.querySelector<HTMLElement>('.unified-preview-hit.selected')?.style.width.replace('%', '')) > 1, 'Resize did not start');
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
      drag.end(45); await nextTick();
      assert(transformEvents === previousTransforms && project.value === baseline, 'Escape committed resize state');
      await wait(() => document.querySelector('.preview-stage')?.getAttribute('data-preview-current') === 'true', 'Cancel did not settle');
      const next = startDrag('.selection-handle.tl'); next.move(-45); replace(); await nextTick(); next.end(-45); await nextTick();
      assert(transformEvents === previousTransforms && project.value.timeline.tracks[0].items[0].clip.visual.scaleX === 1, 'External replacement allowed a stale resize commit');
      return { transformEvents, version: version.value };
    });
    await record('Text width handles reflow live without changing font or scale, commit once and undo', async () => {
      await wait(() => document.querySelector('.preview-stage')?.getAttribute('data-preview-current') === 'true', 'Preview was not current');
      const before=document.querySelector<HTMLElement>('.unified-preview-hit.selected')!.getBoundingClientRect();
      const baseline=project.value, visual=baseline.timeline.tracks[0].items[0].clip.visual, previousTransforms=transformEvents;
      const drag=startDrag('.text-width-handle.right'); drag.move(-100);
      await wait(()=>document.querySelector<HTMLElement>('.unified-preview-hit.selected')!.getBoundingClientRect().width<before.width-40,'Width did not reflow live');
      assert(project.value===baseline,'Live width mutated the authored project');
      drag.end(-100); await nextTick();
      const edited=project.value.timeline.tracks[0].items[0].clip;
      assert(transformEvents===previousTransforms+1,'Width gesture made multiple commits');
      assert(edited.text!.fontSize===36&&edited.visual.scaleX===visual.scaleX&&edited.visual.scaleY===visual.scaleY,'Width changed typography or layer scale');
      assert(edited.text!.layoutWidth!>0,'Width was not authored');
      apply([{action:'undo'}]); await nextTick();
      assert(project.value.timeline.tracks[0].items[0].clip.text!.layoutWidth===undefined,'Undo did not restore automatic width');
      await wait(()=>Math.abs(document.querySelector<HTMLElement>('.unified-preview-hit.selected')!.getBoundingClientRect().width-before.width)<1,'Undo did not restore control width');
      return {beforeWidth:before.width,fontSize:edited.text!.fontSize,authoredWidth:edited.text!.layoutWidth,transformEvents};
    });
    await record('Escape cancels uncommitted text width',async()=>{
      await wait(() => document.querySelector('.preview-stage')?.getAttribute('data-preview-current') === 'true', 'Preview was not current');
      const baseline=project.value, previousTransforms=transformEvents;
      const drag=startDrag('.text-width-handle.left');drag.move(65);await nextTick();
      window.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true,cancelable:true}));drag.end(65);await nextTick();
      assert(project.value===baseline&&transformEvents===previousTransforms,'Escape committed a width edit');
      return {transformEvents};
    });
    assert(errors.length===0,`Component errors: ${errors.join('; ')}`);
    report.complete = true;
  } catch (error) { rows.push({ name: 'Component integration failure', passed: false, details: error instanceof Error ? error.message : String(error) }); }
  finally { show(); button.disabled = false; }
};
