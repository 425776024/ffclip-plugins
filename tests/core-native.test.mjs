import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, symlink, readdir, rm } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import {
  createProject,
  addAsset,
  addText,
  editTimeline,
  validateProject,
  ticks,
  evaluateVisual,
  evaluateEffects
} from '../packages/core/project.mjs';
import {
  prepareNativeTemplates,
  resolveNativeTextFonts
} from '../packages/server/native-templates.mjs';
import { systemFonts } from '../packages/server/system-fonts.mjs';
import { recipes } from '../packages/text-wasm/src/recipes.mjs';
const bridge = resolve('.local/bin/videocut-bridge');
let nativeFontCatalog;
function run(args, input) {
  if (args[0] === 'export') input = resolveNativeTextFonts(input, nativeFontCatalog);
  return JSON.parse(
    execFileSync(bridge, args, {
      input: input === undefined ? undefined : JSON.stringify(input),
      encoding: 'utf8',
      maxBuffer: 32 * 1024 * 1024
    })
  );
}
async function workspace(t) {
  if (!existsSync(bridge)) {
    t.skip('Native bridge is not built');
    return null;
  }
  const root = await mkdtemp(join(tmpdir(), 'videocut-native-model-'));
  nativeFontCatalog = await systemFonts.catalog();
  t.after(() => rm(root, { recursive: true, force: true }));
  return root;
}
test('native basic text preserves the selected system font identity and rejects changed fonts', async (t) => {
  const root = await workspace(t);
  if (!root) return;
  const p = createProject();
  addText(p, { content: '中文 Latin 123' });
  const resolved = resolveNativeTextFonts(p, nativeFontCatalog);
  const originalText = resolved.timeline.tracks[0].items[0].clip.text;
  assert.equal(p.timeline.tracks[0].items[0].clip.text.fontFamily, 'system');
  assert.equal(p.timeline.tracks[0].items[0].clip.text.font, undefined);
  assert.equal(originalText.font.identity, nativeFontCatalog.defaults.cjk);
  validateProject(resolved);
  assert.throws(
    () =>
      execFileSync(bridge, ['export', join(root, 'missing-font.vcut')], {
        input: JSON.stringify(p),
        encoding: 'utf8',
        stdio: ['pipe', 'pipe', 'pipe']
      }),
    /Resolve the system font/
  );
  const templateWithBasicFont = structuredClone(resolved);
  templateWithBasicFont.timeline.tracks[0].items[0].clip.text.template = {
    id: 'studio-glow',
    version: 1
  };
  assert.throws(() => validateProject(templateWithBasicFont), /不能指定基础文字字体/);
  const path = join(root, 'system-font.vcut');
  run(['export', path], resolved);
  const reopened = validateProject(run(['import', path]));
  assert.deepEqual(reopened.timeline.tracks[0].items[0].clip.text, originalText);
  assert.deepEqual(resolveNativeTextFonts(reopened, nativeFontCatalog), reopened);
  const changed = structuredClone(reopened);
  changed.timeline.tracks[0].items[0].clip.text.font.postscriptName += '-Changed';
  assert.throws(() => resolveNativeTextFonts(changed, nativeFontCatalog), /不可用或已变更/);
  const missing = { ...nativeFontCatalog, fonts: [] };
  assert.throws(() => resolveNativeTextFonts(reopened, missing), /不可用或已变更/);
  const inconsistent = structuredClone(reopened);
  inconsistent.timeline.tracks[0].items[0].clip.text.fontFamily = 'Unrelated face';
  assert.throws(() => validateProject(inconsistent), /文字样式/);
  const files = await readdir(path, { recursive: true });
  assert.ok(files.every((file) => !/\.(?:ttf|otf|ttc|otc|woff2?)$/i.test(file)));
});
test('native property owners retain supported edits and reject unsupported edits before saving', async (t) => {
  const root = await workspace(t);
  if (!root) return;
  let p = createProject();
  const base = {
    name: 'Owner',
    duration: ticks(4),
    size: 1,
    width: 320,
    height: 180,
    hasAudio: false,
    fingerprint: 'sha256:' + 'e'.repeat(64)
  };
  const audio = addAsset(p, {
    ...base,
    id: 'owner-audio',
    kind: 'audio',
    path: '/tmp/owner.wav',
    width: 0,
    height: 0,
    hasAudio: true,
    nativeProbe: {
      streams: [
        { index: 0, codec_type: 'audio', codec_name: 'pcm_s16le', sample_rate: 48000, channels: 2 }
      ]
    }
  });
  const silent = addAsset(p, {
    ...base,
    id: 'owner-silent',
    kind: 'video',
    path: '/tmp/owner.mp4',
    nativeProbe: {
      streams: [{ index: 0, codec_type: 'video', codec_name: 'h264', width: 320, height: 180 }]
    }
  });
  const image = addAsset(p, {
    ...base,
    id: 'owner-image',
    kind: 'image',
    path: '/tmp/owner.png',
    nativeProbe: {
      streams: [{ index: 0, codec_type: 'video', codec_name: 'png', width: 320, height: 180 }]
    }
  });
  const text = addText(p, { content: '音画属性', length: ticks(4) });
  for (const [item, property] of [
    [audio, 'visual.opacity'],
    [silent, 'audio.gainLinear'],
    [image, 'audio.gainLinear'],
    [text, 'audio.gainLinear']
  ]) {
    assert.throws(
      () => editTimeline(p, [{ action: 'set_property', itemId: item.id, property, value: 0.25 }]),
      /不支持此属性/
    );
    assert.throws(
      () =>
        editTimeline(p, [
          { action: 'set_keyframe', itemId: item.id, property, timeSeconds: 0, value: 1 }
        ]),
      /不支持此关键帧/
    );
    // Snapshot/API validation also rejects bypassing the command entry point.
    const invalid = structuredClone(p),
      target = invalid.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === item.id);
    target.clip.automation[property] = {
      timeDomain: 'itemLocal',
      keyframes: [{ id: 'invalid-owner-key', time: 0, value: 1, interpolation: 'linear' }]
    };
    assert.throws(() => validateProject(invalid), /不支持此关键帧/);
  }
  p = editTimeline(p, [
    { action: 'set_audio', itemId: audio.id, gainLinear: 0.5, fadeIn: ticks(0.5) },
    {
      action: 'set_keyframe',
      itemId: audio.id,
      property: 'audio.gainLinear',
      timeSeconds: 0,
      value: 0.25
    },
    {
      action: 'set_keyframe',
      itemId: audio.id,
      property: 'audio.gainLinear',
      timeSeconds: 2,
      value: 0.75
    },
    ...[silent, image, text].map((item) => ({
      action: 'set_transform',
      itemId: item.id,
      opacity: 0.5
    })),
    {
      action: 'set_keyframe',
      itemId: image.id,
      property: 'visual.opacity',
      timeSeconds: 0,
      value: 0.25
    }
  ]).project;
  const path = join(root, 'owners.vcut');
  run(['export', path], p);
  const imported = validateProject(run(['import', path]));
  for (const original of p.timeline.tracks.flatMap((t) => t.items)) {
    const loaded = imported.timeline.tracks
      .flatMap((t) => t.items)
      .find((i) => i.id === original.id);
    assert.deepEqual(loaded.clip.automation, original.clip.automation);
    assert.deepEqual(loaded.clip.audio, original.clip.audio);
    assert.deepEqual(loaded.clip.visual, original.clip.visual);
  }
});
test('native LUT parameter automation preserves item-local keyframes through Format1 and splitting', async (t) => {
  const root = await workspace(t);
  if (!root) return;
  let p = createProject();
  const item = addAsset(p, {
    id: 'asset-animated-lut',
    name: 'Video',
    path: resolve('.local/qa/测试 sample.mp4'),
    kind: 'video',
    duration: ticks(20),
    size: 1,
    width: 320,
    height: 180,
    hasAudio: false
  });
  p = editTimeline(p, [{ action: 'add_effect', itemId: item.id, templateId: 'lut' }]).project;
  const fx = p.timeline.tracks[0].items[0].clip.effects[0],
    property = `effects.${fx.id}.amount`;
  p = editTimeline(p, [
    { action: 'set_speed', itemId: item.id, constantRatePpm: 2000000 },
    { action: 'move_clip', itemId: item.id, startSeconds: 2 },
    {
      action: 'set_keyframe',
      itemId: item.id,
      property,
      timeSeconds: 0,
      value: 0,
      interpolation: 'easeIn'
    },
    { action: 'set_keyframe', itemId: item.id, property, timeSeconds: 10, value: 1 },
    { action: 'split_clip', itemId: item.id, atSeconds: 6 }
  ]).project;
  const text = addText(p, { content: '关键帧文字', start: ticks(2), length: ticks(10) });
  p = editTimeline(p, [{ action: 'add_effect', itemId: text.id, templateId: 'lut' }]).project;
  const textFx = p.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === text.id).clip
    .effects[0];
  p = editTimeline(p, [
    {
      action: 'set_keyframe',
      itemId: text.id,
      property: `effects.${textFx.id}.amount`,
      timeSeconds: 0,
      value: 0
    },
    {
      action: 'set_keyframe',
      itemId: text.id,
      property: `effects.${textFx.id}.amount`,
      timeSeconds: 10,
      value: 1
    },
    { action: 'split_clip', itemId: text.id, atSeconds: 6 }
  ]).project;
  p.assets[0].fingerprint = 'sha256:' + 'c'.repeat(64);
  p.assets[0].nativeProbe = {
    streams: [{ index: 0, codec_type: 'video', codec_name: 'h264', width: 320, height: 180 }]
  };
  const path = join(root, 'animated-lut.vcut');
  run(['export', path], p);
  const actual = validateProject(run(['import', path]));
  for (const original of p.timeline.tracks.flatMap((t) => t.items)) {
    const loaded = actual.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === original.id);
    assert.deepEqual(loaded.clip.automation, original.clip.automation);
    for (const offset of [0, 0.5, 2])
      assert.deepEqual(
        evaluateEffects(loaded, loaded.placement.begin + ticks(offset)),
        evaluateEffects(original, original.placement.begin + ticks(offset))
      );
  }
});
test('native Format1 roundtrip retains retime, crop, fit, flip, blend, curves, groups and linked audio', async (t) => {
  const root = await workspace(t);
  if (!root) return;
  let p = createProject();
  const a = addAsset(p, {
    id: 'asset-video',
    name: 'Video',
    path: resolve('.local/qa/测试 sample.mp4'),
    kind: 'video',
    duration: ticks(20),
    size: 1,
    width: 320,
    height: 180,
    hasAudio: true
  });
  const b = addText(p, {
    content: '中文测试',
    start: ticks(2),
    length: ticks(5),
    color: '#4faa19'
  });
  p = editTimeline(p, [
    { action: 'set_speed', itemId: a.id, constantRatePpm: 2000000 },
    {
      action: 'set_transform',
      itemId: a.id,
      crop: { left: 0.1, top: 0.05, right: 0.2, bottom: 0 },
      fitPolicy: 'cover',
      flipHorizontal: true,
      blendMode: 'screen',
      anchorX: 0.2
    },
    { action: 'set_audio', itemId: a.id, gainLinear: 0.5, fadeIn: ticks(1) },
    {
      action: 'set_keyframe',
      itemId: a.id,
      property: 'visual.opacity',
      timeSeconds: 0,
      value: 0.2,
      interpolation: 'easeIn'
    },
    {
      action: 'set_keyframe',
      itemId: a.id,
      property: 'visual.opacity',
      timeSeconds: 8,
      value: 0.9
    },
    {
      action: 'set_keyframe',
      itemId: a.id,
      property: 'audio.gainLinear',
      timeSeconds: 0,
      value: 0.1
    },
    {
      action: 'set_keyframe',
      itemId: a.id,
      property: 'audio.gainLinear',
      timeSeconds: 8,
      value: 0.7
    },
    { action: 'group_clips', itemIds: [a.id, b.id] }
  ]).project;
  p.assets[0].fingerprint = 'sha256:' + 'a'.repeat(64);
  p.assets[0].nativeProbe = {
    streams: [
      { index: 0, codec_type: 'video', codec_name: 'h264', width: 320, height: 180 },
      { index: 1, codec_type: 'audio', codec_name: 'aac', sample_rate: 48000, channels: 2 }
    ]
  };
  p.assets.push({ ...p.assets[0], id: 'asset-unused-library', name: 'Unused library clip' });
  p.timeline.tracks.find((track) => track.items.some((item) => item.id === a.id)).syncLocked = true;
  const path = join(root, 'roundtrip.vcut');
  const receipt = run(['export', path], p);
  assert.equal(receipt.verified, true);
  const nativeVideoTrack = receipt.tracks.find((track) => track.items.some((item) => item.id === a.id));
  const nativeAudioTrack = receipt.tracks.find((track) => track.id === `audio-${nativeVideoTrack.id}`);
  assert.equal(nativeVideoTrack.syncLocked, true);
  assert.equal(nativeAudioTrack.syncLocked, true);
  const reopened = validateProject(run(['import', path]));
  assert.deepEqual(reopened.assets.map((a) => a.id).sort(), p.assets.map((a) => a.id).sort());
  assert.equal(reopened.assets.find((a) => a.id === 'asset-unused-library').hasAudio, true);
  const video = reopened.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === a.id);
  const original = p.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === a.id);
  assert.equal(video.clip.retime.constantRatePpm, 2000000);
  assert.deepEqual(video.clip.visual, original.clip.visual);
  assert.deepEqual(video.clip.audio, original.clip.audio);
  assert.deepEqual(video.clip.automation, original.clip.automation);
  assert.deepEqual(reopened.timeline.groups, p.timeline.groups);
  assert.equal(evaluateVisual(video, ticks(4)).opacity, evaluateVisual(original, ticks(4)).opacity);
  assert.equal(
    reopened.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === b.id).clip.text.color,
    '#4faa19'
  );
  assert.ok(!(await readdir(path)).includes('web-session.json'));
});
test('native independent companion synchronization is rejected instead of silently merged', async (t) => {
  const root = await workspace(t);
  if (!root) return;
  const path = resolve('tests/fixtures/native-companion-sync-mismatch.vcut');
  const native = run(['inspect', path]);
  assert.deepEqual(native.tracks.map((track) => track.syncLocked), [true, false]);
  assert.throws(() => execFileSync(bridge, ['import', path], { encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] }), /independent authored state/);
});
test('native complex recipe roundtrip verifies authored fingerprint and bundle digest without JSON sidecar', async (t) => {
  const root = await workspace(t);
  if (!root) return;
  const staticDir = join(root, 'static'),
    temp = join(root, 'resources');
  await mkdir(staticDir);
  await mkdir(temp);
  await symlink(resolve('dist/web/text-templates'), join(staticDir, 'text-templates'));
  const p = createProject();
  for (const recipe of recipes)
    addText(p, { content: '流光', template: { id: recipe.id, version: 1 } });
  const custom = { id: 'release-custom-flower', version: 1, style: { color: '#ff5c70', fontSize: 180 },
    recipe: { base: 'flower-style-38', backdrop: 'bubble-nine-slice', animation: 'anim-lua-letter-transform' } };
  const widthItem = addText(p, { content: '发布验收', template: custom });
  widthItem.clip.text.layoutWidth = 480;
  const { bundles, assets } = await prepareNativeTemplates(p, staticDir, temp);
  const catalog = await systemFonts.catalog();
  const sans = catalog.fonts.find((f) => f.id === catalog.defaults.sans);
  const cjk = catalog.fonts.find((f) => f.id === catalog.defaults.cjk);
  const references = [];
  function inspectFonts(value) {
    if (!value || typeof value !== 'object') return;
    if (value.primary && Array.isArray(value.fallbacks)) references.push(value);
    for (const child of Object.values(value)) inspectFonts(child);
  }
  inspectFonts(bundles);
  assert.ok(references.length > 0);
  for (const font of references) {
    assert.equal(font.primary.kind, 'system');
    assert.equal(font.primary.asset_id, '');
    assert.equal(font.primary.digest, '');
    const face = [sans, cjk].find((f) => f.family === font.primary.family);
    assert.ok(face);
    assert.equal(font.primary.face_index, face.sourceFaceIndex);
    assert.equal(font.family, face.family);
    for (const fallback of font.fallbacks) {
      assert.equal(fallback.kind, 'system');
      assert.equal(fallback.asset_id, '');
      assert.equal(fallback.digest, '');
      assert.equal(fallback.family, cjk.family);
      assert.equal(fallback.face_index, cjk.sourceFaceIndex);
    }
  }
  assert.ok(assets.every((asset) => asset.kind !== 'font'));
  assert.ok(!JSON.stringify(bundles).includes('builtin.font.'));
  const secondTemp = join(root, 'resources-again');
  await mkdir(secondTemp);
  const second = await prepareNativeTemplates(p, staticDir, secondTemp);
  assert.deepEqual(second.bundles, bundles);
  const path = join(root, 'template.vcut');
  const receipt = run(['export', path], { ...p, templateBundles: bundles, templateAssets: assets });
  assert.equal(receipt.verified, true);
  const reopened = validateProject(run(['import', path]));
  const outputs = reopened.timeline.tracks.flatMap((track) =>
    track.items.map((item) => item.clip.text)
  );
  assert.deepEqual(
    outputs.map((text) => text.template.id).sort(),
    [...recipes.map((recipe) => recipe.id), custom.id].sort()
  );
  const digests = run(['template-digests'], bundles);
  assert.deepEqual(digests, run(['template-digests'], second.bundles));
  for (const output of outputs) {
    assert.equal(output.content, output.template.id === custom.id ? '发布验收' : '流光');
    assert.equal(output.template.packageDigest, digests[output.template.id]);
    if (output.template.id === custom.id) assert.deepEqual(output.template.recipe, custom.recipe);
    if (output.template.id === custom.id) assert.deepEqual(output.template.style, custom.style);
    if (output.template.id === custom.id) assert.equal(output.layoutWidth, 480);
  }
  const files = await readdir(path, { recursive: true });
  assert.ok(files.every((file) => !/\.(?:ttf|otf|ttc|otc|woff2?)$/i.test(file)));
  assert.ok(!(await readdir(path)).includes('web-session.json'));
});
test('native basic text width survives canonical export/import without changing font size', async (t) => {
  const root = await workspace(t); if (!root) return;
  const p = createProject();
  const item = addText(p, { content: '文字宽度换行验收', fontSize:48 });
  item.clip.text.layoutWidth = 240;
  const path = join(root, 'text-width.vcut');
  run(['export', path], p);
  const actual = validateProject(run(['import', path])).timeline.tracks[0].items[0];
  assert.equal(actual.clip.text.layoutWidth, 240);
  assert.equal(actual.clip.text.fontSize,48);
  assert.deepEqual(actual.clip.visual,item.clip.visual);
});
test('native Format1 retains canonical blur, glow, LUT and each supported transition', async (t) => {
  const root = await workspace(t);
  if (!root) return;
  for (const variant of ['dissolve', 'fade', 'fade-white', 'wipe', 'slide']) {
    const templateId = variant === 'fade-white' ? 'fade' : variant;
    let p = createProject();
    const item = addAsset(p, {
      id: 'asset-transition',
      name: 'Transition',
      path: resolve('.local/qa/测试 sample.mp4'),
      kind: 'video',
      duration: ticks(20),
      size: 1,
      width: 320,
      height: 180,
      hasAudio: false
    });
    const split = editTimeline(p, [{ action: 'split_clip', itemId: item.id, atSeconds: 10 }]);
    p = split.project;
    const toItemId = split.operations[0].rightItemId;
    p = editTimeline(p, [
      { action: 'add_effect', itemId: item.id, templateId: 'blur', parameters: { radius: 9 } },
      {
        action: 'add_effect',
        itemId: item.id,
        templateId: 'glow',
        parameters: { radius: 12, strength: 0.5 }
      },
      {
        action: 'add_effect',
        itemId: item.id,
        templateId: 'lut',
        parameters: { preset: 'cool', amount: 0.7 }
      },
      {
        action: 'add_transition',
        fromItemId: item.id,
        toItemId,
        templateId,
        durationSeconds: 2,
        parameters: ['wipe', 'slide'].includes(templateId)
          ? { direction: 'right' }
          : variant === 'fade-white'
            ? { color: '#ffffff' }
            : {}
      }
    ]).project;
    p.assets[0].fingerprint = 'sha256:' + 'b'.repeat(64);
    p.assets[0].nativeProbe = {
      streams: [{ index: 0, codec_type: 'video', codec_name: 'h264', width: 320, height: 180 }]
    };
    const path = join(root, variant + '.vcut');
    run(['export', path], p);
    const actual = validateProject(run(['import', path]));
    assert.deepEqual(actual.timeline.transitions, p.timeline.transitions);
    assert.deepEqual(
      actual.timeline.tracks[0].items[0].clip.effects,
      p.timeline.tracks[0].items[0].clip.effects
    );
  }
});
