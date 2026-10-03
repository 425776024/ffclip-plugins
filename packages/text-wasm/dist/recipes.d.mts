import type { TemplateBundle } from './index.mjs';
export interface TextRecipe {
  category: 'flower' | 'bubble' | 'animation';
  id: string;
  name: string;
  tag: string;
  base: string;
  external?: boolean;
  backdrop?: string;
  animation?: string;
  text: string;
  timeUs: number;
}
export const recipes: readonly TextRecipe[];
export const TEMPLATE_PARTS: { base: string[]; backdrop: string[]; animation: string[] };
export const TEMPLATE_COMPOSITION_RULES: {
  overlayBases: string[];
  originalOnlyBases: string[];
  overlays: string[];
  note: string;
};
export function resolveRecipe(template: {
  id: string;
  recipe?: { base: string; backdrop?: string; animation?: string };
}): TextRecipe;
export function composeRecipe(
  recipe: TextRecipe,
  load: (id: string) => Promise<TemplateBundle>
): Promise<{ bundle: TemplateBundle; sources: string[]; adapted: boolean }>;
