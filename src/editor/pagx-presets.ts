import { PAGX_TEMPLATES, createPagxTemplate } from '../../packages/pagx/templates.mjs';
import type { PagxContent } from '../../packages/core/types';
const posters = import.meta.glob('./assets/motion/pagx-poster-*.png', {
  eager: true,
  query: '?url',
  import: 'default'
}) as Record<string, string>;
export const PAGX_PRESETS = PAGX_TEMPLATES.map((template) => ({
  ...template,
  poster: posters[`./assets/motion/pagx-poster-${template.id}.png`],
  posterEn: posters[`./assets/motion/pagx-poster-${template.id}-en.png`]
}));
export function pagxPreset(id: string, locale: 'zh' | 'en'): { name: string; pagx: PagxContent } {
  return createPagxTemplate(id, { locale });
}
