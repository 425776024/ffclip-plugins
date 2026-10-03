// Authored document DTOs. Persistence remains the canonical desktop Format 1 directory.
export type AssetKind = 'video' | 'audio' | 'image';

export interface Asset {
  id: string;
  name: string;
  kind: AssetKind;
  path: string;
  duration: number;
  size: number;
  width: number;
  height: number;
  hasAudio: boolean;
  sourceIdentity?: string;
  firstTimestamp?: number;
  streams?: Record<string, unknown>[];
  codec?: string;
  fingerprint?: string;
  nativeProbe?: { streams: Record<string, unknown>[] };
}

export interface Item {
  id: string;
  name: string;
  enabled: boolean;
  placement: { begin: number; end: number };
  clip: {
    id: string;
    type: AssetKind | 'text' | 'html-clip';
    assetId: string;
    text?: TextContent;
    html?: HtmlContent;
    source: { begin: number; end: number };
    visual: VisualProperties;
    audio: AudioProperties;
    retime?: { version: 1; mode: 'constant'; constantRatePpm: number };
    automation?: Record<string, AutomationBinding>;
    effects?: EffectInstance[];
  };
}

export interface Track {
  id: string;
  name: string;
  type: 'video' | 'audio' | 'text';
  visible: boolean;
  muted: boolean;
  locked: boolean;
  syncLocked?: boolean;
  items: Item[];
}

export interface Project {
  format: string;
  version: number;
  id: string;
  name: string;
  canvas: { width: number; height: number };
  frameRate: { numerator: number; denominator: number };
  audio: { sampleRate: number; channels: number };
  assets: Asset[];
  timeline: {
    id: string;
    tracks: Track[];
    groups?: ItemRelation[];
    links?: ItemRelation[];
    transitions?: TransitionInstance[];
  };
}

export interface TextContent {
  content: string;
  fontSize: number;
  color: string;
  fontFamily: string;
  /** Wrapping width in project canvas pixels, before the layer transform. */
  layoutWidth?: number;
  font?: {
    family: string;
    postscriptName: string;
    weight: number;
    width: number;
    slant: 'upright' | 'italic' | 'oblique';
    sourceFaceIndex: number;
    platform: string;
    identity: string;
  };
  template?: {
    id: string;
    version: 1;
    packageDigest?: string;
    /** Same-origin frozen resources supplied by the server when reopening .vcutweb. */
    resourceBase?: string;
    recipe?: { base: string; backdrop?: string; animation?: string };
    /** Optional edits; absent values retain the template's authored appearance. */
    style?: { color?: string; fontSize?: number };
  };
}

/** Authored HTML animation. Duration and clip source times use the 120,000 Hz clock. */
export interface HtmlContent {
  html: string;
  width: number;
  height: number;
  duration: number;
  transparent: boolean;
  variables?: Record<string, string | number | boolean>;
}

export interface TextTemplate {
  id: string;
  name: string;
  tag: string;
  text: string;
  timeUs: number;
}

export interface VisualProperties {
  positionX: number;
  positionY: number;
  scaleX: number;
  scaleY: number;
  rotationDegrees: number;
  opacity: number;
  anchorX?: number;
  anchorY?: number;
  crop?: { left: number; top: number; right: number; bottom: number };
  fitPolicy?: 'contain' | 'cover' | 'stretch' | 'nativeCrop';
  flipHorizontal?: boolean;
  flipVertical?: boolean;
  blendMode?: 'normal' | 'multiply' | 'screen' | 'overlay' | 'darken' | 'lighten';
}

export interface AudioProperties {
  gainLinear: number;
  muted: boolean;
  fadeIn?: number;
  fadeOut?: number;
}

export interface Keyframe {
  id: string;
  time: number;
  value: number;
  interpolation: 'linear' | 'hold' | 'easeIn' | 'easeOut' | 'easeInOut';
  segment?: { firstControl: { x: number; y: number }; secondControl: { x: number; y: number } };
}

export interface AutomationBinding {
  timeDomain: 'itemLocal';
  keyframes: Keyframe[];
}

export interface EffectInstance {
  id: string;
  templateId: 'blur' | 'glow' | 'lut';
  enabled: boolean;
  parameters: Record<string, number | string>;
}

export interface TransitionInstance {
  id: string;
  fromItemId: string;
  toItemId: string;
  templateId: 'dissolve' | 'fade' | 'wipe' | 'slide';
  duration: number;
  parameters: Record<string, number | string>;
}

export interface ItemRelation {
  id: string;
  itemIds: string[];
}

export interface PropertyDescriptor {
  type: 'number' | 'boolean' | 'enum' | 'text' | 'color';
  owner?: 'visual' | 'audio' | 'text' | 'plainText' | 'media' | 'item';
  impact?: string[];
  maxLength?: number;
  pattern?: string;
  label?: string;
  min?: number;
  max?: number;
  default: number | boolean | string;
  unit?: string;
  step?: number;
  keyframe?: boolean;
  values?: string[];
}

export interface EffectTemplate {
  id: string;
  name: string;
  parameters: Record<string, PropertyDescriptor>;
}

export interface CommandResult {
  action: string;
  itemId?: string;
  itemIds?: string[];
  rightItemId?: string;
  groupId?: string;
  effectId?: string;
  transitionId?: string;
  keyframeId?: string;
}

export interface FrameLayer {
  itemId: string;
  trackId: string;
  assetId: string;
  type: Item['clip']['type'];
  item: Item;
  sourceTime: number;
  itemLocalTime: number;
  visual: Required<VisualProperties>;
  audio: Required<AudioProperties>;
  effects: EffectInstance[];
  transition?: {
    id: string;
    templateId: TransitionInstance['templateId'];
    progress: number;
    role: 'from' | 'to';
    parameters: Record<string, number | string>;
  };
}
