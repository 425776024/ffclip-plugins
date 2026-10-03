import test from 'node:test';
import assert from 'node:assert/strict';
import {
  createProject,
  addAsset,
  splitItem,
  moveItem,
  trimItem,
  removeItem,
  ticks,
  duration,
  validateProject,
  clone,
  identity
} from '../packages/core/project.mjs';
const asset = () => ({
  id: identity('asset'),
  name: 'clip.mp4',
  kind: 'video',
  path: '/media/clip.mp4',
  duration: ticks(10),
  size: 42,
  width: 1920,
  height: 1080,
  hasAudio: true
});

test('every new session starts empty with a distinct identity', () => {
  const a = createProject(),
    b = createProject();
  assert.notEqual(a.id, b.id);
  assert.equal(duration(a), 0);
  assert.deepEqual(a.assets, []);
  assert.deepEqual(a.timeline.tracks, []);
});
test('split, move and trim preserve source selection at native tick precision', () => {
  const p = createProject(),
    item = addAsset(p, asset());
  const right = splitItem(p, item.id, ticks(4));
  assert.equal(right.clip.source.begin, ticks(4));
  assert.equal(item.clip.source.end, ticks(4));
  moveItem(p, right.id, ticks(6));
  assert.deepEqual(right.clip.source, { begin: ticks(4), end: ticks(10) });
  trimItem(p, right.id, ticks(7), ticks(11));
  assert.deepEqual(right.clip.source, { begin: ticks(5), end: ticks(9) });
  assert.equal(duration(p), ticks(11));
  validateProject(p);
  removeItem(p, item.id);
  removeItem(p, right.id);
  assert.equal(p.timeline.tracks.length, 0);
});
test('collision, invalid ranges and locked tracks fail instead of corrupting native data', () => {
  const p = createProject(),
    item = addAsset(p, asset());
  const right = splitItem(p, item.id, ticks(5));
  assert.throws(() => moveItem(clone(p), right.id, ticks(4)), /重叠/);
  assert.throws(() => trimItem(clone(p), right.id, ticks(5), ticks(11)), /超出/);
  assert.throws(() => splitItem(clone(p), item.id, 0), /内部/);
  p.timeline.tracks[0].locked = true;
  assert.throws(() => removeItem(p, item.id), /锁定/);
  const invalid = clone(p);
  invalid.assets[0].duration = Infinity;
  assert.throws(() => validateProject(invalid), /时长/);
});

test('editable text uses its own track and preserves content when split and trimmed', async () => {
  const { addText } = await import('../packages/core/project.mjs');
  const p = createProject();
  const title = addText(p, { content: '你好\nVideoCut', length: ticks(5) });
  assert.equal(p.assets.length, 0);
  assert.equal(p.timeline.tracks[0].type, 'text');
  const right = splitItem(p, title.id, ticks(2));
  assert.equal(right.clip.text.content, '你好\nVideoCut');
  trimItem(p, right.id, ticks(2), ticks(7));
  validateProject(p);
  right.clip.text.color = 'red:injected';
  assert.throws(() => validateProject(p), /样式/);
});
