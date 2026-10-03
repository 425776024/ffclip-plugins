import { createApp, nextTick } from 'vue';
import Editor from '../src/editor/Editor.vue';
import '../src/editor/editor.css';
import { VideoCutClient } from '../packages/client/index.mjs';
import { createProject, addAsset, findItem } from '../packages/core/project.mjs';

const button = document.querySelector<HTMLButtonElement>('#run')!;
const status = document.querySelector<HTMLElement>('#status')!;
const wait = async (predicate: () => boolean, message: string) => {
  const start = performance.now();
  while (!predicate()) {
    if (performance.now() - start > 20000) throw new Error(message);
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
};
const delay = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
const assert = (condition: unknown, message: string) => {
  if (!condition) throw new Error(message);
};
function pointer(target: EventTarget, type: string, x: number, y: number) {
  target.dispatchEvent(
    new PointerEvent(type, {
      bubbles: true,
      cancelable: true,
      pointerId: 1,
      pointerType: 'mouse',
      isPrimary: true,
      button: 0,
      buttons: type === 'pointerup' ? 0 : 1,
      clientX: x,
      clientY: y
    })
  );
}
const hit = () => document.querySelector<HTMLElement>('.unified-preview-hit.selected')!;
const left = () => parseFloat(hit().style.left);
const revisions = () =>
  [...document.querySelectorAll<HTMLElement>('.clip-media-visual')].map((element) =>
    Number(element.dataset.visualRevision)
  );
async function drag() {
  const node = hit(),
    rect = node.getBoundingClientRect();
  const x = rect.left + rect.width / 2,
    y = rect.top + rect.height / 2;
  const before = left(),
    stage = document.querySelector('.preview-stage')!.getBoundingClientRect();
  pointer(node, 'pointerdown', x, y);
  pointer(window, 'pointermove', x + 35, y);
  const expected = before + 3500 / stage.width;
  await wait(() => Math.abs(left() - expected) < 0.3, 'Moving frame was not presented');
  const presented = left();
  pointer(window, 'pointerup', x + 35, y);
  return { before, presented };
}

button.onclick = async () => {
  button.disabled = true;
  const rows: unknown[] = [];
  const report = { complete: false, eventSource: 'Synthetic PointerEvent, isTrusted=false', rows };
  const show = () => {
    status.textContent = JSON.stringify(report, null, 2);
  };
  const originalUrl = location.href,
    originalFetch = window.fetch;
  const client = new VideoCutClient(location.origin);
  let app: ReturnType<typeof createApp> | undefined,
    session = '',
    failNext = false;
  const requests: Array<{ version: number; operations: unknown[] }> = [];
  let disabledChanges = 0;
  const observer = new MutationObserver((mutations) => {
    disabledChanges += mutations.length;
  });
  const record = async (name: string, test: () => Promise<unknown>) => {
    rows.push({ name, passed: true, details: await test() });
    show();
  };
  try {
    await client.connect();
    const fixtures =
      new URLSearchParams(location.search).get('fixtures') ||
      '/Users/jxinfa/WebstormProjects/videocut/.local/architecture-qa';
    const video = await client.importMedia(`${fixtures}/timecode.mp4`);
    const audio = await client.importMedia(`${fixtures}/stereo.wav`);
    const project = createProject('Preview commit regression');
    project.canvas = { width: 640, height: 360 };
    const item = addAsset(project, video);
    addAsset(project, video);
    addAsset(project, audio);
    session = (await client.createSession(project)).id;
    history.replaceState(null, '', `/?session=${session}`);
    window.fetch = async (input, init) => {
      if (String(input).includes(`/sessions/${session}/commands`)) {
        requests.push(JSON.parse(String(init?.body)));
        await delay(350);
        if (failNext) {
          failNext = false;
          return new Response(JSON.stringify({ error: 'Synthetic commit rejection' }), {
            status: 500
          });
        }
      }
      return originalFetch(input, init);
    };
    app = createApp(Editor);
    app.mount('#fixture');
    await wait(
      () =>
        document.querySelectorAll('.clip-media-visual[data-visual-quality="final"]').length === 2 &&
        revisions().length === 3 &&
        revisions().every((value) => value > 0),
      'Media tiles did not settle'
    );
    const clip = document.querySelector<HTMLElement>('.timeline-clip')!;
    const rect = clip.getBoundingClientRect();
    pointer(clip, 'pointerdown', rect.left + 60, rect.top + rect.height / 2);
    pointer(window, 'pointerup', rect.left + 60, rect.top + rect.height / 2);
    await wait(() => Boolean(hit()), 'Selected preview hit was not presented');
    await delay(200);
    document
      .querySelectorAll('.project-name, .open-session, .app-actions .primary, .inspector-body')
      .forEach((element) =>
        observer.observe(element, { attributes: true, attributeFilter: ['disabled'] })
      );
    await record(
      'Delayed commit and queued edit keep the final frame and all media tiles stable',
      async () => {
        const before = revisions(),
          movement = await drag();
        const name = document.querySelector<HTMLInputElement>('.project-name')!;
        name.value = 'Queued rename';
        name.dispatchEvent(new Event('change', { bubbles: true }));
        const samples: number[] = [];
        const sample = setInterval(() => samples.push(left()), 10);
        await wait(
          () => requests.length === 2 && document.title.startsWith('Queued rename'),
          'Queued edit did not complete'
        );
        await delay(200);
        clearInterval(sample);
        const saved = await client.getSession(session);
        assert(
          findItem(saved.project, item.id).item.clip.visual.positionX !== 0,
          'Transform was not authored'
        );
        assert(
          requests[0].version === 0 && requests[1].version === 1 && saved.version === 2,
          'Edits were not serialized'
        );
        assert(
          samples.every((value) => Math.abs(value - movement.presented) < 0.3),
          'Frame reverted during commit'
        );
        assert(
          JSON.stringify(revisions()) === JSON.stringify(before),
          'Position/rename repainted media tiles'
        );
        assert(disabledChanges === 0, 'Global controls flashed disabled during ordinary edits');
        return {
          samples: samples.length,
          leftRange: [Math.min(...samples), Math.max(...samples)],
          requests: requests.map((request) => request.version),
          tileRevisions: before,
          disabledChanges
        };
      }
    );
    await record(
      'Undo and redo preserve media tiles while restoring the authored transform',
      async () => {
        const before = revisions();
        let saved = await client.getSession(session);
        for (const action of ['undo', 'undo', 'redo'] as const)
          saved = await client.editSession(session, [{ action }], saved.version);
        await delay(250);
        assert(findItem(saved.project, item.id).item.clip.visual.positionX !== 0, 'Redo failed');
        assert(
          JSON.stringify(revisions()) === JSON.stringify(before),
          'Undo/redo repainted unchanged tiles'
        );
        return { version: saved.version, tileRevisions: revisions() };
      }
    );
    await record('Rejected commit restores the old frame and accepts the next drag', async () => {
      const saved = await client.getSession(session),
        before = left();
      failNext = true;
      await drag();
      await wait(
        () => Boolean(document.querySelector('.message.error')),
        'Commit error was not shown'
      );
      await wait(() => Math.abs(left() - before) < 0.3, 'Rejected presentation was not discarded');
      assert(
        (await client.getSession(session)).version === saved.version,
        'Rejected edit changed version'
      );
      await drag();
      await delay(700);
      assert(
        (await client.getSession(session)).version === saved.version + 1,
        'Next gesture remained blocked'
      );
      return { version: saved.version + 1 };
    });
    await record('Source crop refreshes only the affected media tile', async () => {
      const before = revisions(),
        saved = await client.getSession(session);
      await client.editSession(
        session,
        [
          {
            action: 'set_transform',
            itemId: item.id,
            crop: { left: 0.1, right: 0, top: 0, bottom: 0 }
          }
        ],
        saved.version
      );
      await wait(() => revisions()[0] > before[0], 'Crop did not refresh its media tile');
      await delay(250);
      assert(
        revisions()
          .slice(1)
          .every((value, index) => value === before[index + 1]),
        'Crop repainted other tiles'
      );
      return { before, after: revisions() };
    });
    await record(
      'Editor snap switch controls preview center snapping and free movement',
      async () => {
        const saved = await client.getSession(session);
        await client.editSession(
          session,
          [
            {
              action: 'set_transform',
              itemId: item.id,
              positionX: 30,
              positionY: 20,
              scaleX: 0.4,
              scaleY: 0.4,
              crop: { left: 0, right: 0, top: 0, bottom: 0 }
            }
          ],
          saved.version
        );
        const stage = document.querySelector<HTMLElement>('.preview-stage')!;
        await wait(() => stage.dataset.previewCurrent === 'true', 'Preview was not current');
        await wait(
          () => Math.abs(left() - (30 + (30 / 640) * 100)) < 0.3,
          'Positioned preview was not presented'
        );
        const switchNode = document.querySelector<HTMLElement>('[aria-label="吸附"]')!;
        assert(
          switchNode.getAttribute('aria-pressed') === 'true',
          'Snap must initially be enabled'
        );
        const canvas = stage.getBoundingClientRect(),
          bounds = hit().getBoundingClientRect();
        const fromX = bounds.left + bounds.width / 2,
          fromY = bounds.top + bounds.height / 2;
        const nearX = canvas.left + canvas.width / 2 + 3;
        const nearY = canvas.top + canvas.height / 2 + 2;
        pointer(hit(), 'pointerdown', fromX, fromY);
        pointer(window, 'pointermove', nearX, nearY);
        await wait(
          () => document.querySelectorAll('.preview-snap-guide').length === 2,
          'Editor did not enable preview center guides'
        );
        pointer(window, 'pointerup', nearX, nearY);
        await delay(700);
        const centered = await client.getSession(session);
        const visual = findItem(centered.project, item.id).item.clip.visual;
        assert(
          Math.abs(visual.positionX) < 1e-8 && Math.abs(visual.positionY) < 1e-8,
          'Center snapping did not author exact center'
        );
        await wait(
          () => stage.dataset.previewCurrent === 'true',
          'Centered preview did not settle'
        );
        switchNode.click();
        await nextTick();
        assert(switchNode.getAttribute('aria-pressed') === 'false', 'Snap switch did not turn off');
        const center = hit().getBoundingClientRect();
        const x = center.left + center.width / 2,
          y = center.top + center.height / 2;
        pointer(hit(), 'pointerdown', x, y);
        pointer(window, 'pointermove', x + 3, y + 2);
        await nextTick();
        assert(!document.querySelector('.preview-snap-guide'), 'Disabled preview still snapped');
        pointer(window, 'pointerup', x + 3, y + 2);
        await delay(700);
        const free = findItem((await client.getSession(session)).project, item.id).item.clip.visual;
        assert(
          Math.abs(free.positionX - (3 * 640) / canvas.width) < 1e-6 &&
            Math.abs(free.positionY - (2 * 360) / canvas.height) < 1e-6,
          'Disabled snapping did not preserve free pointer movement'
        );
        switchNode.click();
        return {
          centered: { x: visual.positionX, y: visual.positionY },
          free: { x: free.positionX, y: free.positionY }
        };
      }
    );
    report.complete = true;
  } catch (error) {
    rows.push({ name: 'Preview commit regression', passed: false, details: String(error) });
  } finally {
    observer.disconnect();
    app?.unmount();
    window.fetch = originalFetch;
    if (session) await client.closeSession(session);
    history.replaceState(null, '', originalUrl);
    show();
    button.disabled = false;
  }
};
