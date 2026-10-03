export type Locale = 'zh-CN' | 'en';
export type MessageParams = Readonly<Record<string, string | number>>;
export function resolveLocale(languages?: readonly string[], fallback?: string): Locale;
export function browserLocale(navigatorLike?: {
  readonly languages?: readonly string[];
  readonly language?: string;
}): Locale;
export function translate(
  locale: Locale,
  message: string | null | undefined,
  params?: MessageParams
): string;
export function translateMessage(locale: Locale, message: string | null | undefined): string;
