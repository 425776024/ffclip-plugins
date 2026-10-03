import {
  clone,
  findItem,
  splitItem,
  removeItem,
  validateProject,
  ticks,
  identity,
  addAsset,
  addText,
  addHtmlClip
} from './project.mjs';
import {
  enrichClip,
  getPropertyDescriptor,
  isPropertyApplicable,
  renewEffectIdentities,
  selectionClosure,
  validateProperty,
  validateHtmlContent,
  writePath,
  sliceAutomation,
  trimSourceRange,
  defaultParameters,
  EFFECT_TEMPLATES,
  TRANSITION_TEMPLATES,
  PROPERTY_DESCRIPTORS
} from './model.mjs';
import { createProjectDraft, documentPatches } from './immutable.mjs';
function time(v, signed = false) {
  if (typeof v !== 'number' || !Number.isFinite(v) || Math.abs(v) > 86400 || (!signed && v < 0))
    throw new Error('秒数无效');
  return ticks(v);
}
function selected(project, ids, expand = true) {
  return selectionClosure(project, ids, { expand }).map((id) => {
    const found = findItem(project, id);
    if (found.track.locked) throw new Error('轨道已锁定');
    return found;
  });
}
function shift(item, delta) {
  item.placement = { begin: item.placement.begin + delta, end: item.placement.end + delta };
}
function rawMove(project, item, track, begin, targetId) {
  const target = targetId ? project.timeline.tracks.find((t) => t.id === targetId) : track;
  if (!target || target.locked || target.type !== track.type) throw new Error('无法移动到该轨道');
  shift(item, begin - item.placement.begin);
  if (target !== track) {
    track.items = track.items.filter((i) => i.id !== item.id);
    target.items.push(item);
  }
}
function retimeCurves(item, oldLength, newLength) {
  for (const binding of Object.values(item.clip.automation ?? {})) {
    const byTime = new Map();
    for (const frame of binding.keyframes) {
      frame.time = Math.round((frame.time * newLength) / oldLength);
      byTime.set(frame.time, frame);
    }
    binding.keyframes = [...byTime.values()].sort((a, b) => a.time - b.time);
  }
  for (const key of ['fadeIn', 'fadeOut'])
    if (item.clip.audio[key])
      item.clip.audio[key] = Math.min(
        newLength,
        Math.round((item.clip.audio[key] * newLength) / oldLength)
      );
}
function template(catalog, id) {
  const value = catalog.find((t) => t.id === id);
  if (!value) throw new Error(`未知模板：${id}`);
  return value;
}
/** Translate linked edits from one immutable clock, avoiding double edits in a multiselection. */
function expandRelatedOperations(project, operations) {
  const result = [],
    addressed = new Map();
  let relationships = project;
  for (const operation of operations) {
    let op = { ...operation };
    if (['group_clips', 'link_clips', 'ungroup_clips', 'unlink_clips'].includes(op.action)) {
      const field = ['group_clips', 'ungroup_clips'].includes(op.action) ? 'groups' : 'links';
      let entries = relationships.timeline[field] ?? [];
      if (op.action.startsWith('un')) entries = entries.filter((entry) => entry.id !== op.groupId);
      else {
        const ids = selectionClosure(relationships, op.itemIds, {
          expand: op.expandLinked !== false
        });
        entries = entries
          .map((entry) => ({ ...entry, itemIds: entry.itemIds.filter((id) => !ids.includes(id)) }))
          .filter((entry) => entry.itemIds.length > 1);
        entries.push({ id: `pending-${field}-${result.length}`, itemIds: ids });
      }
      relationships = {
        ...relationships,
        timeline: { ...relationships.timeline, [field]: entries }
      };
    }
    if (op.action === 'remove_clip')
      op = { action: 'delete_clips', itemIds: [op.itemId], expandLinked: op.expandLinked };
    if (op.action === 'set_property' && op.property?.startsWith('time.')) {
      validateProperty(op.property, op.value);
      const item = findItem(project, op.itemId).item;
      if (!isPropertyApplicable(project, item, op.property))
        throw new Error('片段不支持此时间属性');
      if (op.property === 'time.start')
        op = { action: 'move_clip', itemId: op.itemId, startSeconds: op.value / 120000 };
      else if (op.property === 'time.duration')
        op = {
          action: 'trim_range',
          itemId: op.itemId,
          beginSeconds: item.placement.begin / 120000,
          endSeconds: (item.placement.begin + op.value) / 120000
        };
      else if (op.property === 'time.sourceIn')
        op = {
          action: 'trim_clip',
          itemId: op.itemId,
          sourceInSeconds: op.value / 120000,
          durationSeconds: (item.placement.end - item.placement.begin) / 120000
        };
      else
        op = { action: 'set_speed', itemId: op.itemId, rate: op.value, ripple: op.ripple ?? true };
    }
    if (
      !op.itemId ||
      op.expandLinked === false ||
      !['move_clip', 'trim_clip', 'trim_range', 'trim_edges', 'set_speed', 'split_clip'].includes(
        op.action
      )
    ) {
      result.push(op);
      continue;
    }
    const anchor = findItem(project, op.itemId).item;
    const ids = selectionClosure(relationships, [op.itemId], { groups: op.action === 'move_clip' });
    if (op.action === 'move_clip' && ids.length > 1) {
      if (op.trackId && op.trackId !== findItem(project, op.itemId).track.id)
        throw new Error('关联片段跨轨移动请使用多选轨道映射');
      const key = `move_closure:${[...ids].sort().join(',')}`;
      const deltaSeconds = op.startSeconds - anchor.placement.begin / 120000;
      if (addressed.has(key)) {
        if (addressed.get(key) !== deltaSeconds) throw new Error('分组或关联片段收到冲突移动');
        continue;
      }
      addressed.set(key, deltaSeconds);
      result.push({ action: 'move_clips', itemIds: ids, deltaSeconds });
      continue;
    }
    for (const id of ids) {
      const target = findItem(project, id).item;
      let next = { ...op, itemId: id, expandLinked: false };
      if (op.action === 'trim_clip' && id !== op.itemId) {
        const anchorRate = anchor.clip.retime.constantRatePpm / 1e6;
        const offset = (time(op.sourceInSeconds) - anchor.clip.source.begin) / anchorRate;
        next.sourceInSeconds =
          (target.clip.source.begin +
            Math.round((offset * target.clip.retime.constantRatePpm) / 1e6)) /
          120000;
      } else if (['trim_range', 'trim_edges'].includes(op.action) && id !== op.itemId) {
        next.beginSeconds =
          (target.placement.begin + time(op.beginSeconds) - anchor.placement.begin) / 120000;
        next.endSeconds =
          (target.placement.end + time(op.endSeconds) - anchor.placement.end) / 120000;
      } else if (
        op.action === 'split_clip' &&
        (time(op.atSeconds) <= target.placement.begin || time(op.atSeconds) >= target.placement.end)
      ) {
        throw new Error('关联分割点必须位于所有关联片段内部');
      } else if (
        op.action === 'set_speed' &&
        !['video', 'audio', 'html-clip'].includes(target.clip.type)
      ) {
        throw new Error('关联变速只能包含视频、音频和 HTML 动画片段');
      }
      const key = `${op.action}:${id}`,
        previous = addressed.get(key);
      if (previous) {
        if (JSON.stringify(previous) !== JSON.stringify(next))
          throw new Error('关联片段收到冲突编辑');
        continue;
      }
      addressed.set(key, next);
      result.push(next);
    }
  }
  return result;
}
/** Commands operate on one unpublished candidate; only the final state is validated.
 * @param {import('./project.mjs').Project} project
 * @param {import('./commands.js').EditorCommand[]} operations
 */
/** @type {(project: import('./types.js').Project,operations: import('./commands.js').EditorCommand[]) => { project: import('./types.js').Project; operations: import('./types.js').CommandResult[]; changes: { itemIds: string[]; trackIds: string[]; impacts: string[] }; patches: import('./immutable.mjs').DocumentPatch[]; inversePatches: import('./immutable.mjs').DocumentPatch[] }} */
export function editTimeline(project, operations) {
  validateProject(project);
  if (!Array.isArray(operations) || !operations.length || operations.length > 100)
    throw new Error('提供 1–100 项剪辑操作');
  operations = expandRelatedOperations(project, operations);
  const draftState = createProjectDraft(project);
  let next = draftState.draft;
  const results = [];
  next.timeline.groups ??= [];
  next.timeline.links ??= [];
  next.timeline.transitions ??= [];
  for (const op of operations) {
    if (!op || typeof op !== 'object' || typeof op.action !== 'string')
      throw new Error('剪辑命令无效');
    let result = { action: op.action };
    if (op.action === 'configure_project') {
      if (op.name !== undefined) next.name = op.name;
      if (op.width !== undefined) next.canvas.width = op.width;
      if (op.height !== undefined) next.canvas.height = op.height;
      if (op.frameRate !== undefined) next.frameRate = clone(op.frameRate);
      else if (op.fps !== undefined) next.frameRate = { numerator: op.fps, denominator: 1 };
    } else if (['add_media', 'add_asset'].includes(op.action)) {
      result.itemId = addAsset(next, op.asset, {
        start: op.startSeconds === undefined ? undefined : time(op.startSeconds),
        trackId: op.trackId,
        validate: false
      }).id;
    } else if (op.action === 'add_text') {
      result.itemId = addText(next, {
        ...op,
        start: time(op.startSeconds ?? 0),
        length: time(op.durationSeconds ?? 5),
        validate: false
      }).id;
    } else if (op.action === 'add_subtitles') {
      if (!Array.isArray(op.segments) || !op.segments.length || op.segments.length > 10000)
        throw new Error('请提供 1–10000 条字幕');
      const track = {
        id: identity('track'),
        name: op.name || '语音字幕',
        type: 'text',
        visible: true,
        muted: false,
        locked: false,
        items: []
      };
      next.timeline.tracks.unshift(track);
      const itemIds = [];
      let previousEnd = 0;
      for (const segment of op.segments) {
        if (
          typeof segment?.text !== 'string' ||
          !segment.text.trim() ||
          segment.text.length > 1000 ||
          !Number.isFinite(segment.start) ||
          !Number.isFinite(segment.end) ||
          segment.start < 0 ||
          segment.end <= segment.start ||
          segment.end > 86400 ||
          time(segment.start) < previousEnd ||
          time(segment.end) <= time(segment.start)
        )
          throw new Error('字幕文字、时间或顺序无效');
        const item = addText(next, {
          content: segment.text.trim(),
          start: time(segment.start),
          length: time(segment.end) - time(segment.start),
          fontSize: Math.max(24, Math.round(next.canvas.height * 0.05)),
          trackId: track.id,
          validate: false
        });
        item.clip.visual.positionY = Math.round(next.canvas.height * 0.37);
        itemIds.push(item.id);
        previousEnd = time(segment.end);
      }
      result.itemIds = itemIds;
    } else if (op.action === 'add_html_clip') {
      result.itemId = addHtmlClip(next, { ...op, validate: false }).id;
    } else if (
      [
        'move_clips',
        'duplicate_clips',
        'group_clips',
        'link_clips',
        'ripple_delete',
        'delete_clips'
      ].includes(op.action)
    ) {
      const selection = selected(next, op.itemIds, op.expandLinked !== false);
      const ids = selection.map((v) => v.item.id);
      result.itemIds = ids;
      if (op.action === 'move_clips') {
        const delta = time(op.deltaSeconds, true);
        for (const { item, track } of selection)
          rawMove(next, item, track, item.placement.begin + delta, op.trackId);
      } else if (op.action === 'duplicate_clips') {
        const offset =
          op.offsetSeconds === undefined
            ? Math.max(...selection.map((v) => v.item.placement.end)) -
              Math.min(...selection.map((v) => v.item.placement.begin))
            : time(op.offsetSeconds, true);
        const copies = new Map();
        for (const { item, track } of selection) {
          const copy = clone(item);
          copy.id = identity('item');
          copy.clip.id = identity('clip');
          for (const binding of Object.values(copy.clip.automation))
            for (const frame of binding.keyframes) frame.id = identity('keyframe');
          renewEffectIdentities(copy.clip);
          shift(copy, offset);
          track.items.push(copy);
          copies.set(item.id, copy.id);
        }
        for (const field of ['groups', 'links'])
          for (const entry of [...next.timeline[field]])
            if (entry.itemIds.every((id) => copies.has(id)))
              next.timeline[field].push({
                id: identity(field === 'groups' ? 'group' : 'link'),
                itemIds: entry.itemIds.map((id) => copies.get(id))
              });
        for (const transition of [...next.timeline.transitions])
          if (copies.has(transition.fromItemId) && copies.has(transition.toItemId))
            next.timeline.transitions.push({
              ...clone(transition),
              id: identity('transition'),
              fromItemId: copies.get(transition.fromItemId),
              toItemId: copies.get(transition.toItemId)
            });
        result.itemIds = [...copies.values()];
      } else if (op.action === 'group_clips' || op.action === 'link_clips') {
        const field = op.action === 'group_clips' ? 'groups' : 'links';
        if (ids.length < 2) throw new Error('至少选择两个片段');
        next.timeline[field] = next.timeline[field]
          .map((g) => ({ ...g, itemIds: g.itemIds.filter((id) => !ids.includes(id)) }))
          .filter((g) => g.itemIds.length > 1);
        const entry = { id: identity(field === 'groups' ? 'group' : 'link'), itemIds: ids };
        next.timeline[field].push(entry);
        result.groupId = entry.id;
      } else {
        if (op.action === 'ripple_delete') {
          const tracks =
            op.scope === 'all'
              ? next.timeline.tracks
              : next.timeline.tracks.filter(
                  (t) =>
                    selection.some((v) => v.track.id === t.id) ||
                    (op.scope === 'syncLocked' && t.syncLocked)
                );
          const globalRanges = selection
            .map((v) => v.item.placement)
            .sort((a, b) => a.begin - b.begin);
          for (const track of tracks) {
            if (track.locked) throw new Error('波纹编辑遇到锁定轨道');
            const source =
              op.scope === 'all' || op.scope === 'syncLocked'
                ? globalRanges
                : selection
                    .filter((v) => v.track.id === track.id)
                    .map((v) => v.item.placement)
                    .sort((a, b) => a.begin - b.begin);
            const ranges = [];
            for (const range of source) {
              const prev = ranges.at(-1);
              if (prev && range.begin <= prev.end) prev.end = Math.max(prev.end, range.end);
              else ranges.push({ ...range });
            }
            for (const item of track.items)
              if (!ids.includes(item.id)) {
                if (
                  ranges.some((r) => item.placement.begin < r.end && item.placement.end > r.begin)
                )
                  throw new Error('波纹删除范围穿过未选片段');
                shift(
                  item,
                  -ranges.reduce(
                    (sum, r) => sum + (r.end <= item.placement.begin ? r.end - r.begin : 0),
                    0
                  )
                );
              }
          }
        }
        for (const id of ids) removeItem(next, id);
      }
    } else if (['ungroup_clips', 'unlink_clips'].includes(op.action)) {
      const field = op.action === 'ungroup_clips' ? 'groups' : 'links';
      const group = next.timeline[field].find((g) => g.id === (op.groupId ?? op.linkId));
      if (!group) throw new Error('分组或关联不存在');
      selected(next, group.itemIds, false);
      next.timeline[field] = next.timeline[field].filter((g) => g !== group);
    } else if (op.action === 'remove_transition' || op.action === 'update_transition') {
      const t = next.timeline.transitions.find((t) => t.id === op.transitionId);
      if (!t) throw new Error('转场不存在');
      selected(next, [t.fromItemId, t.toItemId], false);
      if (op.action === 'remove_transition')
        next.timeline.transitions = next.timeline.transitions.filter((x) => x !== t);
      else {
        if (op.durationSeconds !== undefined) t.duration = time(op.durationSeconds);
        if (op.parameters) Object.assign(t.parameters, op.parameters);
      }
    } else if (op.action === 'add_transition') {
      selected(next, [op.fromItemId, op.toItemId], false);
      const definition = template(TRANSITION_TEMPLATES, op.templateId);
      const transition = {
        id: identity('transition'),
        fromItemId: op.fromItemId,
        toItemId: op.toItemId,
        templateId: op.templateId,
        duration: time(op.durationSeconds ?? 1),
        parameters: { ...defaultParameters(definition), ...op.parameters }
      };
      next.timeline.transitions.push(transition);
      result.transitionId = transition.id;
    } else if (op.action === 'set_track') {
      const track = next.timeline.tracks.find((t) => t.id === op.trackId);
      if (!track) throw new Error('轨道不存在');
      for (const key of ['visible', 'muted', 'locked', 'syncLocked', 'name'])
        if (op[key] !== undefined) track[key] = op[key];
    } else {
      const { item, track } = findItem(next, op.itemId);
      if (track.locked) throw new Error('轨道已锁定');
      enrichClip(item.clip);
      result.itemId = item.id;
      switch (op.action) {
        case 'trim_clip': {
          const source = time(op.sourceInSeconds),
            length = time(op.durationSeconds);
          if (!length) throw new Error('时长必须大于零');
          const old = clone(item),
            rate = item.clip.retime.constantRatePpm / 1e6;
          const localBegin = (source - old.clip.source.begin) / rate;
          item.clip.source = { begin: source, end: source + Math.round(length * rate) };
          item.placement.end = item.placement.begin + length;
          item.clip.automation = sliceAutomation(
            old,
            Math.round(localBegin),
            Math.round(localBegin) + length
          );
          for (const key of ['fadeIn', 'fadeOut'])
            item.clip.audio[key] = Math.min(item.clip.audio[key], length);
          break;
        }
        case 'trim_range':
        case 'trim_edges': {
          const begin = time(op.beginSeconds),
            end = time(op.endSeconds),
            old = clone(item);
          item.clip.source = trimSourceRange(old, begin, end);
          item.clip.automation = sliceAutomation(
            old,
            begin - old.placement.begin,
            end - old.placement.begin
          );
          item.placement = { begin, end };
          for (const key of ['fadeIn', 'fadeOut'])
            item.clip.audio[key] = Math.min(item.clip.audio[key], end - begin);
          break;
        }
        case 'move_clip':
          rawMove(next, item, track, time(op.startSeconds), op.trackId);
          break;
        case 'split_clip':
          result.rightItemId = splitItem(next, item.id, time(op.atSeconds)).id;
          break;
        case 'remove_clip':
          removeItem(next, item.id);
          break;
        case 'set_transform':
          for (const property of Object.keys(PROPERTY_DESCRIPTORS).filter((p) =>
            p.startsWith('visual.')
          )) {
            const key = property.slice(7),
              value = key.startsWith('crop.') ? op.crop?.[key.slice(5)] : op[key];
            if (value !== undefined) {
              validateProperty(property, value);
              writePath(item.clip, property, value);
            }
          }
          break;
        case 'set_property':
          validateProperty(op.property, op.value, item.clip);
          writePath(item.clip, op.property, op.value);
          if (op.property === 'text.content') item.name = op.value.slice(0, 80);
          break;
        case 'set_audio':
          for (const key of ['gainLinear', 'muted', 'fadeIn', 'fadeOut'])
            if (op[key] !== undefined) {
              validateProperty(`audio.${key}`, op[key]);
              item.clip.audio[key] = op[key];
            }
          break;
        case 'set_text':
          if (!item.clip.text) throw new Error('这不是文字片段');
          if (op.layoutWidth === null) delete item.clip.text.layoutWidth;
          else if (op.layoutWidth !== undefined) {
            if (!Number.isFinite(op.layoutWidth) || op.layoutWidth < 1 || op.layoutWidth > 65536)
              throw new Error('文字框宽度必须为 1–65536');
            item.clip.text.layoutWidth = op.layoutWidth;
          }
          if (op.template !== undefined) {
            if (op.template === null) {
              const style = item.clip.text.template?.style;
              if (style?.color !== undefined) item.clip.text.color = style.color;
              if (style?.fontSize !== undefined) item.clip.text.fontSize = style.fontSize;
              delete item.clip.text.template;
            }
            else {
              const style = op.template.style ?? item.clip.text.template?.style ?? {
                ...(item.clip.text.color !== '#ffffff' ? { color: item.clip.text.color } : {}),
                ...(item.clip.text.fontSize !== 64 ? { fontSize: item.clip.text.fontSize } : {})
              };
              item.clip.text.template = clone(op.template);
              if (Object.keys(style).length) item.clip.text.template.style = clone(style);
              // Plain-text defaults are not template overrides. Keep existing
              // templates unchanged until an explicit authored style is supplied.
              item.clip.text.fontSize = 64;
              item.clip.text.color = '#ffffff';
              item.clip.text.fontFamily = 'system';
              delete item.clip.text.font;
            }
          }
          if (op.templateStyle !== undefined) {
            if (!item.clip.text.template || !op.templateStyle || typeof op.templateStyle !== 'object' || Array.isArray(op.templateStyle))
              throw new Error('模板文字样式无效');
            const style = { ...item.clip.text.template.style };
            for (const [key, value] of Object.entries(op.templateStyle)) {
              if (!['color', 'fontSize'].includes(key)) throw new Error('模板文字样式属性无效');
              if (value === null) delete style[key];
              else { validateProperty(`text.${key}`, value); style[key] = value; }
            }
            if (Object.keys(style).length) item.clip.text.template.style = style;
            else delete item.clip.text.template.style;
            delete item.clip.text.template.packageDigest;
          }
          for (const key of ['content', 'fontSize', 'color'])
            if (op[key] !== undefined) item.clip.text[key] = op[key];
          item.name = item.clip.text.content.slice(0, 80) || '文字';
          break;
        case 'set_html_clip':
          if (item.clip.type !== 'html-clip') throw new Error('这不是 HTML 动画片段');
          validateHtmlContent(op.html);
          item.clip.html = clone(op.html);
          if (op.name !== undefined) item.name = op.name;
          break;
        case 'set_speed': {
          const ppm = op.constantRatePpm ?? Math.round(op.rate * 1e6);
          if (!Number.isSafeInteger(ppm) || ppm < 100000 || ppm > 10000000)
            throw new Error('速度必须为0.1–10');
          if (!['video', 'audio', 'html-clip'].includes(item.clip.type))
            throw new Error('只有视频、音频和 HTML 动画可以变速');
          const oldLength = item.placement.end - item.placement.begin;
          const length = Math.round(((item.clip.source.end - item.clip.source.begin) * 1e6) / ppm);
          item.clip.retime = { version: 1, mode: 'constant', constantRatePpm: ppm };
          item.placement.end = item.placement.begin + length;
          retimeCurves(item, oldLength, length);
          if (op.ripple)
            for (const follower of track.items)
              if (
                follower.id !== item.id &&
                follower.placement.begin >= item.placement.begin + oldLength
              )
                shift(follower, length - oldLength);
          break;
        }
        case 'set_keyframe': {
          if (!getPropertyDescriptor(item, op.property)?.keyframe)
            throw new Error('属性不支持关键帧');
          validateProperty(op.property, op.value, item.clip);
          const at = time(op.timeSeconds),
            binding = (item.clip.automation[op.property] ??= {
              timeDomain: 'itemLocal',
              keyframes: []
            });
          const existing = binding.keyframes.find((f) => f.time === at);
          const frame = {
            id: existing?.id ?? identity('keyframe'),
            time: at,
            value: op.value,
            interpolation: op.interpolation ?? 'linear'
          };
          binding.keyframes = binding.keyframes.filter((f) => f.time !== at);
          binding.keyframes.push(frame);
          binding.keyframes.sort((a, b) => a.time - b.time);
          result.keyframeId = frame.id;
          break;
        }
        case 'remove_keyframe': {
          const binding = item.clip.automation[op.property];
          if (!binding || !binding.keyframes.some((f) => f.id === op.keyframeId))
            throw new Error('关键帧不存在');
          binding.keyframes = binding.keyframes.filter((f) => f.id !== op.keyframeId);
          if (!binding.keyframes.length) delete item.clip.automation[op.property];
          break;
        }
        case 'add_effect': {
          const definition = template(EFFECT_TEMPLATES, op.templateId),
            effect = {
              id: identity('effect'),
              templateId: definition.id,
              enabled: true,
              parameters: { ...defaultParameters(definition), ...op.parameters }
            };
          item.clip.effects.push(effect);
          result.effectId = effect.id;
          break;
        }
        case 'update_effect': {
          const effect = item.clip.effects.find((e) => e.id === op.effectId);
          if (!effect) throw new Error('特效不存在');
          if (op.enabled !== undefined) effect.enabled = op.enabled;
          if (op.parameters) Object.assign(effect.parameters, op.parameters);
          break;
        }
        case 'remove_effect':
          if (!item.clip.effects.some((e) => e.id === op.effectId)) throw new Error('特效不存在');
          item.clip.effects = item.clip.effects.filter((e) => e.id !== op.effectId);
          for (const path of Object.keys(item.clip.automation))
            if (path.startsWith('effects.') && path.slice(8, path.lastIndexOf('.')) === op.effectId)
              delete item.clip.automation[path];
          break;
        case 'reorder_effects':
          if (
            !Array.isArray(op.effectIds) ||
            op.effectIds.length !== item.clip.effects.length ||
            new Set(op.effectIds).size !== op.effectIds.length ||
            op.effectIds.some((id) => !item.clip.effects.some((e) => e.id === id))
          )
            throw new Error('特效排序无效');
          item.clip.effects = op.effectIds.map((id) => item.clip.effects.find((e) => e.id === id));
          break;
        case 'reorder_track':
          if (
            !Number.isInteger(op.index) ||
            op.index < 0 ||
            op.index >= next.timeline.tracks.length
          )
            throw new Error('轨道顺序无效');
          next.timeline.tracks = next.timeline.tracks.filter((t) => t.id !== track.id);
          next.timeline.tracks.splice(op.index, 0, track);
          break;
        default:
          throw new Error(`未知操作：${op.action}`);
      }
    }
    results.push(result);
  }
  const temporalActions = new Set([
    'add_media',
    'add_asset',
    'add_text',
    'add_subtitles',
    'add_html_clip',
    'move_clips',
    'move_clip',
    'duplicate_clips',
    'trim_clip',
    'trim_range',
    'trim_edges',
    'set_speed',
    'split_clip',
    'ripple_delete'
  ]);
  if (operations.some((op) => temporalActions.has(op.action)))
    for (const track of next.timeline.tracks)
      track.items.sort((a, b) => a.placement.begin - b.placement.begin);
  const splitRights = new Map(
    results.filter((r) => r.rightItemId).map((r) => [r.itemId, r.rightItemId])
  );
  if (splitRights.size) {
    const rights = new Set(splitRights.values()),
      additions = [];
    for (const link of next.timeline.links) {
      link.itemIds = link.itemIds.filter((id) => !rights.has(id));
      const rightIds = link.itemIds.map((id) => splitRights.get(id)).filter(Boolean);
      if (rightIds.length > 1) additions.push({ id: identity('link'), itemIds: rightIds });
    }
    next.timeline.links = [
      ...next.timeline.links.filter((link) => link.itemIds.length > 1),
      ...additions
    ];
  }
  next = draftState.finish();
  validateProject(next);
  const before = new Map(
      project.timeline.tracks.flatMap((t) => t.items.map((i) => [i.id, { trackId: t.id, item: i }]))
    ),
    after = new Map(
      next.timeline.tracks.flatMap((t) => t.items.map((i) => [i.id, { trackId: t.id, item: i }]))
    );
  const changedItemIds = [...new Set([...before.keys(), ...after.keys()])].filter((id) => {
    const a = before.get(id),
      b = after.get(id);
    return (
      a?.trackId !== b?.trackId || (a?.item !== b?.item && JSON.stringify(a) !== JSON.stringify(b))
    );
  });
  return {
    project: next,
    operations: results,
    patches: documentPatches(project, next),
    inversePatches: documentPatches(next, project),
    changes: {
      itemIds: changedItemIds,
      impacts: [
        ...new Set(
          operations.flatMap((op) =>
            op.action === 'set_property' ||
            op.action === 'set_keyframe' ||
            op.action === 'remove_keyframe'
              ? (after.get(op.itemId)?.item ?? before.get(op.itemId)?.item)
                ? (getPropertyDescriptor(
                    after.get(op.itemId)?.item ?? before.get(op.itemId)?.item,
                    op.property
                  )?.impact ?? [])
                : ['media', 'layout', 'effect', 'composite', 'audio']
              : op.action === 'set_audio'
                ? ['audio']
                : ['add_effect', 'update_effect', 'remove_effect', 'reorder_effects'].includes(
                      op.action
                    )
                  ? ['effect', 'composite']
                  : op.action === 'set_transform'
                    ? ['layout', 'composite']
                    : ['media', 'layout', 'effect', 'composite', 'audio']
          )
        )
      ],
      trackIds: next.timeline.tracks
        .filter((t) => {
          const old = project.timeline.tracks.find((x) => x.id === t.id);
          return t !== old;
        })
        .map((t) => t.id)
    }
  };
}
