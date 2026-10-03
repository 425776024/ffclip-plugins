import test from 'node:test';
import assert from 'node:assert/strict';
import {
  createProject,
  addText,
  addAsset,
  trimItem,
  editTimeline,
  clone,
  ticks,
  findItem,
  sampleProperty,
  validateProject,
  CommandHistory,
  TEXT_TEMPLATES
} from '../packages/core/project.mjs';

test('text ranges extend left beyond source zero while preserving content and existing keyframe timing', () => {
  for (const template of [undefined, { id: TEXT_TEMPLATES[0].id, version: 1 }]) {
    const project = createProject();
    const item = addText(project, {
      content: '向左延长文字',
      start: ticks(10),
      length: ticks(5),
      template
    });
    const seeded = editTimeline(project, [
      {
        action: 'set_keyframe',
        itemId: item.id,
        property: 'visual.opacity',
        timeSeconds: 0,
        value: 0
      },
      {
        action: 'set_keyframe',
        itemId: item.id,
        property: 'visual.opacity',
        timeSeconds: 4,
        value: 1
      }
    ]).project;
    const trimmed = editTimeline(seeded, [
      { action: 'trim_range', itemId: item.id, beginSeconds: 11, endSeconds: 15 }
    ]).project;
    const baseline = findItem(trimmed, item.id).item;
    for (const direct of [false, true]) {
      let extended = clone(trimmed);
      if (direct) trimItem(extended, item.id, 0, ticks(15));
      else
        extended = editTimeline(extended, [
          { action: 'trim_range', itemId: item.id, beginSeconds: 0, endSeconds: 15 }
        ]).project;
      const result = findItem(extended, item.id).item;
      assert.deepEqual(result.placement, { begin: 0, end: ticks(15) });
      assert.deepEqual(result.clip.source, { begin: 0, end: ticks(15) });
      assert.deepEqual(result.clip.text, baseline.clip.text);
      for (const at of [11, 12, 13, 14, 15])
        assert.equal(
          sampleProperty(result, 'visual.opacity', ticks(at)),
          sampleProperty(baseline, 'visual.opacity', ticks(at - 11))
        );
      validateProject(extended);
    }
    const history = new CommandHistory();
    const prepared = history.prepare(trimmed, [
      { action: 'trim_range', itemId: item.id, beginSeconds: 0, endSeconds: 15 }
    ]);
    history.accept(prepared);
    const undone = history.prepare(prepared.project, [{ action: 'undo' }]);
    history.accept(undone);
    assert.deepEqual(undone.project, trimmed);
    const redone = history.prepare(undone.project, [{ action: 'redo' }]);
    assert.deepEqual(redone.project, prepared.project);
  }
});

test('media source limits and text timeline boundaries still reject invalid extensions', () => {
  const project = createProject();
  const video = addAsset(
    project,
    {
      id: 'video',
      name: 'Video',
      kind: 'video',
      path: '/media/video.mp4',
      duration: ticks(5),
      size: 1,
      width: 640,
      height: 360,
      hasAudio: false
    },
    { start: ticks(10) }
  );
  const text = addText(project, { content: '文字', start: ticks(10), length: ticks(5) });
  assert.throws(
    () =>
      editTimeline(project, [
        { action: 'trim_range', itemId: video.id, beginSeconds: 0, endSeconds: 15 }
      ]),
    /素材入点/
  );
  assert.throws(
    () =>
      editTimeline(project, [
        { action: 'trim_range', itemId: text.id, beginSeconds: -1, endSeconds: 15 }
      ]),
    /秒数无效/
  );
  addText(project, {
    content: '前一段',
    start: 0,
    length: ticks(3),
    trackId: findItem(project, text.id).track.id
  });
  assert.throws(
    () =>
      editTimeline(project, [
        { action: 'trim_range', itemId: text.id, beginSeconds: 2, endSeconds: 15 }
      ]),
    /重叠/
  );
});
