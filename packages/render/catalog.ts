import { LOOK_NAMES } from '../core/color.mjs';
import { EFFECT_TEMPLATES, TRANSITION_TEMPLATES, type EffectTemplate } from '../core/project.mjs';

/** Admission is deliberately explicit: loading a manifest is not an executable effect. */
export interface VisualPackage {
  id: string;
  version: 1;
  kind: 'effect' | 'transition';
  name: string;
  description: string;
  category: string;
  template: EffectTemplate;
  backend: 'webgpu';
  colorSpace: 'srgb';
  alpha: 'premultiplied';
  nativeOperation: string;
  resources: readonly { id: string; source: 'generated'; version: 1 }[];
  provenance: {
    implementation: string;
    assets: 'none' | 'procedural';
    redistribution: 'project-license';
  };
  acceptance: { editable: true; preview: true; export: true; nativeRoundtrip: boolean };
}

const admitted: Record<
  string,
  {
    description: string;
    category: string;
    nativeOperation: string;
    resource?: string;
  }
> = {
  ...Object.fromEntries(
    [
      'push',
      'zoom',
      'blur-transition',
      'fan',
      'circle',
      'diamond',
      'clock',
      'pageCurl',
      'blinds',
      'flash',
      'stripeWipe',
      'checkerboard',
      'flip',
      'beam',
      'tear',
      'pixelate',
      'rgbSplit'
    ].map((id) => [id, { description: '创意转场', category: '创意转场', nativeOperation: '' }])
  ),
  'color-grade': {
    description: '基础调色、HSL、RGB 曲线与色彩分级',
    category: '调色',
    nativeOperation: ''
  },
  looks: { description: '黑白、胶片、日系与复古等滤镜', category: '调色', nativeOperation: '' },
  detail: { description: '锐化、颗粒与暗角', category: '光影', nativeOperation: '' },
  'custom-lut': { description: '导入三维 .cube 色彩查找表', category: '调色', nativeOperation: '' },
  blur: {
    description: '柔化背景与细节，可调整模糊半径',
    category: '光影',
    nativeOperation: 'com.videocut.effect.blur'
  },
  glow: {
    description: '高光柔和扩散，可调整半径与强度',
    category: '光影',
    nativeOperation: 'com.videocut.effect.creative-lab'
  },
  lut: {
    description: '暖调、冷调与电影色彩，强度支持关键帧',
    category: '调色',
    nativeOperation: 'com.videocut.effect.looks-lut',
    resource: 'videocut-sdr-looks-33'
  },
  dissolve: {
    description: '前后画面自然叠化',
    category: '基础转场',
    nativeOperation: 'com.videocut.transition.standard'
  },
  fade: {
    description: '经过黑色或白色连接两段画面',
    category: '基础转场',
    nativeOperation: 'com.videocut.transition.standard'
  },
  wipe: {
    description: '沿指定方向逐步显现下一段画面',
    category: '方向转场',
    nativeOperation: 'com.videocut.transition.standard'
  },
  slide: {
    description: '前后画面沿指定方向一起推移',
    category: '方向转场',
    nativeOperation: 'com.videocut.transition.standard'
  }
};

function packages(
  kind: VisualPackage['kind'],
  templates: readonly EffectTemplate[]
): readonly VisualPackage[] {
  return Object.freeze(
    templates.map((template) => {
      const entry =
        admitted[kind === 'transition' && template.id === 'blur' ? 'blur-transition' : template.id];
      if (!entry) throw new Error(`模板尚未完成执行验收：${template.id}`);
      return Object.freeze({
        id: template.id,
        version: 1 as const,
        kind,
        name: template.name,
        description: entry.description,
        category: entry.category,
        template,
        backend: 'webgpu' as const,
        colorSpace: 'srgb' as const,
        alpha: 'premultiplied' as const,
        nativeOperation: entry.nativeOperation,
        resources: entry.resource
          ? [{ id: entry.resource, source: 'generated' as const, version: 1 as const }]
          : [],
        provenance: {
          implementation: 'VideoCut Web',
          assets: entry.resource ? ('procedural' as const) : ('none' as const),
          redistribution: 'project-license' as const
        },
        acceptance: {
          editable: true as const,
          preview: true as const,
          export: true as const,
          nativeRoundtrip: Boolean(entry.nativeOperation)
        }
      });
    })
  );
}

// Parameters are the model's objects, not a second copy in the library or renderer.
export const EFFECT_PACKAGES = Object.freeze(
  packages('effect', EFFECT_TEMPLATES).flatMap((pack) =>
    pack.id === 'looks'
      ? Object.entries(LOOK_NAMES)
          .filter(([id]) => id !== 'none')
          .map(([id, name]) => ({
            ...pack,
            id: 'look-' + id,
            name,
            template: {
              ...pack.template,
              parameters: {
                ...pack.template.parameters,
                preset: { ...pack.template.parameters.preset, default: id }
              }
            }
          }))
      : [pack]
  )
);
export const TRANSITION_PACKAGES = packages('transition', TRANSITION_TEMPLATES);
export const VISUAL_PACKAGES = Object.freeze([...EFFECT_PACKAGES, ...TRANSITION_PACKAGES]);
export function visualPackage(kind: VisualPackage['kind'], id: string): VisualPackage {
  const found = VISUAL_PACKAGES.find((value) => value.kind === kind && value.id === id);
  if (!found) throw new Error(`未开放的模板：${kind}/${id}`);
  return found;
}
