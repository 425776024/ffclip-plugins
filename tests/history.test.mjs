import test from 'node:test';
import assert from 'node:assert/strict';
import { createProject, addText, editTimeline, CommandHistory, applyDocumentPatches, EditTransaction } from '../packages/core/project.mjs';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';

test('drafts share untouched branches and history stores compact reversible field changes', () => {
  const p = createProject();
  for (let n = 0; n < 100; n++) addText(p, { content: `Text ${n}` });
  const item = p.timeline.tracks[0].items[0];
  const result = editTimeline(p, [{ action: 'set_transform', itemId: item.id, positionX: 42 }]);
  assert.equal(p.timeline.tracks[0].items[0].clip.visual.positionX, 0);
  assert.equal(result.project.assets, p.assets);
  assert.equal(result.project.timeline.tracks[1], p.timeline.tracks[1]);
  assert.equal(result.patches.length, 1);
  assert.ok(JSON.stringify(result.patches).length < 400);
  assert.deepEqual(applyDocumentPatches(result.project, result.inversePatches), p);
  const transaction = new EditTransaction();
  transaction.begin({ project: p, version: 1 });
  for (let n = 1; n < 10; n++) {
    const draft = transaction.update([{ action: 'set_transform', itemId: item.id, positionX: n }]);
    assert.equal(draft.timeline.tracks[1], p.timeline.tracks[1]);
  }
  assert.equal(transaction.cancel(), p);
});

test('history reverses add/remove/reorder and rejects stale publication without mutating stacks', () => {
  let p = createProject();
  const history = new CommandHistory();
  const apply = (ops) => { const c = history.prepare(p, ops); history.accept(c); p = c.project; return c; };
  apply([{ action: 'add_text', content: 'A' }, { action: 'add_text', content: 'B' }]);
  const original = p;
  const stale = history.prepare(p, [{ action: 'configure_project', name: 'stale' }]);
  apply([{ action: 'reorder_track', itemId: p.timeline.tracks[0].items[0].id, index: 1 }]);
  assert.throws(() => history.accept(stale), /历史版本冲突/);
  apply([{ action: 'undo' }]); assert.deepEqual(p, original);
  apply([{ action: 'redo' }]); assert.equal(p.timeline.tracks[1].items[0].clip.text.content, 'B');
  apply([{ action: 'undo' }]);
  apply([{ action: 'delete_clips', itemIds: [p.timeline.tracks[0].items[0].id] }]);
  assert.equal(history.state.canRedo, false);
  apply([{ action: 'undo' }]); assert.deepEqual(p, original);
  const bounded = new CommandHistory({ byteLimit: 1 });
  bounded.accept(bounded.prepare(p, [{ action: 'configure_project', name: 'new' }]));
  assert.equal(bounded.state.bytes, 0);
});

test('two clients share atomic undo/redo history and stale undo does not consume it', async (t) => {
  const server = await startServer({ port: 0 }); t.after(() => server.close());
  const a = new VideoCutClient(server.url), b = new VideoCutClient(server.url);
  const initial = await a.createSession();
  const edited = await a.editSession(initial.id, [{ action: 'add_text', content: 'Shared' }], 0);
  assert.equal(edited.history.canUndo, true);
  const outcomes = await Promise.allSettled([
    a.editSession(initial.id, [{ action: 'undo' }], edited.version),
    b.editSession(initial.id, [{ action: 'undo' }], edited.version)
  ]);
  assert.equal(outcomes.filter((o) => o.status === 'fulfilled').length, 1);
  assert.equal(outcomes.find((o) => o.status === 'rejected').reason.status, 409);
  const undone = await b.getSession(initial.id);
  assert.equal(undone.project.timeline.tracks.length, 0);
  assert.equal(undone.history.canRedo, true);
  const redone = await b.editSession(initial.id, [{ action: 'redo' }], undone.version);
  assert.equal(redone.project.timeline.tracks[0].items[0].clip.text.content, 'Shared');
  await assert.rejects(a.editSession(initial.id, [{ action: 'set_transform', itemId: 'bad', positionX: 1 }], redone.version));
  assert.deepEqual((await a.getSession(initial.id)).history, redone.history);
  const reset = await a.updateSession(initial.id, redone.project, redone.version);
  assert.equal(reset.history.canUndo, false);
});
