import test from 'node:test';
import assert from 'node:assert/strict';
import { createProject, addAsset, addText, ticks } from '../packages/core/project.mjs';
import { editTimeline } from '../packages/core/operations.mjs';

test('agent trim, split and move retain the correct source frames; failed batches are atomic', () => {
  const original = createProject();
  addAsset(original, {
    id: 'video',
    kind: 'video',
    path: '/tmp/test.mp4',
    name: 'Test',
    duration: ticks(12),
    size: 1000,
    width: 960,
    height: 540,
    hasAudio: false
  });
  const itemId = original.timeline.tracks[0].items[0].id;
  const trim = editTimeline(original, [
    { action: 'trim_clip', itemId, sourceInSeconds: 2, durationSeconds: 8 }
  ]);
  const split = editTimeline(trim.project, [{ action: 'split_clip', itemId, atSeconds: 4 }]);
  const rightItemId = split.operations[0].rightItemId;
  const cut = editTimeline(split.project, [
    { action: 'trim_clip', itemId: rightItemId, sourceInSeconds: 8, durationSeconds: 2 },
    { action: 'move_clip', itemId: rightItemId, startSeconds: 4 }
  ]);
  const [left, right] = cut.project.timeline.tracks[0].items;
  assert.deepEqual(left.clip.source, { begin: ticks(2), end: ticks(6) });
  assert.deepEqual(right.clip.source, { begin: ticks(8), end: ticks(10) });
  assert.deepEqual(right.placement, { begin: ticks(4), end: ticks(6) });
  assert.equal(original.timeline.tracks[0].items[0].clip.source.begin, 0);
  const before = JSON.stringify(cut.project);
  assert.throws(() =>
    editTimeline(cut.project, [
      { action: 'configure_project', name: 'Must not leak' },
      { action: 'trim_clip', itemId: rightItemId, sourceInSeconds: 11, durationSeconds: 5 }
    ])
  );
  assert.equal(JSON.stringify(cut.project), before);
  addText(cut.project, { content: 'Original' });
  const textId = cut.project.timeline.tracks[0].items[0].id;
  const styled = editTimeline(cut.project, [
    { action: 'set_text', itemId: textId, content: 'Agent 标题', fontSize: 42, color: '#ffffff' },
    { action: 'set_transform', itemId: textId, positionY: 170 }
  ]);
  assert.equal(styled.project.timeline.tracks[0].items[0].clip.text.content, 'Agent 标题');
  assert.equal(styled.project.timeline.tracks[0].items[0].clip.visual.positionY, 170);
});
