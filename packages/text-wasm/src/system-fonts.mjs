/** Resolve the same installed faces for the browser renderer and native project writer. */
export function chooseTemplateFonts(catalog) {
  const fonts = new Map(catalog.fonts.map((font) => [font.id, font]));
  const sans = fonts.get(catalog.defaults.sans);
  const cjk = fonts.get(catalog.defaults.cjk);
  if (!sans || !cjk) throw new Error('没有可用于文字模板的系统字体');
  return { sans, cjk };
}

const plainFontFields = [
  'family',
  'postscriptName',
  'weight',
  'width',
  'slant',
  'sourceFaceIndex',
  'platform',
  'identity'
];

/** Minimal editor projection of the persisted native System FontReference. */
export function plainTextFontProjection(font) {
  return Object.fromEntries(plainFontFields.map((key) => [key, font[key]]));
}

/** Existing authored identities must resolve exactly; only new text selects a default. */
export function choosePlainTextFont(catalog, text) {
  if (text.font) {
    const selected = catalog.fonts.find((font) =>
      plainFontFields.every((key) => font[key] === text.font[key])
    );
    if (!selected || text.fontFamily !== selected.family)
      throw new Error(`作品系统字体不可用或已变更：${text.font.family}`);
    return selected;
  }
  if (text.fontFamily && text.fontFamily !== 'system')
    throw new Error('基础文字缺少完整的系统字体身份');
  const fonts = new Map(catalog.fonts.map((font) => [font.id, font]));
  const cjk = fonts.get(catalog.defaults.cjk);
  if (cjk) return cjk;
  if (/[\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}\p{Script=Hangul}]/u.test(text.content))
    throw new Error('当前系统未安装可用的中日韩字体，请先安装系统字体');
  const sans = fonts.get(catalog.defaults.sans);
  if (!sans) throw new Error('没有可用于基础文字的系统字体');
  return sans;
}

const cjkFont = (reference) =>
  /reference-cjk|source-han|source han|pingfang|noto.*cjk/i.test(
    `${reference?.asset_id ?? ''} ${reference?.family ?? ''}`
  );

/**
 * Font bytes remain in the local font service. WASM addresses an extracted sfnt
 * face; Format1 addresses the original installed family and collection face.
 * This is deliberately shared so font selection cannot diverge between them.
 */
export function rewriteTemplateFonts(value, catalog, { native = false } = {}) {
  const fonts = chooseTemplateFonts(catalog);
  const isCjk = (item) =>
    cjkFont(item) ||
    item?.asset_id === fonts.cjk.id ||
    (fonts.cjk.id !== fonts.sans.id &&
      item?.family === fonts.cjk.family &&
      (!item.postscript_name || item.postscript_name === fonts.cjk.postscriptName));
  const reference = (font) => ({
    kind: native ? 'system' : 'builtin',
    family: font.family,
    postscript_name: font.postscriptName ?? '',
    weight: font.weight ?? 400,
    width: font.width ?? 5,
    slant: font.slant ?? 'upright',
    face_index: native ? (font.sourceFaceIndex ?? 0) : 0,
    variation_axes: [],
    asset_id: native ? '' : font.id,
    platform: native ? font.platform : '',
    digest: '',
    face_fingerprint: font.identity ?? '',
    allow_system_glyph_fallback: false
  });
  function visit(item, key) {
    if (item instanceof Map) return item;
    if (Array.isArray(item))
      return item
        .filter((entry) => key !== 'resources' || entry?.kind !== 'font')
        .map((entry) => visit(entry));
    if (!item || typeof item !== 'object') return item;
    if (item.primary && Array.isArray(item.fallbacks)) {
      const selected = isCjk(item.primary) ? fonts.cjk : fonts.sans;
      const primary = reference(selected);
      return {
        ...item,
        primary,
        fallbacks: selected.id === fonts.cjk.id ? [] : [reference(fonts.cjk)],
        family: primary.family,
        postscript_name: primary.postscript_name,
        weight: primary.weight,
        width: primary.width,
        slant: primary.slant,
        face_index: primary.face_index,
        variation_axes: [],
        allow_system_glyph_fallback: false
      };
    }
    if (
      ['builtin', 'project_managed', 'system'].includes(item.kind) &&
      typeof item.family === 'string' &&
      ('asset_id' in item || 'platform' in item)
    )
      return reference(isCjk(item) ? fonts.cjk : fonts.sans);
    return Object.fromEntries(Object.entries(item).map(([k, v]) => [k, visit(v, k)]));
  }
  return visit(value);
}
