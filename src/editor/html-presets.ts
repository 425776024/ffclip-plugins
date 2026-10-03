import { type Item } from '../../packages/core/project.mjs';
import { PRESENTATION_PRESETS } from './html-presentation-presets';
import { OVERLAY_PRESETS } from './html-overlay-presets';
import prismPoster from './assets/motion/html-poster-prism-launch.png';
import swissPoster from './assets/motion/html-poster-swiss-story.png';
import dataPoster from './assets/motion/html-poster-aurora-data.png';
import velvetPoster from './assets/motion/html-poster-velvet-keynote.png';
import roadmapPoster from './assets/motion/html-poster-blueprint-roadmap.png';
import productPoster from './assets/motion/html-poster-hologram-product.png';
import offerPoster from './assets/motion/html-poster-kinetic-offer.png';
import comparisonPoster from './assets/motion/html-poster-split-comparison.png';
import lowerPoster from './assets/motion/html-poster-lower-third.png';
import titlePoster from './assets/motion/html-poster-tick-title.png';

export type HtmlContent = NonNullable<Item['clip']['html']>;
export interface HtmlPreset {
  id: string;
  name: string;
  html: HtmlContent;
  poster?: string;
}

const presentationPosters: Record<string, string> = {
  'prism-launch': prismPoster,
  'swiss-story': swissPoster,
  'aurora-data': dataPoster,
  'velvet-keynote': velvetPoster,
  'blueprint-roadmap': roadmapPoster,
  'hologram-product': productPoster,
  'kinetic-offer': offerPoster,
  'split-comparison': comparisonPoster,
  'lower-third': lowerPoster,
  'tick-title': titlePoster
};

export const HTML_PRESETS: HtmlPreset[] = [...PRESENTATION_PRESETS, ...OVERLAY_PRESETS].map(
  (preset) => ({ ...preset, poster: presentationPosters[preset.id] })
);
