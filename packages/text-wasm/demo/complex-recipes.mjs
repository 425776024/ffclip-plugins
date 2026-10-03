import { loadTextTemplate } from '../dist/index.mjs';
import { recipes, composeRecipe } from '../dist/recipes.mjs';
export { recipes };
export function loadRecipe(
  recipe,
  fetcher = globalThis.fetch,
  baseUrl = new URL('../fixtures/templates/', import.meta.url)
) {
  return composeRecipe(recipe, (id) =>
    loadTextTemplate(new URL(`com.videocut.text.qt-type.${id}/manifest.json`, baseUrl), {
      fetch: fetcher
    })
  );
}
