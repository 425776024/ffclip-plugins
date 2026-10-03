import { parseHtmlProperties, applyHtmlProperties } from './html-properties';
import type { HtmlContent } from './html-presets';
import { translate, type Locale } from './i18n/runtime.mjs';

/** Only called when inserting a built-in preset, never on existing authored clips. */
export function localizeHtmlPreset(preset: HtmlContent, locale: Locale): HtmlContent {
  const content = structuredClone(preset);
  if (locale === 'zh-CN') return content;
  const fields = parseHtmlProperties(content);
  for (const field of fields) {
    if (field.kind === 'text' || (field.group === 'variable' && typeof field.value === 'string'))
      field.value = translate(locale, String(field.value));
  }
  return applyHtmlProperties(content, fields);
}
