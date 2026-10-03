/** @typedef {import('./types.js').AssetKind} AssetKind */
/** @typedef {import('./types.js').Asset} Asset */
/** @typedef {import('./types.js').Item} Item */
/** @typedef {import('./types.js').Track} Track */
/** @typedef {import('./types.js').Project} Project */
/** @typedef {import('./types.js').TextContent} TextContent */
/** @typedef {import('./types.js').TextTemplate} TextTemplate */
/** @typedef {import('./types.js').VisualProperties} VisualProperties */
/** @typedef {import('./types.js').AudioProperties} AudioProperties */
/** @typedef {import('./types.js').Keyframe} Keyframe */
/** @typedef {import('./types.js').AutomationBinding} AutomationBinding */
/** @typedef {import('./types.js').EffectInstance} EffectInstance */
/** @typedef {import('./types.js').TransitionInstance} TransitionInstance */
/** @typedef {import('./types.js').ItemRelation} ItemRelation */
/** @typedef {import('./types.js').PropertyDescriptor} PropertyDescriptor */
/** @typedef {import('./types.js').EffectTemplate} EffectTemplate */
/** @typedef {import('./types.js').CommandResult} CommandResult */
/** @typedef {import('./types.js').FrameLayer} FrameLayer */
/** @typedef {import('./commands.js').EditorCommand} EditorCommand */
import {
  enrichClip,
  validateHtmlContent,
  validateExtensions,
  mapTimelineToSource,
  trimSourceRange,
  sliceAutomation,
  renewEffectIdentities
} from './model.mjs';
import { recipes, TEMPLATE_PARTS, assertRecipeCombination } from '../text-wasm/src/recipes.mjs';
export { TEMPLATE_PARTS, TEMPLATE_COMPOSITION_RULES } from '../text-wasm/src/recipes.mjs';
export const TEXT_TEMPLATES = recipes.map(({ id, name, tag, text, timeUs, category }) => ({
  id,
  name,
  tag,
  text,
  timeUs,
  category
}));
export const TEMPLATE_EXPORT_NOTICE = 'MP4 按作品分辨率逐帧渲染；导出期间请保持页面打开。';
/** @type {(project: import('./types.js').Project) => void} */
export function assertTextExportSupported(project) {
  if (
    project.timeline.tracks.some((t) => t.items.some((i) => i.clip.text?.template)) &&
    (project.canvas.width > 4096 ||
      project.canvas.height > 4096 ||
      project.canvas.width * project.canvas.height > 8388608)
  )
    throw new Error('复杂模板导出支持单边不超过 4096、总计不超过 8388608 像素的画布');
}
/** VideoCut authored editing model. Time values use the native 120,000 Hz clock. */
export const TIME_BASE = 120000;
export const FORMAT = 'videocut.edit-session';
/** @type {(seconds: number) => number} */
export const ticks = (seconds) => Math.round(seconds * TIME_BASE);
/** @type {(value: number) => number} */
export const seconds = (value) => value / TIME_BASE;
/** @type {(prefix: string) => string} */
export const identity = (prefix) => `${prefix}-${globalThis.crypto.randomUUID()}`;
/** @type {<T>(value: T) => T} */
export const clone = (value) => JSON.parse(JSON.stringify(value));
export { CommandHistory } from './history.mjs';
export { createProjectDraft, documentPatches, applyDocumentPatches } from './immutable.mjs';

/** @type {(name?: string) => import('./types.js').Project} */
export function createProject(name = '未命名作品') {
  return {
    format: FORMAT,
    version: 1,
    id: identity('project'),
    name,
    canvas: { width: 1920, height: 1080 },
    frameRate: { numerator: 30, denominator: 1 },
    audio: { sampleRate: 48000, channels: 2 },
    assets: [],
    timeline: { id: identity('timeline'), tracks: [], groups: [], links: [], transitions: [] }
  };
}

/** @type {(project: import('./types.js').Project) => number} */
export function duration(project) {
  return Math.max(
    0,
    ...project.timeline.tracks.flatMap((t) => t.items.map((i) => i.placement.end))
  );
}

/** @type {(project: import('./types.js').Project,asset: import('./types.js').Asset,options?: { validate?: boolean; start?: number; trackId?: string }) => import('./types.js').Item} */
export function addAsset(project, asset, { start, trackId, validate = true } = {}) {
  if (!project.assets.some((a) => a.id === asset.id)) project.assets.push(clone(asset));
  const type = asset.kind === 'audio' ? 'audio' : 'video';
  let track = project.timeline.tracks.find((t) => t.id === trackId);
  if (track && (track.type !== type || track.locked)) throw new Error('目标轨道类型不匹配或已锁定');
  if (!track) {
    track = {
      id: identity('track'),
      name: type === 'audio' ? '音频' : '视频',
      type,
      visible: true,
      muted: false,
      locked: false,
      items: []
    };
    project.timeline.tracks.push(track);
  }
  const begin = start ?? Math.max(0, ...track.items.map((i) => i.placement.end));
  const length = asset.kind === 'image' ? ticks(5) : asset.duration;
  const item = {
    id: identity('item'),
    name: asset.name,
    enabled: true,
    placement: { begin, end: begin + length },
    clip: {
      id: identity('clip'),
      type: asset.kind,
      assetId: asset.id,
      source: { begin: 0, end: length },
      visual: { positionX: 0, positionY: 0, scaleX: 1, scaleY: 1, rotationDegrees: 0, opacity: 1 },
      audio: { gainLinear: 1, muted: false }
    }
  };
  enrichClip(item.clip);
  track.items.push(item);
  if (validate) validateProject(project);
  return item;
}

/** Text is authored content, never a fake media file or browser-only overlay. */
/** @type {(project: import('./types.js').Project,options?: { validate?: boolean;
    content?: string;
    start?: number;
    length?: number;
    fontSize?: number;
    color?: string;
    trackId?: string;
    template?: import('./types.js').TextContent['template'];
  }) => import('./types.js').Item} */
export function addText(
  project,
  {
    content = '默认文字',
    start = 0,
    length = ticks(5),
    fontSize = 64,
    color = '#ffffff',
    trackId,
    template,
    validate = true
  } = {}
) {
  const item = {
    id: identity('item'),
    name: content.slice(0, 80) || '文字',
    enabled: true,
    placement: { begin: start, end: start + length },
    clip: {
      id: identity('clip'),
      type: 'text',
      assetId: '',
      source: { begin: 0, end: length },
      text: {
        content,
        fontSize,
        color,
        fontFamily: 'system',
        ...(template ? { template: clone(template) } : {})
      },
      visual: { positionX: 0, positionY: 0, scaleX: 1, scaleY: 1, rotationDegrees: 0, opacity: 1 },
      audio: { gainLinear: 1, muted: false }
    }
  };
  enrichClip(item.clip);
  const target = trackId ? project.timeline.tracks.find((track) => track.id === trackId) : null;
  if (trackId && (!target || target.type !== 'text' || target.locked))
    throw new Error('目标字幕轨不存在或已锁定');
  if (target) target.items.push(item);
  else
    project.timeline.tracks.unshift({
      id: identity('track'),
      name: '文字',
      type: 'text',
      visible: true,
      muted: false,
      locked: false,
      items: [item]
    });
  if (validate) validateProject(project);
  return item;
}

/** HTML is authored visual content on a normal video track, with its own source clock. */
/** @type {(project: import('./types.js').Project,options: {
    html: import('./types.js').HtmlContent;
    name?: string;
    start?: number;
    length?: number;
    trackId?: string;
    validate?: boolean;
  }) => import('./types.js').Item} */
export function addHtmlClip(
  project,
  { html, name = 'HTML 动画', start = 0, length, trackId, validate = true }
) {
  validateHtmlContent(html);
  length ??= html.duration;
  if (
    !Number.isSafeInteger(start) ||
    start < 0 ||
    !Number.isSafeInteger(length) ||
    length < 1 ||
    length > html.duration ||
    start + length > ticks(86400)
  )
    throw new Error('HTML 动画片段时间范围无效或超出动画时长');
  let track = project.timeline.tracks.find((t) => t.id === trackId);
  if (trackId !== undefined && (!track || track.type !== 'video' || track.locked))
    throw new Error('HTML 动画目标必须是存在且未锁定的视频轨道');
  const item = {
    id: identity('item'),
    name,
    enabled: true,
    placement: { begin: start, end: start + length },
    clip: {
      id: identity('clip'),
      type: 'html-clip',
      assetId: '',
      html: clone(html),
      source: { begin: 0, end: length },
      visual: { positionX: 0, positionY: 0, scaleX: 1, scaleY: 1, rotationDegrees: 0, opacity: 1 },
      audio: { gainLinear: 1, muted: false }
    }
  };
  enrichClip(item.clip);
  if (!track) {
    track = {
      id: identity('track'),
      name: 'HTML 动画',
      type: 'video',
      visible: true,
      muted: false,
      locked: false,
      items: []
    };
    project.timeline.tracks.unshift(track);
  }
  track.items.push(item);
  track.items.sort((a, b) => a.placement.begin - b.placement.begin);
  if (validate) validateProject(project);
  return item;
}

/** @type {(project: import('./types.js').Project,id: string) => { track: import('./types.js').Track; item: import('./types.js').Item }} */
export function findItem(project, id) {
  for (const track of project.timeline.tracks) {
    const item = track.items.find((i) => i.id === id);
    if (item) return { track, item };
  }
  throw new Error('片段不存在');
}

/** @type {(project: import('./types.js').Project,id: string,at: number) => import('./types.js').Item} */
export function splitItem(project, id, at) {
  const { track, item } = findItem(project, id);
  if (track.locked) throw new Error('轨道已锁定');
  if (at <= item.placement.begin || at >= item.placement.end)
    throw new Error('请将播放头移动到片段内部');
  const right = clone(item);
  right.id = identity('item');
  right.clip.id = identity('clip');
  right.placement.begin = at;
  right.clip.source.begin = mapTimelineToSource(item, at);
  const offset = at - item.placement.begin;
  const fullLength = item.placement.end - item.placement.begin;
  right.clip.automation = sliceAutomation(item, offset, fullLength, { freshIds: true });
  item.clip.automation = sliceAutomation(item, 0, offset);
  renewEffectIdentities(right.clip);
  for (const key of ['fadeIn', 'fadeOut']) {
    if (item.clip.audio[key]) item.clip.audio[key] = Math.min(item.clip.audio[key], offset);
    if (right.clip.audio[key])
      right.clip.audio[key] = Math.min(right.clip.audio[key], fullLength - offset);
  }
  for (const t of project.timeline.transitions ?? [])
    if (t.fromItemId === id) t.fromItemId = right.id;
  for (const field of ['groups', 'links'])
    for (const entry of project.timeline[field] ?? [])
      if (entry.itemIds.includes(id)) entry.itemIds.push(right.id);
  item.clip.source.end = right.clip.source.begin;
  item.placement.end = at;
  track.items.push(right);
  track.items.sort((a, b) => a.placement.begin - b.placement.begin);
  return right;
}

/** @type {(project: import('./types.js').Project,id: string) => void} */
export function removeItem(project, id) {
  const { track } = findItem(project, id);
  if (track.locked) throw new Error('轨道已锁定');
  track.items = track.items.filter((i) => i.id !== id);
  project.timeline.tracks = project.timeline.tracks.filter((t) => t.items.length);
  for (const field of ['groups', 'links'])
    if (project.timeline[field])
      project.timeline[field] = project.timeline[field]
        .map((entry) => ({ ...entry, itemIds: entry.itemIds.filter((i) => i !== id) }))
        .filter((entry) => entry.itemIds.length > 1);
  if (project.timeline.transitions)
    project.timeline.transitions = project.timeline.transitions.filter(
      (t) => t.fromItemId !== id && t.toItemId !== id
    );
}

/** @type {(project: import('./types.js').Project,id: string,begin: number,trackId?: string) => void} */
export function moveItem(project, id, begin, targetTrackId) {
  const { track, item } = findItem(project, id);
  const target = targetTrackId
    ? project.timeline.tracks.find((t) => t.id === targetTrackId)
    : track;
  if (!target || track.locked || target.locked || target.type !== track.type)
    throw new Error('无法移动到该轨道');
  const length = item.placement.end - item.placement.begin;
  item.placement = { begin, end: begin + length };
  if (target !== track) {
    track.items = track.items.filter((i) => i.id !== id);
    target.items.push(item);
  }
  validateProject(project);
}

/** @type {(project: import('./types.js').Project,id: string,begin: number,end: number) => void} */
export function trimItem(project, id, begin, end) {
  const { track, item } = findItem(project, id);
  if (track.locked) throw new Error('轨道已锁定');
  const original = clone(item);
  item.clip.source = trimSourceRange(original, begin, end);
  item.clip.automation = sliceAutomation(
    original,
    begin - original.placement.begin,
    end - original.placement.begin
  );
  for (const key of ['fadeIn', 'fadeOut'])
    if (item.clip.audio[key]) item.clip.audio[key] = Math.min(item.clip.audio[key], end - begin);
  item.placement = { begin, end };
  validateProject(project);
}

/** Validate at the boundary before replacing a session or invoking native code. */
/** @type {(project: unknown) => import('./types.js').Project} */
export function validateProject(p) {
  const fail = (message) => {
    throw new Error(message);
  };
  const number = (n, min, max, label) => {
    if (typeof n !== 'number' || !Number.isFinite(n) || n < min || n > max) fail(`无效的${label}`);
  };
  const integer = (n, min, max, label) => {
    number(n, min, max, label);
    if (!Number.isSafeInteger(n)) fail(`${label}必须是整数`);
  };
  const seen = new Set();
  const id = (v) => {
    if (typeof v !== 'string' || !/^[a-zA-Z0-9][a-zA-Z0-9_.:-]{0,159}$/.test(v) || seen.has(v))
      fail('标识无效或重复');
    seen.add(v);
  };
  const name = (v) => {
    if (typeof v !== 'string' || !v.trim() || v.length > 512) fail('名称无效');
  };
  if (p?.format !== FORMAT || p.version !== 1)
    fail('不支持的作品格式；请打开 VideoCut 编辑会话文件');
  id(p.id);
  name(p.name);
  id(p.timeline?.id);
  integer(p.canvas?.width, 2, 7680, '画布宽度');
  integer(p.canvas?.height, 2, 7680, '画布高度');
  if (p.canvas.width % 2 || p.canvas.height % 2) fail('画布尺寸必须为偶数');
  integer(p.frameRate?.numerator, 1, 120000, '帧率');
  integer(p.frameRate?.denominator, 1, 10000, '帧率分母');
  number(p.frameRate.numerator / p.frameRate.denominator, 1, 120, '帧率');
  if (p.audio?.sampleRate !== 48000 || p.audio.channels !== 2) fail('音频设置必须为 48 kHz 双声道');
  if (
    !Array.isArray(p.assets) ||
    p.assets.length > 2000 ||
    !Array.isArray(p.timeline.tracks) ||
    p.timeline.tracks.length > 100
  )
    fail('作品过大或轨道缺失');
  const assets = new Map();
  for (const a of p.assets) {
    id(a.id);
    name(a.name);
    if (!['video', 'audio', 'image'].includes(a.kind)) fail('暂不支持此素材类型');
    if (typeof a.path !== 'string' || !a.path || a.path.includes('\0')) fail('素材路径无效');
    integer(a.duration, 1, ticks(86400), '素材时长');
    integer(a.size, 0, Number.MAX_SAFE_INTEGER, '素材大小');
    integer(a.width, 0, 32768, '素材宽度');
    integer(a.height, 0, 32768, '素材高度');
    if (typeof a.hasAudio !== 'boolean') fail('音频流信息缺失');
    assets.set(a.id, a);
  }
  let count = 0;
  for (const t of p.timeline.tracks) {
    id(t.id);
    name(t.name);
    if (!['video', 'audio', 'text'].includes(t.type) || !Array.isArray(t.items))
      fail('轨道类型无效');
    for (const key of ['visible', 'muted', 'locked'])
      if (typeof t[key] !== 'boolean') fail('轨道状态无效');
    let previousEnd = 0;
    for (const i of [...t.items].sort((a, b) => a.placement.begin - b.placement.begin)) {
      if (++count > 10000) fail('片段数量过多');
      id(i.id);
      id(i.clip?.id);
      name(i.name);
      if (typeof i.enabled !== 'boolean') fail('片段状态无效');
      const a = assets.get(i.clip.assetId);
      if (i.clip.type === 'text') {
        if (t.type !== 'text' || i.clip.assetId !== '') fail('文字轨道无效');
        const text = i.clip.text;
        if (
          !text ||
          typeof text.content !== 'string' ||
          !text.content.trim() ||
          text.content.length > 10000 ||
          text.content.includes('\0')
        )
          fail('文字内容无效');
        if (
          text.template !== undefined &&
          (!text.template ||
            text.template.version !== 1 ||
            (text.template.recipe
              ? typeof text.template.id !== 'string' ||
                !/^[a-zA-Z0-9._-]{1,120}$/.test(text.template.id)
              : !TEXT_TEMPLATES.some((t) => t.id === text.template.id)))
        )
          fail('文字模板或版本无效');
        if (text.template?.recipe) {
          const recipe = text.template.recipe;
          if (
            !TEMPLATE_PARTS.base.includes(recipe.base) ||
            (recipe.backdrop !== undefined && !TEMPLATE_PARTS.backdrop.includes(recipe.backdrop)) ||
            (recipe.animation !== undefined && !TEMPLATE_PARTS.animation.includes(recipe.animation))
          )
            fail('文字模板配方包含未提供的原生部件');
          assertRecipeCombination(recipe);
        }
        if (
          text.template?.resourceBase !== undefined &&
          !/^\/project-resources\/[a-f0-9]+\/text-templates\/$/.test(text.template.resourceBase)
        )
          fail('文字模板资源地址无效');
        if (text.template && [...text.content].length > 100) fail('模板文字最多 100 个字符');
        number(text.fontSize, 4, 1000, '字号');
        if (
          !/^#[0-9a-f]{6}$/i.test(text.color) ||
          text.fontFamily !== (text.font?.family ?? 'system')
        )
          fail('文字样式无效');
      } else if (i.clip.type === 'html-clip') {
        if (t.type !== 'video' || i.clip.assetId !== '') fail('HTML 动画轨道无效');
        validateHtmlContent(i.clip.html);
      } else if (
        !a ||
        a.kind !== i.clip.type ||
        t.type === 'text' ||
        (t.type === 'audio') !== (a.kind === 'audio')
      )
        fail('片段素材引用无效');
      const { begin, end } = i.placement;
      integer(begin, 0, ticks(86400), '片段开始');
      integer(end, begin + 1, ticks(86400), '片段结束');
      if (begin < previousEnd) fail('同一轨道内的片段不能重叠');
      previousEnd = end;
      integer(i.clip.source.begin, 0, ticks(86400), '素材入点');
      integer(i.clip.source.end, i.clip.source.begin + 1, ticks(86400), '素材出点');
      const rate = (i.clip.retime?.constantRatePpm ?? 1000000) / 1000000;
      if (
        Math.abs(i.clip.source.end - i.clip.source.begin - (end - begin) * rate) >
        (rate === 1 ? 0 : Math.max(1, rate))
      )
        fail('素材时长与片段变速时长不一致');
      if (a && a.kind !== 'image' && i.clip.source.end > a.duration) fail('裁剪超出素材时长');
      if (i.clip.type === 'html-clip' && i.clip.source.end > i.clip.html.duration)
        fail('裁剪超出 HTML 动画时长');
      const v = i.clip.visual;
      number(v?.positionX, -32768, 32768, '水平位置');
      number(v?.positionY, -32768, 32768, '垂直位置');
      number(v?.scaleX, 0.01, 20, '水平缩放');
      number(v?.scaleY, 0.01, 20, '垂直缩放');
      number(v?.rotationDegrees, -360, 360, '旋转');
      number(v?.opacity, 0, 1, '不透明度');
      number(i.clip.audio?.gainLinear, 0, 4, '音量');
      if (typeof i.clip.audio?.muted !== 'boolean') fail('片段静音状态无效');
    }
  }
  validateExtensions(p, seen);
  return p;
}

export * from './model.mjs';
export { editTimeline } from './operations.mjs';
export { EditTransaction } from './transaction.mjs';
