import { english } from './messages.mjs';

/** Use the first preferred language; other languages fall back to English. */
export function resolveLocale(languages = [], fallback = '') {
  const language = languages.find((value) => typeof value === 'string' && value.trim()) || fallback;
  return /^zh(?:[-_]|$)/i.test(language.trim()) ? 'zh-CN' : 'en';
}

export function browserLocale(navigatorLike = globalThis.navigator) {
  return resolveLocale(navigatorLike?.languages || [], navigatorLike?.language || '');
}

export function translate(locale, message, params = {}) {
  if (!message) return '';
  const template = locale === 'zh-CN' ? message : (english[message] ?? message);
  return template.replace(/\{(\w+)\}/g, (token, key) =>
    Object.hasOwn(params, key) ? String(params[key]) : token
  );
}

// Legacy engine/job messages already contain values. Translate their presentation
// without changing error contracts or substituting text inside a path or filename.
const patterns = Object.entries(english)
  .filter(([key]) => key.includes('{'))
  .map(([key]) => {
    const names = [];
    const expression = key
      .split(/(\{\w+\})/)
      .map((part) => {
        if (/^\{\w+\}$/.test(part)) {
          names.push(part.slice(1, -1));
          return '(.*?)';
        }
        return part.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
      })
      .join('');
    return { key, names, regex: new RegExp(`^${expression}$`, 's') };
  });

export function translateMessage(locale, message) {
  if (!message || locale === 'zh-CN' || Object.hasOwn(english, message))
    return translate(locale, message);
  for (const { key, names, regex } of patterns) {
    const match = regex.exec(message);
    if (!match) continue;
    const params = Object.fromEntries(
      names.map((name, i) => [
        name,
        ['label', 'side', 'kind', 'skipped'].includes(name)
          ? translateMessage(locale, match[i + 1])
          : match[i + 1]
      ])
    );
    return translate(locale, key, params);
  }
  return message;
}
