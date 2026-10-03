export interface SystemTemplateFont {
  id: string;
  family: string;
  postscriptName: string;
  weight: number;
  width: number;
  slant: 'upright' | 'italic' | 'oblique';
  sourceFaceIndex: number;
  platform: string;
  identity: string;
}
export interface SystemTemplateFontCatalog {
  fonts: SystemTemplateFont[];
  defaults: { sans: string | null; cjk: string | null };
}
export interface PlainTextFontIdentity {
  family: string;
  postscriptName: string;
  weight: number;
  width: number;
  slant: 'upright' | 'italic' | 'oblique';
  sourceFaceIndex: number;
  platform: string;
  identity: string;
}
export function plainTextFontProjection(font: SystemTemplateFont): PlainTextFontIdentity;
export function choosePlainTextFont(
  catalog: SystemTemplateFontCatalog,
  text: { content: string; fontFamily?: string; font?: PlainTextFontIdentity }
): SystemTemplateFont;
export function chooseTemplateFonts(catalog: SystemTemplateFontCatalog): {
  sans: SystemTemplateFont;
  cjk: SystemTemplateFont;
};
export function rewriteTemplateFonts<T>(
  value: T,
  catalog: SystemTemplateFontCatalog,
  options?: { native?: boolean }
): T;
