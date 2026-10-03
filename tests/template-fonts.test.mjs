import test from 'node:test';
import assert from 'node:assert/strict';
import {
  chooseTemplateFonts,
  rewriteTemplateFonts,
  choosePlainTextFont,
  plainTextFontProjection
} from '../packages/text-wasm/src/system-fonts.mjs';

const catalog = {
  fonts: [
    {
      id: 'system-sans',
      family: 'Local Sans',
      postscriptName: 'LocalSans-Regular',
      weight: 400,
      width: 5,
      slant: 'upright',
      sourceFaceIndex: 2,
      platform: 'macos',
      identity: 'face-sans'
    },
    {
      id: 'system-cjk',
      family: 'Local CJK',
      postscriptName: 'LocalCJK-Medium',
      weight: 500,
      width: 5,
      slant: 'upright',
      sourceFaceIndex: 3,
      platform: 'macos',
      identity: 'face-cjk'
    }
  ],
  defaults: { sans: 'system-sans', cjk: 'system-cjk' }
};
const original = () => ({
  composition: {
    resources: [
      { kind: 'font', asset_id: 'packaged-font' },
      { kind: 'image', asset_id: 'art' }
    ],
    font: {
      primary: {
        kind: 'builtin',
        asset_id: 'builtin.font.inter.variable.v1',
        family: 'Inter',
        weight: 600,
        face_index: 0,
        variation_axes: [{ tag: 'wght', value: 600 }]
      },
      fallbacks: [
        {
          kind: 'builtin',
          asset_id: 'builtin.font.reference-cjk.source-han-sans-sc-medium.v1',
          family: 'Source Han Sans SC'
        }
      ],
      family: 'Inter',
      weight: 600,
      features: [{ tag: 'kern', value: 1 }]
    }
  },
  assets: new Map([['art.png', { bytes: new Uint8Array([1, 2]), mediaType: 'image/png' }]])
});

test('browser and Format1 use the same installed font faces without persisting font bytes', () => {
  const authored = original();
  const browser = rewriteTemplateFonts(authored, catalog);
  const native = rewriteTemplateFonts(authored, catalog, { native: true });
  assert.equal(browser.assets, authored.assets);
  assert.equal(authored.composition.font.family, 'Inter');
  assert.deepEqual(native.composition.resources, [{ kind: 'image', asset_id: 'art' }]);
  const browserFont = browser.composition.font;
  const nativeFont = native.composition.font;
  for (const property of ['family', 'postscript_name', 'weight', 'width', 'slant'])
    assert.equal(browserFont[property], nativeFont[property]);
  assert.equal(browserFont.primary.asset_id, 'system-sans');
  assert.equal(browserFont.primary.face_index, 0);
  assert.equal(nativeFont.primary.kind, 'system');
  assert.equal(nativeFont.primary.asset_id, '');
  assert.equal(nativeFont.primary.digest, '');
  assert.equal(nativeFont.primary.platform, 'macos');
  assert.equal(nativeFont.primary.face_index, 2);
  assert.equal(nativeFont.primary.face_fingerprint, 'face-sans');
  assert.equal(nativeFont.fallbacks[0].family, 'Local CJK');
  assert.equal(nativeFont.fallbacks[0].face_index, 3);
  assert.deepEqual(nativeFont.variation_axes, []);
  assert.deepEqual(nativeFont.features, authored.composition.font.features);
});

test('font normalization is deterministic and records changed system identity', () => {
  const input = original();
  const first = rewriteTemplateFonts(input, catalog, { native: true });
  assert.deepEqual(first, rewriteTemplateFonts(input, structuredClone(catalog), { native: true }));
  const updated = structuredClone(catalog);
  updated.fonts[0].identity = 'changed-font-version';
  assert.notDeepEqual(first, rewriteTemplateFonts(input, updated, { native: true }));
  assert.throws(() => chooseTemplateFonts({ fonts: [], defaults: catalog.defaults }), /系统字体/);
});

test('a CJK primary and a shared default face do not introduce duplicate fallback references', () => {
  const input = original();
  input.composition.font.primary = input.composition.font.fallbacks[0];
  const native = rewriteTemplateFonts(input, catalog, { native: true });
  assert.equal(native.composition.font.primary.family, 'Local CJK');
  assert.deepEqual(native.composition.font.fallbacks, []);
  assert.deepEqual(rewriteTemplateFonts(native, catalog, { native: true }), native);
  const sameDefault = { ...catalog, defaults: { sans: 'system-cjk', cjk: 'system-cjk' } };
  assert.deepEqual(rewriteTemplateFonts(original(), sameDefault).composition.font.fallbacks, []);
});

test('basic text selects one system face consistently and retains an authored identity', () => {
  const selected = choosePlainTextFont(catalog, { content: '中文 ABC', fontFamily: 'system' });
  assert.equal(selected.id, 'system-cjk');
  const text = {
    content: '中文 ABC',
    fontFamily: selected.family,
    font: plainTextFontProjection(selected)
  };
  assert.equal(choosePlainTextFont(catalog, text), selected);
  assert.throws(
    () => choosePlainTextFont(catalog, { ...text, fontFamily: 'Changed' }),
    /不可用或已变更/
  );
  assert.throws(
    () =>
      choosePlainTextFont(catalog, {
        ...text,
        font: { ...text.font, identity: 'changed-font' }
      }),
    /不可用或已变更/
  );
});

test('without a CJK font Latin text uses system sans and CJK text fails explicitly', () => {
  const latinOnly = { fonts: [catalog.fonts[0]], defaults: { sans: 'system-sans', cjk: null } };
  assert.equal(
    choosePlainTextFont(latinOnly, { content: 'Latin 123', fontFamily: 'system' }).id,
    'system-sans'
  );
  for (const content of ['中文', '日本語', 'かな', '한글'])
    assert.throws(
      () => choosePlainTextFont(latinOnly, { content, fontFamily: 'system' }),
      /未安装可用的中日韩字体/
    );
  assert.throws(
    () => choosePlainTextFont({ fonts: [], defaults: {} }, { content: 'ABC' }),
    /没有可用于基础文字/
  );
});
