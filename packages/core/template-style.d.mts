import type { TemplateBundle } from '../text-wasm/src/index.mjs';
import type { TextContent } from './types';
export function applyTemplateStyle(
  bundle: TemplateBundle,
  style?: NonNullable<TextContent['template']>['style']
): TemplateBundle;
