import test from 'node:test';
import assert from 'node:assert/strict';
import {
  createProject, addAsset, addText, addHtmlClip, ticks, clone, validateProject,
  validateHtmlContent, evaluateFrame, editTimeline, CommandHistory, findItem,
  isPropertyApplicable, TEMPLATE_PARTS, HTML_MAX_BYTES
} from '../packages/core/project.mjs';

const animation = (patch = {}) => ({
  html: '<main id="title">VideoCut</main><script>window.__videocut = { tick(t) { document.querySelector("main").style.opacity = String(t / 4); } };</script>',
  width: 640, height: 360, duration: ticks(8), transparent: true,
  variables: { title: '标题', count: 3, reveal: true }, ...patch
});
const media = (kind, durationSeconds = 8) => ({
  id: `asset-${kind}`, name: `${kind}.file`, kind, path: `/tmp/${kind}.file`,
  duration: ticks(durationSeconds), size: 10,
  width: kind === 'audio' ? 0 : 640, height: kind === 'audio' ? 0 : 360,
  hasAudio: kind !== 'image'
});

test('HTML clips are authored visual layers on video tracks mixed with media, with no fake audio', () => {
  const project = createProject();
  const video = addAsset(project, media('video'));
  const sound = addAsset(project, media('audio'));
  const content = animation();
  const html = addHtmlClip(project, { html: content, name: '标题动画' });
  assert.equal(project.timeline.tracks[0].name, 'HTML 动画');
  assert.equal(project.timeline.tracks[0].type, 'video');
  assert.equal(html.clip.assetId, '');
  assert.deepEqual(html.clip.html, content);
  assert.notEqual(html.clip.html, content);
  assert.notEqual(html.clip.html.variables, content.variables);
  assert.equal(project.assets.length, 2);
  const frame = evaluateFrame(project, ticks(2));
  assert.deepEqual(frame.layers.map((layer) => layer.itemId), [video.id, html.id]);
  assert.deepEqual(frame.audio.map((layer) => layer.itemId).sort(), [sound.id, video.id].sort());
  assert.equal(frame.layers.at(-1).sourceTime, ticks(2));
  assert.equal(frame.layers.at(-1).item.clip.html.transparent, true);
  const trackId = project.timeline.tracks[0].id;
  const image = addAsset(project, media('image'), { trackId, start: ticks(8) });
  const second = addHtmlClip(project, { html: animation(), start: ticks(13), trackId });
  assert.deepEqual(project.timeline.tracks[0].items.map((item) => item.clip.type), ['html-clip', 'image', 'html-clip']);
  assert.equal(evaluateFrame(project, ticks(9)).layers.at(-1).itemId, image.id);
  assert.equal(evaluateFrame(project, ticks(14)).layers.at(-1).itemId, second.id);
  validateProject(project);
});

test('HTML trim, split, speed and backwards frame evaluation preserve the authored source clock', () => {
  const original = createProject();
  const item = addHtmlClip(original, { html: animation(), start: ticks(2) });
  assert.equal(isPropertyApplicable(original, item, 'time.sourceIn'), true);
  assert.equal(isPropertyApplicable(original, item, 'time.speed'), true);
  assert.equal(isPropertyApplicable(original, item, 'audio.gainLinear'), false);
  const trimmed = editTimeline(original, [
    { action: 'trim_clip', itemId: item.id, sourceInSeconds: 2, durationSeconds: 6 },
    { action: 'set_speed', itemId: item.id, rate: 2 }
  ]).project;
  const beforeSplit = evaluateFrame(trimmed, ticks(4.5)).layers[0];
  assert.equal(beforeSplit.sourceTime, ticks(7));
  const split = editTimeline(trimmed, [{ action: 'split_clip', itemId: item.id, atSeconds: 3 }]);
  const rightId = split.operations[0].rightItemId;
  const left = findItem(split.project, item.id).item;
  const right = findItem(split.project, rightId).item;
  assert.deepEqual(left.clip.source, { begin: ticks(2), end: ticks(4) });
  assert.deepEqual(right.clip.source, { begin: ticks(4), end: ticks(8) });
  assert.deepEqual(right.clip.html, left.clip.html);
  assert.notEqual(right.clip.html, left.clip.html);
  assert.notEqual(right.clip.html.variables, left.clip.html.variables);
  assert.equal(evaluateFrame(split.project, ticks(4.5)).layers[0].sourceTime, beforeSplit.sourceTime);
  assert.equal(evaluateFrame(split.project, ticks(2.5)).layers[0].sourceTime, ticks(3));
  assert.equal(evaluateFrame(split.project, ticks(4)).layers[0].sourceTime, ticks(6));
  const shifted = editTimeline(split.project, [{ action: 'move_clip', itemId: rightId, startSeconds: 5 }]).project;
  assert.equal(evaluateFrame(shifted, ticks(6.5)).layers[0].sourceTime, ticks(7));
  assert.equal(original.timeline.tracks[0].items[0].clip.source.begin, 0);
});

test('HTML commands use shared atomic history for payload replacement, split, duplicate, undo and redo', () => {
  let project = createProject();
  const history = new CommandHistory();
  const apply = (operations) => {
    const candidate = history.prepare(project, operations);
    history.accept(candidate); project = candidate.project; return candidate;
  };
  const added = apply([{ action: 'add_html_clip', html: animation(), length: ticks(4) }]);
  const itemId = added.operations[0].itemId;
  const original = clone(project);
  const changed = apply([{ action: 'set_html_clip', itemId, name: '更新动画', html: animation({ variables: { title: '更新' }, transparent: false }) }]);
  assert.deepEqual(changed.changes.itemIds, [itemId]);
  assert.ok(changed.changes.impacts.includes('composite'));
  assert.equal(findItem(project, itemId).item.clip.html.variables.title, '更新');
  apply([{ action: 'undo' }]); assert.deepEqual(project, original);
  apply([{ action: 'redo' }]); assert.equal(findItem(project, itemId).item.name, '更新动画');
  const split = apply([{ action: 'split_clip', itemId, atSeconds: 2 }]);
  const rightId = split.operations[0].rightItemId;
  const copies = apply([{ action: 'duplicate_clips', itemIds: [rightId], offsetSeconds: 2 }]);
  const copy = findItem(project, copies.operations[0].itemIds[0]).item;
  assert.notEqual(copy.clip.id, findItem(project, rightId).item.clip.id);
  assert.deepEqual(copy.clip.html, findItem(project, rightId).item.clip.html);
  apply([{ action: 'undo' }]); assert.equal(project.timeline.tracks[0].items.length, 2);
  apply([{ action: 'undo' }]); assert.equal(project.timeline.tracks[0].items.length, 1);
  apply([{ action: 'redo' }]); assert.equal(project.timeline.tracks[0].items.length, 2);
});

test('HTML source extent and track ownership reject invalid edits without publishing partial work', () => {
  const project = createProject();
  const item = addHtmlClip(project, { html: animation() });
  const original = JSON.stringify(project);
  for (const invalid of [
    { action: 'trim_clip', itemId: item.id, sourceInSeconds: 7, durationSeconds: 2 },
    { action: 'trim_range', itemId: item.id, beginSeconds: 0, endSeconds: 9 },
    { action: 'set_html_clip', itemId: item.id, html: animation({ duration: ticks(4) }) },
    { action: 'set_audio', itemId: item.id, gainLinear: 2 }
  ]) {
    assert.throws(() => editTimeline(project, [{ action: 'configure_project', name: 'Must not publish' }, invalid]));
    assert.equal(JSON.stringify(project), original);
  }
  const audio = addAsset(project, media('audio'));
  const audioTrack = project.timeline.tracks.find((track) => track.items.some((clip) => clip.id === audio.id));
  assert.throws(() => addHtmlClip(project, { html: animation(), trackId: audioTrack.id }), /视频轨道/);
  assert.throws(() => addHtmlClip(project, { html: animation(), trackId: 'missing' }), /视频轨道/);
  project.timeline.tracks[0].locked = true;
  assert.throws(() => editTimeline(project, [{ action: 'set_html_clip', itemId: item.id, html: animation() }]), /锁定/);
  assert.throws(() => addHtmlClip(project, { html: animation(), trackId: project.timeline.tracks[0].id }), /视频轨道/);
});

test('HTML validates a bounded primitive document and matching clip payload', () => {
  for (const patch of [
    { html: '' }, { html: '\0' }, { html: '<video src="x"></video>' }, { html: '<AuDiO/>' },
    { html: '标题'.repeat(HTML_MAX_BYTES / 2) },
    { width: Infinity }, { width: 1.5 }, { width: 0 }, { width: 4097 },
    { width: 4096, height: 4096 }, { duration: 0 }, { duration: 1.5 }, { duration: Infinity },
    { transparent: 'true' }, { variables: { nested: {} } }, { variables: { count: NaN } },
    { variables: { constructor: 'x' } }, { variables: null }, { unknown: true }
  ]) assert.throws(() => validateHtmlContent(animation(patch)));
  assert.doesNotThrow(() => validateHtmlContent(animation({ html: '<div data-video="native track">Ready</div>' })));
  const project = createProject();
  const item = addHtmlClip(project, { html: animation() });
  const missingPayload = clone(project); delete missingPayload.timeline.tracks[0].items[0].clip.html;
  assert.throws(() => validateProject(missingPayload), /HTML 动画/);
  const textPayload = clone(project); textPayload.timeline.tracks[0].items[0].clip.text = { content: 'x' };
  assert.throws(() => validateProject(textPayload), /同时包含/);
  const invalidType = clone(project); invalidType.timeline.tracks[0].items[0].clip.type = 'text';
  assert.throws(() => validateProject(invalidType));
  const text = addText(project, { content: '字幕' });
  assert.throws(() => editTimeline(project, [{ action: 'set_html_clip', itemId: text.id, html: animation() }]), /不是 HTML/);
  assert.throws(() => editTimeline(project, [{ action: 'add_html_clip', html: animation(), start: 0.5 }]), /时间范围/);
  assert.throws(() => editTimeline(project, [{ action: 'add_html_clip', html: animation(), length: ticks(9) }]), /时长/);
  assert.equal(item.clip.html.duration, ticks(8));
});

test('same-track video and HTML transitions evaluate source handles using each native source extent', () => {
  const project = createProject();
  const video = addAsset(project, media('video', 12));
  const trimmed = editTimeline(project, [{ action: 'trim_clip', itemId: video.id, sourceInSeconds: 1, durationSeconds: 4 }]).project;
  const trackId = trimmed.timeline.tracks[0].id;
  const html = addHtmlClip(trimmed, { html: animation(), trackId, start: ticks(4), length: ticks(4) });
  const handles = editTimeline(trimmed, [{ action: 'trim_clip', itemId: html.id, sourceInSeconds: 1, durationSeconds: 4 }]).project;
  const transitioned = editTimeline(handles, [{ action: 'add_transition', templateId: 'dissolve', fromItemId: video.id, toItemId: html.id, durationSeconds: 1 }]).project;
  const frame = evaluateFrame(transitioned, ticks(3.75));
  assert.equal(frame.layers.length, 2);
  assert.equal(frame.layers[1].sourceTime, ticks(0.75));
  assert.equal(frame.layers[1].transition.progress, 0.25);
  assert.throws(() => editTimeline(trimmed, [{ action: 'add_transition', templateId: 'dissolve', fromItemId: video.id, toItemId: html.id, durationSeconds: 1 }]), /余量/);
});

test('authored flower template recipes admit only shipped native components and strict fields', () => {
  const project = createProject();
  const template = { id: 'my-flower', version: 1, recipe: {
    base: 'flower-style-38', backdrop: 'bubble-nine-slice', animation: 'anim-lua-letter-transform'
  } };
  const item = addText(project, { content: '定制花字', template });
  assert.deepEqual(item.clip.text.template, template);
  assert.ok(TEMPLATE_PARTS.base.includes(template.recipe.base));
  validateProject(project);
  for (const invalid of [
    { ...template, id: 'bad/id' }, { id: 'unknown', version: 1 },
    { ...template, recipe: { base: 'not-shipped' } },
    { ...template, recipe: { base: 'flower-style-38', backdrop: 'external' } },
    { ...template, recipe: { base: 'flower-style-38', animation: 'external' } },
    { ...template, recipe: { base: 'flower-style-38', script: 'x' } },
    { ...template, recipe: null }
  ]) assert.throws(() => addText(createProject(), { content: '无效', template: invalid }));
});
