import { readFile, writeFile } from 'node:fs/promises';
import { join, extname } from 'node:path';
import { createHash } from 'node:crypto';
import { resolveRecipe, composeRecipe } from '../text-wasm/src/recipes.mjs';
import {
  rewriteTemplateFonts,
  choosePlainTextFont,
  plainTextFontProjection
} from '../text-wasm/src/system-fonts.mjs';
import { systemFonts } from './system-fonts.mjs';
const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');
const isFontResource = (path, asset) =>
  /^(font\/|application\/(?:x-font|font-|vnd\.ms-fontobject))/.test(asset.mediaType) ||
  /\.(?:ttf|otf|ttc|otc|woff2?)$/i.test(path);

/** Resolve the nonpersistent default selection before writing canonical font references. */
export function resolveNativeTextFonts(project, catalog) {
  const resolved = structuredClone(project);
  for (const track of resolved.timeline.tracks)
    for (const { clip } of track.items)
      if (clip.text && !clip.text.template) {
        const font = choosePlainTextFont(catalog, clip.text);
        clip.text.font = plainTextFontProjection(font);
        clip.text.fontFamily = font.family;
      }
  return resolved;
}

/** Copy media resources; installed fonts remain native system locators, never project assets. */
export async function prepareNativeTemplates(project, staticDir, temporary, options = {}) {
  const ids = [
    ...new Set(
      project.timeline.tracks
        .flatMap((t) => t.items)
        .map((i) => i.clip.text?.template?.id)
        .filter(Boolean)
    )
  ];
  const assets = new Map(),
    bundles = {};
  if (!project.timeline.tracks.some((track) => track.items.some((item) => item.clip.text)))
    return { bundles, assets: [], project: structuredClone(project) };
  const catalog = options.fontCatalog ?? (await systemFonts.catalog());
  const resolvedProject = resolveNativeTextFonts(project, catalog);
  if (!ids.length) return { bundles, assets: [], project: resolvedProject };
  const register = async (bytes, mediaType, kind, suffix) => {
    const digest = hash(bytes),
      id = `text-asset-${digest}`;
    if (!assets.has(id)) {
      const fileName = `${digest}${suffix}`,
        path = join(temporary, fileName);
      await writeFile(path, bytes, { flag: 'wx' });
      assets.set(id, {
        id,
        digest: `sha256:${digest}`,
        mediaType,
        kind,
        byteLength: bytes.length,
        path,
        relativePath: `assets/text/${fileName}`
      });
    }
    return assets.get(id);
  };
  const load = async (id) => {
    const dir = join(staticDir, 'text-templates/templates', `com.videocut.text.qt-type.${id}`);
    const manifest = JSON.parse(await readFile(join(dir, 'manifest.json'), 'utf8'));
    const files = new Map();
    for (const file of manifest.files) {
      if (
        !/^[A-Za-z0-9_./-]+$/.test(file.path) ||
        file.path.startsWith('/') ||
        file.path.split('/').some((p) => !p || p === '.' || p === '..')
      )
        throw new Error('无效的模板资源路径');
      files.set(file.path, {
        bytes: await readFile(join(dir, file.path)),
        mediaType: file.media_type
      });
    }
    const entry = (name) => {
      const path = manifest.entries[name].path;
      const value = files.get(path);
      files.delete(path);
      return JSON.parse(value.bytes);
    };
    return {
      manifest,
      composition: entry('composition'),
      animation: entry('animation'),
      effectProgram: entry('effect_program'),
      assets: files
    };
  };
  for (const id of ids) {
    const templates = project.timeline.tracks.flatMap((track) => track.items)
      .map((item) => item.clip.text?.template).filter((template) => template?.id === id);
    if (templates.some((template) => JSON.stringify(template.recipe || null) !== JSON.stringify(templates[0].recipe || null)))
      throw new Error('相同文字模板 ID 必须使用相同组件配方');
    const recipe = resolveRecipe(templates[0]);
    const { bundle } = await composeRecipe(recipe, load);
    const local = new Map(),
      visiting = new Set();
    async function resource(path) {
      if (local.has(path)) return local.get(path);
      if (visiting.has(path)) throw new Error('模板资源依赖成环');
      const asset = bundle.assets.get(path);
      if (!asset) throw new Error(`缺少模板资源：${path}`);
      if (isFontResource(path, asset)) throw new Error('模板字体必须使用本机系统字体引用');
      visiting.add(path);
      const json = asset.mediaType.includes('json');
      const bytes = json
        ? Buffer.from(JSON.stringify(await rewriteWrapper(JSON.parse(asset.bytes))))
        : asset.bytes;
      // Native text resources use Image assets for all non-font payloads, including Lottie.
      const kind = 'image';
      const registered = await register(bytes, asset.mediaType, kind, extname(path));
      local.set(path, registered);
      visiting.delete(path);
      return registered;
    }
    async function rewriteWrapper(value) {
      if (typeof value === 'string' && value.startsWith('asset://'))
        return (await resource(value.slice(8))).id;
      if (Array.isArray(value)) {
        const out = [];
        for (const item of value) out.push(await rewriteWrapper(item));
        return out;
      }
      if (value && typeof value === 'object') {
        const out = {};
        for (const [k, v] of Object.entries(value)) out[k] = await rewriteWrapper(v);
        if (typeof value.asset_id === 'string' && value.asset_id.startsWith('asset://'))
          out.digest = (await resource(value.asset_id.slice(8))).digest;
        return out;
      }
      return value;
    }
    for (const [path, asset] of bundle.assets)
      if (!isFontResource(path, asset)) await resource(path);
    const resolve = (id) => (id.startsWith('asset://') ? local.get(id.slice(8)) : undefined);
    function normalize(value) {
      if (Array.isArray(value)) return value.map(normalize);
      if (!value || typeof value !== 'object') return value;
      const out = Object.fromEntries(Object.entries(value).map(([k, v]) => [k, normalize(v)]));
      if (typeof value.asset_id === 'string') {
        const asset = resolve(value.asset_id);
        if (asset) {
          out.asset_id = asset.id;
          if (!('id' in value)) out.digest = asset.digest;
          if ('ownership' in value) out.ownership = 'project_managed';
          if ('source_kind' in value) out.source_kind = 'project_managed';
          if (value.kind === 'builtin') out.kind = 'project_managed';
        } else if (value.asset_id) throw new Error(`未打包的原生模板资源：${value.asset_id}`);
      }
      return out;
    }
    const normalized = normalize(
      rewriteTemplateFonts(
        {
          composition: bundle.composition,
          animation: bundle.animation,
          effectProgram: bundle.effectProgram
        },
        catalog,
        { native: true }
      )
    );
    bundles[id] = normalized;
  }
  return { bundles, assets: [...assets.values()], project: resolvedProject };
}
