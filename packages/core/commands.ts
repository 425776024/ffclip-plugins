import type {
  Asset,
  VisualProperties,
  AudioProperties,
  TextContent,
  HtmlContent
} from './types.js';

export type RippleScope = 'selected' | 'all' | 'syncLocked';
type ItemTarget = { itemId: string; expandLinked?: boolean };
type SelectionTarget = { itemIds: string[]; expandLinked?: boolean };
export interface PropertyValues {
  'visual.positionX': number;
  'visual.positionY': number;
  'visual.scaleX': number;
  'visual.scaleY': number;
  'visual.rotationDegrees': number;
  'visual.opacity': number;
  'visual.anchorX': number;
  'visual.anchorY': number;
  'visual.crop.left': number;
  'visual.crop.top': number;
  'visual.crop.right': number;
  'visual.crop.bottom': number;
  'visual.fitPolicy': NonNullable<VisualProperties['fitPolicy']>;
  'visual.blendMode': NonNullable<VisualProperties['blendMode']>;
  'visual.flipHorizontal': boolean;
  'visual.flipVertical': boolean;
  'audio.gainLinear': number;
  'audio.muted': boolean;
  'audio.fadeIn': number;
  'audio.fadeOut': number;
  'text.content': string;
  'text.fontSize': number;
  'text.color': string;
  'time.start': number;
  'time.duration': number;
  'time.sourceIn': number;
  'time.speed': number;
}
type PropertyCommand = {
  [P in keyof PropertyValues]: ItemTarget & {
    action: 'set_property';
    property: P;
    value: PropertyValues[P];
    ripple?: boolean;
  };
}[keyof PropertyValues];
export type KeyframeProperty =
  | 'visual.positionX'
  | 'visual.positionY'
  | 'visual.scaleX'
  | 'visual.scaleY'
  | 'visual.rotationDegrees'
  | 'visual.opacity'
  | 'visual.anchorX'
  | 'visual.anchorY'
  | 'audio.gainLinear'
  | `effects.${string}.amount`;
export type Interpolation = 'linear' | 'hold' | 'easeIn' | 'easeOut' | 'easeInOut';
export type EffectParameterPatch = {
  radius?: number;
  strength?: number;
  amount?: number;
  preset?: 'warm' | 'cool' | 'cinema';
};
export type EffectArguments =
  | { templateId: 'blur'; parameters?: { radius?: number } }
  | { templateId: 'glow'; parameters?: { radius?: number; strength?: number } }
  | { templateId: 'lut'; parameters?: { preset?: 'warm' | 'cool' | 'cinema'; amount?: number } };
export type TransitionArguments =
  | { templateId: 'dissolve'; parameters?: Record<string, never> }
  | { templateId: 'fade'; parameters?: { color?: '#000000' | '#ffffff' } }
  | { templateId: 'wipe' | 'slide'; parameters?: { direction?: 'left' | 'right' | 'up' | 'down' } };

/** Authored command input. Unknown actions/fields are not a public escape hatch. */
export type EditorCommand =
  | { action: 'undo' | 'redo' }
  | {
      action: 'configure_project';
      name?: string;
      width?: number;
      height?: number;
      fps?: number;
      frameRate?: { numerator: number; denominator: number };
    }
  | { action: 'add_asset' | 'add_media'; asset: Asset; startSeconds?: number; trackId?: string }
  | {
      action: 'add_text';
      content?: string;
      fontSize?: number;
      color?: string;
      startSeconds?: number;
      durationSeconds?: number;
      template?: TextContent['template'];
    }
  | {
      action: 'add_subtitles';
      segments: { text: string; start: number; end: number }[];
      name?: string;
    }
  | {
      action: 'add_html_clip';
      html: HtmlContent;
      name?: string;
      start?: number;
      length?: number;
      trackId?: string;
    }
  | (ItemTarget & { action: 'set_html_clip'; html: HtmlContent; name?: string })
  | (ItemTarget & { action: 'trim_clip'; sourceInSeconds: number; durationSeconds: number })
  | (ItemTarget & { action: 'trim_range' | 'trim_edges'; beginSeconds: number; endSeconds: number })
  | (ItemTarget & { action: 'move_clip'; startSeconds: number; trackId?: string })
  | (ItemTarget & { action: 'split_clip'; atSeconds: number })
  | (ItemTarget & { action: 'remove_clip' })
  | (ItemTarget & { action: 'set_transform' } & Partial<VisualProperties>)
  | (ItemTarget & { action: 'set_audio' } & Partial<AudioProperties>)
  | (ItemTarget & {
      action: 'set_text';
      template?: TextContent['template'] | null;
      templateStyle?: { color?: string | null; fontSize?: number | null };
      layoutWidth?: number | null;
    } & Partial<Pick<TextContent, 'content' | 'fontSize' | 'color'>>)
  | (ItemTarget & { action: 'reorder_track'; index: number })
  | (SelectionTarget & { action: 'move_clips'; deltaSeconds: number; trackId?: string })
  | (SelectionTarget & { action: 'duplicate_clips'; offsetSeconds?: number })
  | (SelectionTarget & { action: 'group_clips' | 'link_clips' | 'delete_clips' })
  | (SelectionTarget & { action: 'ripple_delete'; scope?: RippleScope })
  | { action: 'ungroup_clips' | 'unlink_clips'; groupId: string }
  | (ItemTarget & { action: 'set_speed'; ripple?: boolean } & (
        { rate: number; constantRatePpm?: never } | { constantRatePpm: number; rate?: never }
      ))
  | {
      action: 'set_track';
      trackId: string;
      visible?: boolean;
      muted?: boolean;
      locked?: boolean;
      syncLocked?: boolean;
      name?: string;
    }
  | PropertyCommand
  | (ItemTarget & {
      action: 'set_property';
      property:
        `effects.${string}.radius` | `effects.${string}.strength` | `effects.${string}.amount`;
      value: number;
    })
  | (ItemTarget & {
      action: 'set_property';
      property: `effects.${string}.preset`;
      value: 'warm' | 'cool' | 'cinema';
    })
  | (ItemTarget & {
      action: 'set_keyframe';
      property: KeyframeProperty;
      timeSeconds: number;
      value: number;
      interpolation?: Interpolation;
    })
  | (ItemTarget & { action: 'remove_keyframe'; property: KeyframeProperty; keyframeId: string })
  | (ItemTarget & { action: 'add_effect' } & EffectArguments)
  | (ItemTarget & {
      action: 'update_effect';
      effectId: string;
      enabled?: boolean;
      parameters?: EffectParameterPatch;
    })
  | (ItemTarget & { action: 'remove_effect'; effectId: string })
  | (ItemTarget & { action: 'reorder_effects'; effectIds: string[] })
  | ({
      action: 'add_transition';
      fromItemId: string;
      toItemId: string;
      durationSeconds?: number;
    } & TransitionArguments)
  | { action: 'remove_transition'; transitionId: string }
  | {
      action: 'update_transition';
      transitionId: string;
      durationSeconds?: number;
      parameters?: { color?: '#000000' | '#ffffff'; direction?: 'left' | 'right' | 'up' | 'down' };
    };
