import { readonly, ref } from 'vue';
import { browserLocale, translate, translateMessage, type MessageParams } from './i18n/runtime.mjs';

const locale = ref(browserLocale());

export function useI18n() {
  return {
    locale: readonly(locale),
    tr: (message: string | null | undefined, params?: MessageParams) =>
      params ? translate(locale.value, message, params) : translateMessage(locale.value, message)
  };
}

export function followSystemLanguage() {
  const update = () => {
    locale.value = browserLocale();
    document.documentElement.lang = locale.value;
    if (!location.pathname.startsWith('/projects/') && !location.search)
      document.title = translate(locale.value, 'VideoCut · 本地剪辑');
  };
  update();
  window.addEventListener('languagechange', update);
  return () => window.removeEventListener('languagechange', update);
}
