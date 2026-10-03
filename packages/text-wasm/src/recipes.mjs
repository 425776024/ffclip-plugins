import { authoredRecipes } from './preset-designs.mjs';
export const recipes = authoredRecipes;
// Authorable combinations use only resources shipped with this plugin.
export const TEMPLATE_PARTS = {
  base: [...new Set(recipes.map((recipe) => recipe.base))],
  backdrop: ['bubble-tile', 'bubble-nine-slice'],
  animation: ['anim-lua-letter-transform']
};
export const TEMPLATE_COMPOSITION_RULES = {
  overlayBases: ['flower-style-03', 'flower-style-38'],
  originalOnlyBases: TEMPLATE_PARTS.base.filter(
    (base) => !['flower-style-03', 'flower-style-38'].includes(base)
  ),
  overlays: ['backdrop', 'animation'],
  note: 'Only flower-style-03 and flower-style-38 support added backdrop/animation components. Other bases retain their original execution graph and may only be selected without added components.'
};
export function assertRecipeCombination(recipe) {
  if (
    (recipe.backdrop !== undefined || recipe.animation !== undefined) &&
    !TEMPLATE_COMPOSITION_RULES.overlayBases.includes(recipe.base)
  )
    throw new Error(
      '新增背景或逐字动画只支持 flower-style-03、flower-style-38；其他模板必须保留原始执行图'
    );
}
export function resolveRecipe(template) {
  if (!template.recipe) {
    const recipe = recipes.find((entry) => entry.id === template.id);
    if (!recipe) throw new Error('未找到文字模板');
    return recipe;
  }
  const recipe = template.recipe;
  for (const key of ['base', 'backdrop', 'animation'])
    if ((key === 'base' || recipe[key] !== undefined) && !TEMPLATE_PARTS[key].includes(recipe[key]))
      throw new Error(`不支持的文字模板组件：${key}`);
  assertRecipeCombination(recipe);
  const base = recipes.find((entry) => entry.base === recipe.base);
  return {
    ...recipe,
    id: template.id,
    external: !!base?.external,
    text: base?.text || '花字',
    timeUs: base?.timeUs || 350000
  };
}
export async function composeRecipe(recipe, load) {
  assertRecipeCombination(recipe);
  const bundle = await load(recipe.base);
  const sources = [recipe.base];
  const mergeRules = (rules) => {
    const selector = ({ channel, content_slot_id, target_layer_id, semantic_role }) =>
      JSON.stringify([channel, content_slot_id, target_layer_id, semantic_role]);
    const replacements = new Map(rules.map((rule) => [selector(rule), structuredClone(rule)]));
    bundle.composition.rules = (bundle.composition.rules || []).filter(
      (rule) => !replacements.has(selector(rule))
    );
    bundle.composition.rules.push(...replacements.values());
  };
  if (recipe.backdrop) {
    const bg = await load(recipe.backdrop);
    sources.push(recipe.backdrop);
    // Prefix the complete backdrop resource closure; both native packages can
    // legitimately own assets/asset-000.png with different pixels.
    const remap = (v) =>
      Array.isArray(v)
        ? v.map(remap)
        : v && typeof v === 'object'
          ? Object.fromEntries(Object.entries(v).map(([k, x]) => [k, remap(x)]))
          : typeof v === 'string' && v.startsWith('asset://')
            ? v.replace('asset://', 'asset://backdrop/')
            : v;
    bundle.composition.appearance.backdrops = remap(bg.composition.appearance.backdrops);
    // Native project application obeys channel rules, independently of the
    // standalone preview document. Carry the selected backdrop's authority.
    mergeRules(bg.composition.rules);
    for (const resource of bg.composition.resources)
      bundle.composition.resources.push({
        ...remap(resource),
        resource_id: 'backdrop/' + resource.resource_id
      });
    for (const [path, asset] of bg.assets) bundle.assets.set('backdrop/' + path, asset);
  }
  if (recipe.animation) {
    const motion = await load(recipe.animation);
    sources.push(recipe.animation);
    if (motion.assets.size)
      throw new Error('Recipe animation resource namespacing is not implemented');
    bundle.animation = motion.animation;
    bundle.effectProgram = motion.effectProgram;
    mergeRules(
      motion.composition.rules.filter(
        (rule) => rule.channel === 'animations' || rule.channel === 'decorations'
      )
    );
    bundle.composition.decorations = structuredClone(motion.composition.decorations || []);
  }
  const graph = bundle.animation.animation.execution_graph;
  const standaloneBubble =
    bundle.composition.display?.category === 'bubble_preset' &&
    bundle.composition.appearance?.backdrops?.layers?.length &&
    graph.nodes.length === 2 &&
    graph.nodes[0].capability === 'layout_glyph_run' &&
    graph.nodes[1].id === 'backdrop-material' &&
    graph.nodes[1].capability === 'operator_material';
  if (
    standaloneBubble ||
    (recipe.backdrop && ['flower-style-03', 'flower-style-38'].includes(recipe.base))
  ) {
    // These flower/letter execution graphs own only glyph attachments. Their
    // terminal omits newly authored backdrop components. The native implicit
    // compositor orders every component while still sampling the same glyph
    // materials, animation layers and effect program. Scope this adaptation to
    // the two known flower packages and the legacy standalone bubble graph.
    // The bubble terminal only samples glyph material; it never schedules the
    // nine-slice backdrop. Keep the authored package intact and include that
    // component in the shared preview/export compositor.
    bundle.animation = structuredClone(bundle.animation);
    bundle.animation.animation.execution_graph.nodes = [];
  }
  return { bundle, sources, adapted: !!(recipe.backdrop || recipe.animation || standaloneBubble) };
}
