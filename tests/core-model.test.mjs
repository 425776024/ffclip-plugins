import test from 'node:test';
import assert from 'node:assert/strict';
import {
  createProject,
  addAsset,
  addText,
  ticks,
  splitItem,
  clone,
  validateProject,
  editTimeline,
  mapTimelineToSource,
  mapSourceToTimeline,
  evaluateAudio,
  evaluateVisual,
  activeTransitions,
  evaluateFrame,
  evaluateEffects,
  getPropertyDescriptor,
  isPropertyApplicable,
  selectionClosure
} from '../packages/core/project.mjs';
import { PROPERTY_DESCRIPTORS, propertyInputSchema } from '../packages/core/project.mjs';
function fixture() {
  const project = createProject();
  const item = addAsset(project, {
    id: 'asset-a',
    name: 'A',
    kind: 'video',
    path: '/tmp/a.mp4',
    duration: ticks(20),
    size: 1,
    width: 1920,
    height: 1080,
    hasAudio: true
  });
  return { project, item };
}
function command(p, ...ops) {
  return editTimeline(p, ops).project;
}
test('linked AV trim, speed and split share one clock and keep left/right links independent', () => {
  let { project, item: video } = fixture();
  const audio = addAsset(project, { ...project.assets[0], id: 'linked-audio', kind: 'audio', width: 0, height: 0 });
  project = command(project, { action: 'link_clips', itemIds: [video.id, audio.id] });
  project = command(project, { action: 'trim_range', itemId: video.id, beginSeconds: 2, endSeconds: 12 });
  const get = (id) => project.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === id);
  assert.deepEqual(get(video.id).clip.source, get(audio.id).clip.source);
  assert.deepEqual(get(video.id).placement, get(audio.id).placement);
  project = command(project,
    { action: 'set_speed', itemId: video.id, rate: 2 },
    { action: 'set_speed', itemId: audio.id, rate: 2 });
  assert.deepEqual(get(video.id).placement, { begin: ticks(2), end: ticks(7) });
  assert.deepEqual(get(video.id).placement, get(audio.id).placement);
  assert.equal(mapTimelineToSource(get(video.id), ticks(4)), mapTimelineToSource(get(audio.id), ticks(4)));
  const split = editTimeline(project, [{ action: 'split_clip', itemId: video.id, atSeconds: 4 }]);
  project = split.project;
  assert.equal(split.operations.length, 2);
  assert.equal(project.timeline.links.length, 2);
  const rights = split.operations.map((result) => result.rightItemId);
  assert.ok(project.timeline.links.some((link) => rights.every((id) => link.itemIds.includes(id))));
  const before = structuredClone(project);
  project = command(project, { action: 'move_clip', itemId: rights[0], startSeconds: 10 });
  assert.equal(get(rights[1]).placement.begin, ticks(10));
  assert.equal(get(video.id).placement.begin, ticks(2));
  const locked = structuredClone(before);
  locked.timeline.tracks.find((track) => track.items.some((item) => item.id === audio.id)).locked = true;
  assert.throws(() => command(locked, { action: 'trim_range', itemId: video.id, beginSeconds: 3, endSeconds: 4 }), /锁定/);
});

test('text/time descriptors drive range validation, values, MCP schema and render impact', () => {
  const project = createProject(), item = addText(project, { content: 'Before' });
  const result = editTimeline(project, [
    { action: 'set_property', itemId: item.id, property: 'text.fontSize', value: 80 },
    { action: 'set_property', itemId: item.id, property: 'text.content', value: 'After' },
    { action: 'set_property', itemId: item.id, property: 'time.start', value: ticks(2) }
  ]);
  const edited = result.project.timeline.tracks[0].items[0];
  assert.equal(edited.name, 'After');
  assert.equal(edited.placement.begin, ticks(2));
  assert.equal(edited.clip.text.fontSize, 80);
  assert.ok(result.changes.impacts.includes('layout'));
  assert.equal(propertyInputSchema('text.fontSize').maximum, PROPERTY_DESCRIPTORS['text.fontSize'].max);
  assert.equal(propertyInputSchema('time.start', { seconds: true }).maximum, 86400);
  assert.throws(() => command(project, { action: 'set_property', itemId: item.id, property: 'text.fontSize', value: 1001 }), /范围/);
  assert.throws(() => command(project, { action: 'set_property', itemId: item.id, property: 'time.sourceIn', value: 1 }), /时间属性/);
});
test('relationship changes inside a batch govern the following trim and single-item delete', () => {
  const { project, item: video } = fixture();
  const audio = addAsset(project, { ...project.assets[0], id: 'batch-audio', kind: 'audio', width: 0, height: 0 });
  const linked = command(project,
    { action: 'link_clips', itemIds: [video.id, audio.id] },
    { action: 'trim_range', itemId: video.id, beginSeconds: 2, endSeconds: 10 });
  const items = (p) => p.timeline.tracks.flatMap((track) => track.items);
  assert.ok(items(linked).every((item) => item.placement.begin === ticks(2) && item.clip.source.end === ticks(10)));
  const moved = command(linked,
    { action: 'set_property', itemId: video.id, property: 'time.start', value: ticks(3) },
    { action: 'set_property', itemId: audio.id, property: 'time.start', value: ticks(3) });
  assert.ok(items(moved).every((item) => item.placement.begin === ticks(3)));
  const unlinked = command(linked,
    { action: 'unlink_clips', groupId: linked.timeline.links[0].id },
    { action: 'trim_range', itemId: video.id, beginSeconds: 3, endSeconds: 9 });
  assert.equal(items(unlinked).find((item) => item.id === audio.id).placement.begin, ticks(2));
  assert.equal(command(linked, { action: 'remove_clip', itemId: video.id }).timeline.tracks.length, 0);
  assert.equal(items(project).length, 2);
});
test('ripple scope shifts synchronized followers and rejects locked or crossing content atomically', () => {
  const project = createProject();
  const cut = addText(project, { content: 'Cut', length: ticks(2) });
  const firstTrack = project.timeline.tracks[0];
  const follower = clone(cut); follower.id = 'ripple-follower'; follower.clip.id = 'ripple-clip';
  follower.placement = { begin: ticks(2), end: ticks(4) }; firstTrack.items.push(follower);
  const synced = addText(project, { content: 'Synced', start: ticks(2), length: ticks(2) });
  const syncTrack = project.timeline.tracks.find((track) => track.items.includes(synced)); syncTrack.syncLocked = true;
  const independent = addText(project, { content: 'Independent', start: ticks(2), length: ticks(2) });
  project.timeline.tracks.find((track) => track.items.includes(independent)).syncLocked = false;
  const result = command(project, { action: 'ripple_delete', itemIds: [cut.id], scope: 'syncLocked' });
  const get = (id) => result.timeline.tracks.flatMap((track) => track.items).find((item) => item.id === id);
  assert.equal(get(follower.id).placement.begin, 0);
  assert.equal(get(synced.id).placement.begin, 0);
  assert.equal(get(independent.id).placement.begin, ticks(2));
  syncTrack.locked = true;
  assert.throws(() => command(project, { action: 'ripple_delete', itemIds: [cut.id], scope: 'syncLocked' }), /锁定/);
  assert.equal(firstTrack.items[0].id, cut.id);
  syncTrack.locked = false; synced.placement = { begin: ticks(1), end: ticks(3) };
  assert.throws(() => command(project, { action: 'ripple_delete', itemIds: [cut.id], scope: 'syncLocked' }), /穿过/);
});
test('effect ordering and transition edits retain owners and validate the final handle window', () => {
  let { project, item } = fixture();
  const right = splitItem(project, item.id, ticks(10));
  project = command(project,
    { action: 'add_effect', itemId: item.id, templateId: 'blur' },
    { action: 'add_effect', itemId: item.id, templateId: 'lut' },
    { action: 'add_transition', fromItemId: item.id, toItemId: right.id, templateId: 'fade', durationSeconds: 1 });
  const effectIds = project.timeline.tracks[0].items[0].clip.effects.map((effect) => effect.id).reverse();
  const transitionId = project.timeline.transitions[0].id;
  project = command(project,
    { action: 'reorder_effects', itemId: item.id, effectIds },
    { action: 'update_transition', transitionId, durationSeconds: 2, parameters: { color: '#ffffff' } });
  assert.deepEqual(project.timeline.tracks[0].items[0].clip.effects.map((effect) => effect.id), effectIds);
  assert.equal(project.timeline.transitions[0].parameters.color, '#ffffff');
  assert.equal(project.timeline.transitions[0].duration, ticks(2));
  assert.throws(() => command(project, { action: 'update_transition', transitionId, durationSeconds: 50 }), /转场/);
});
test('unsupported media property owners reject non-default values and even default-valued curves', () => {
  const { project, item: video } = fixture();
  const audio = addAsset(project, {
    ...project.assets[0],
    id: 'owner-audio',
    kind: 'audio',
    width: 0,
    height: 0
  });
  const image = addAsset(project, {
    ...project.assets[0],
    id: 'owner-image',
    kind: 'image',
    hasAudio: false
  });
  const silent = addAsset(project, { ...project.assets[0], id: 'owner-silent', hasAudio: false });
  const text = addText(project, { content: 'Owner' });
  const cases = [
    [audio, 'visual.opacity', 0.5, 1],
    [image, 'audio.gainLinear', 0.5, 1],
    [silent, 'audio.gainLinear', 0.5, 1],
    [text, 'audio.gainLinear', 0.5, 1]
  ];
  for (const [item, property, value, defaultValue] of cases) {
    assert.equal(isPropertyApplicable(project, item, property), false);
    assert.throws(
      () => command(project, { action: 'set_property', itemId: item.id, property, value }),
      /不支持此属性/
    );
    assert.throws(
      () =>
        command(project, {
          action: 'set_keyframe',
          itemId: item.id,
          property,
          timeSeconds: 0,
          value: defaultValue
        }),
      /不支持此关键帧/
    );
    assert.doesNotThrow(() =>
      command(project, { action: 'set_property', itemId: item.id, property, value: defaultValue })
    );
  }
  assert.throws(
    () => command(project, { action: 'set_audio', itemId: text.id, muted: true }),
    /不支持此属性/
  );
  assert.throws(
    () => command(project, { action: 'set_audio', itemId: silent.id, fadeIn: ticks(1) }),
    /不支持此属性/
  );
  assert.equal(isPropertyApplicable(project, video, 'audio.gainLinear'), true);
  assert.equal(isPropertyApplicable(project, audio, 'audio.gainLinear'), true);
  assert.equal(isPropertyApplicable(project, image, 'visual.opacity'), true);
  assert.equal(isPropertyApplicable(project, text, 'visual.opacity'), true);
  validateProject(project);
});
test('timeline and command selection closure share transitive groups and links', () => {
  let { project, item: a } = fixture();
  const b = splitItem(project, a.id, ticks(10)),
    c = addText(project, { content: 'Related' });
  project = command(
    project,
    { action: 'group_clips', itemIds: [a.id, b.id] },
    { action: 'link_clips', itemIds: [b.id, c.id], expandLinked: false }
  );
  assert.deepEqual(new Set(selectionClosure(project, [a.id])), new Set([a.id, b.id, c.id]));
  assert.deepEqual(selectionClosure(project, [a.id], { expand: false }), [a.id]);
  assert.throws(() => selectionClosure(project, ['missing']), /不存在/);
  const next = editTimeline(project, [{ action: 'move_clips', itemIds: [a.id], deltaSeconds: 2 }]);
  assert.deepEqual(new Set(next.operations[0].itemIds), new Set(selectionClosure(project, [a.id])));
  for (const track of next.project.timeline.tracks)
    for (const item of track.items)
      assert.equal(
        item.placement.begin,
        project.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === item.id).placement
          .begin + ticks(2)
      );
});
test('native-admitted effect automation samples, slices, copies and deletes its stable owner', () => {
  let { project, item } = fixture();
  project = command(
    project,
    { action: 'add_effect', itemId: item.id, templateId: 'lut' },
    { action: 'add_effect', itemId: item.id, templateId: 'blur' }
  );
  item = project.timeline.tracks[0].items[0];
  const effect = item.clip.effects[0];
  effect.id = 'fx.with.dotted.identity';
  const property = `effects.${effect.id}.amount`;
  assert.equal(getPropertyDescriptor(item, property).keyframe, true);
  assert.equal(
    getPropertyDescriptor(item, `effects.${item.clip.effects[1].id}.radius`).keyframe,
    false
  );
  assert.throws(
    () =>
      command(project, {
        action: 'set_keyframe',
        itemId: item.id,
        property: `effects.${item.clip.effects[1].id}.radius`,
        timeSeconds: 0,
        value: 0
      }),
    /不支持/
  );
  assert.throws(
    () =>
      command(project, {
        action: 'set_property',
        itemId: item.id,
        property: '__proto__',
        value: 1
      }),
    /未知/
  );
  project = command(
    project,
    { action: 'set_keyframe', itemId: item.id, property, timeSeconds: 0, value: 0 },
    { action: 'set_keyframe', itemId: item.id, property, timeSeconds: 10, value: 1 }
  );
  item = project.timeline.tracks[0].items[0];
  assert.equal(
    evaluateEffects(item, ticks(2)).find((f) => f.templateId === 'lut').parameters.amount,
    0.2
  );
  assert.equal(evaluateFrame(project, ticks(2)).layers[0].effects[0].parameters.amount, 0.2);
  project = command(project, { action: 'split_clip', itemId: item.id, atSeconds: 4 });
  const right = project.timeline.tracks[0].items[1];
  assert.notEqual(right.clip.effects[0].id, effect.id);
  assert.equal(evaluateEffects(right, ticks(5))[0].parameters.amount, 0.5);
  assert.equal(Object.keys(right.clip.automation)[0], `effects.${right.clip.effects[0].id}.amount`);
  project = command(project, { action: 'duplicate_clips', itemIds: [right.id], offsetSeconds: 20 });
  const duplicate = project.timeline.tracks[0].items[2];
  assert.equal(evaluateEffects(duplicate, ticks(25))[0].parameters.amount, 0.5);
  project = command(project, {
    action: 'remove_effect',
    itemId: duplicate.id,
    effectId: duplicate.clip.effects[0].id
  });
  assert.deepEqual(project.timeline.tracks[0].items[2].clip.automation, {});
  validateProject(project);
});
test('batch moves validate the complete candidate and are independent of sibling operation order', () => {
  const { project, item } = fixture();
  const right = splitItem(project, item.id, ticks(10));
  const ops = [
    { action: 'move_clip', itemId: item.id, startSeconds: 2 },
    { action: 'move_clip', itemId: right.id, startSeconds: 12 }
  ];
  assert.deepEqual(
    editTimeline(project, ops).project,
    editTimeline(project, ops.toReversed()).project
  );
  assert.equal(item.placement.begin, 0);
  assert.throws(
    () => command(project, ...ops, { action: 'set_transform', itemId: item.id, opacity: 2 }),
    /范围/
  );
  assert.equal(item.placement.begin, 0);
});
test('constant speed maps precise source boundaries, and split preserves rate and audio duration', () => {
  let { project, item } = fixture();
  project = command(project, { action: 'set_speed', itemId: item.id, constantRatePpm: 2000000 });
  item = project.timeline.tracks[0].items[0];
  assert.equal(item.placement.end, ticks(10));
  assert.equal(mapTimelineToSource(item, ticks(3)), ticks(6));
  assert.equal(mapSourceToTimeline(item, ticks(6)), ticks(3));
  project = command(project, { action: 'split_clip', itemId: item.id, atSeconds: 4 });
  const right = project.timeline.tracks[0].items[1];
  assert.equal(right.clip.source.begin, ticks(8));
  validateProject(project);
});
test('property projection shares linear and eased keyframes and audio fade semantics', () => {
  let { project, item } = fixture();
  project = command(
    project,
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
    },
    { action: 'set_audio', itemId: item.id, fadeIn: ticks(2), fadeOut: ticks(2) }
  );
  item = project.timeline.tracks[0].items[0];
  assert.equal(evaluateVisual(item, ticks(2)).opacity, 0.5);
  assert.equal(evaluateAudio(item, ticks(1)).gainLinear, 0.5);
  assert.equal(evaluateAudio(item, ticks(19)).gainLinear, 0.5);
  project = command(project, { action: 'split_clip', itemId: item.id, atSeconds: 2 });
  assert.equal(evaluateVisual(project.timeline.tracks[0].items[1], ticks(3)).opacity, 0.75);
});
test('grouped movement, copy identity and ripple deletion preserve shared selections', () => {
  let { project, item } = fixture();
  const right = splitItem(project, item.id, ticks(10));
  project = command(
    project,
    { action: 'group_clips', itemIds: [item.id, right.id] },
    { action: 'move_clips', itemIds: [item.id], deltaSeconds: 2 }
  );
  assert.deepEqual(
    project.timeline.tracks[0].items.map((i) => i.placement.begin),
    [ticks(2), ticks(12)]
  );
  project = command(project, { action: 'duplicate_clips', itemIds: [item.id], offsetSeconds: 20 });
  assert.equal(project.timeline.tracks[0].items.length, 4);
  assert.equal(project.timeline.groups.length, 2);
  project = command(project, { action: 'ripple_delete', itemIds: [item.id] });
  assert.equal(project.timeline.tracks[0].items.length, 2);
  assert.equal(project.timeline.tracks[0].items[0].placement.begin, ticks(2));
  validateProject(project);
});
test('transition windows evaluate both sources and reject missing handles atomically', () => {
  let { project, item } = fixture();
  const right = splitItem(project, item.id, ticks(10));
  project = command(project, {
    action: 'add_transition',
    fromItemId: item.id,
    toItemId: right.id,
    templateId: 'dissolve',
    durationSeconds: 2
  });
  const active = activeTransitions(project, ticks(10));
  assert.equal(active[0].progress, 0.5);
  const plan = evaluateFrame(project, ticks(10));
  assert.equal(plan.layers.length, 2);
  assert.equal(plan.layers[0].sourceTime, ticks(10));
  const { project: p, item: a } = fixture();
  const b = addAsset(p, { ...p.assets[0], id: 'asset-b' }, { trackId: p.timeline.tracks[0].id });
  assert.throws(
    () =>
      command(p, {
        action: 'add_transition',
        fromItemId: a.id,
        toItemId: b.id,
        templateId: 'wipe',
        durationSeconds: 1
      }),
    /余量/
  );
});
test('strict projection validation rejects unknown owners, properties and unsupported time domains', () => {
  const { project, item } = fixture();
  const bad = clone(project);
  bad.unrecognized = true;
  assert.throws(() => validateProject(bad), /未知字段/);
  assert.throws(
    () =>
      command(project, {
        action: 'set_keyframe',
        itemId: item.id,
        property: 'visual.fitPolicy',
        timeSeconds: 0,
        value: 'cover'
      }),
    /不支持/
  );
  const copy = clone(project);
  copy.timeline.tracks[0].items[0].clip.automation = {
    'visual.opacity': { timeDomain: 'source', keyframes: [] }
  };
  assert.throws(() => validateProject(copy), /局部时间/);
  assert.throws(
    () =>
      command(project, {
        action: 'set_transform',
        itemId: item.id,
        crop: { left: 0.8, right: 0.8 }
      }),
    /可见画面/
  );
});
test('model remains ephemeral version 1 and exact same asset metadata remains accepted', () => {
  const { project } = fixture();
  assert.equal(project.version, 1);
  project.assets[0].sourceIdentity = '123';
  project.assets[0].firstTimestamp = 0.12;
  project.assets[0].streams = [{ codec_type: 'video' }];
  validateProject(project);
  const result = command(project, {
    action: 'add_text',
    content: '字幕',
    startSeconds: 1,
    durationSeconds: 2
  });
  assert.equal(result.timeline.tracks[0].items[0].clip.type, 'text');
});
test('eased keyframe slicing preserves the authored curve instead of restarting easing', () => {
  let { project, item } = fixture();
  project = command(
    project,
    {
      action: 'set_keyframe',
      itemId: item.id,
      property: 'visual.opacity',
      timeSeconds: 0,
      value: 0,
      interpolation: 'easeIn'
    },
    {
      action: 'set_keyframe',
      itemId: item.id,
      property: 'visual.opacity',
      timeSeconds: 10,
      value: 1
    }
  );
  const original = project.timeline.tracks[0].items[0];
  project = command(project, { action: 'split_clip', itemId: item.id, atSeconds: 4 });
  for (const sample of [1, 2, 3, 4, 5, 6, 8, 9]) {
    const target = project.timeline.tracks[0].items.find(
      (i) => i.placement.begin <= ticks(sample) && i.placement.end > ticks(sample)
    );
    assert.ok(
      Math.abs(
        evaluateVisual(target, ticks(sample)).opacity -
          evaluateVisual(original, ticks(sample)).opacity
      ) < 1e-9
    );
  }
});
test('gesture draft uses a fixed baseline, cancels and invalidates at authored version changes', async () => {
  const { EditTransaction } = await import('../packages/core/project.mjs');
  const { project, item } = fixture();
  const draft = new EditTransaction();
  draft.begin({ project, version: 3 });
  draft.update([{ action: 'move_clip', itemId: item.id, startSeconds: 2 }]);
  draft.update([{ action: 'move_clip', itemId: item.id, startSeconds: 3 }]);
  assert.equal(draft.project.timeline.tracks[0].items[0].placement.begin, ticks(3));
  assert.equal(item.placement.begin, 0);
  assert.equal(draft.commit().operations.length, 1);
  assert.equal(draft.active, false);
  draft.begin({ project, version: 3 });
  assert.equal(draft.invalidate(4), true);
  assert.throws(() => draft.commit(), /失效/);
  draft.begin({ project, version: 3 });
  draft.update([{ action: 'move_clip', itemId: item.id, startSeconds: 2 }]);
  assert.throws(() => draft.update([{ action: 'set_transform', itemId: item.id, opacity: 2 }]));
  assert.equal(draft.project.timeline.tracks[0].items[0].placement.begin, ticks(2));
  assert.equal(draft.cancel().timeline.tracks[0].items[0].placement.begin, 0);
});
