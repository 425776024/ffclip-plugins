import {
  EFFECT_TEMPLATES, TRANSITION_TEMPLATES,
  type EffectTemplate
} from '../core/project.mjs';

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
  provenance: { implementation: string; assets: 'none' | 'procedural'; redistribution: 'project-license' };
  acceptance: { editable: true; preview: true; export: true; nativeRoundtrip: true };
}

const admitted: Record<string, {
  description: string; category: string; nativeOperation: string; resource?: string
}> = {
  blur: { description: '柔化背景与细节，可调整模糊半径', category: '光影', nativeOperation: 'com.videocut.effect.blur' },
  glow: { description: '高光柔和扩散，可调整半径与强度', category: '光影', nativeOperation: 'com.videocut.effect.creative-lab' },
  lut: { description: '暖调、冷调与电影色彩，强度支持关键帧', category: '调色', nativeOperation: 'com.videocut.effect.looks-lut', resource: 'videocut-sdr-looks-33' },
  dissolve: { description: '前后画面自然叠化', category: '基础转场', nativeOperation: 'com.videocut.transition.standard' },
  fade: { description: '经过黑色或白色连接两段画面', category: '基础转场', nativeOperation: 'com.videocut.transition.standard' },
  wipe: { description: '沿指定方向逐步显现下一段画面', category: '方向转场', nativeOperation: 'com.videocut.transition.standard' },
  slide: { description: '前后画面沿指定方向一起推移', category: '方向转场', nativeOperation: 'com.videocut.transition.standard' }
};

function packages(kind: VisualPackage['kind'], templates: readonly EffectTemplate[]): readonly VisualPackage[] {
  return Object.freeze(templates.map((template) => {
    const entry = admitted[template.id];
    if (!entry) throw new Error(`模板尚未完成执行验收：${template.id}`);
    return Object.freeze({
      id: template.id, version: 1 as const, kind, name: template.name,
      description: entry.description, category: entry.category, template,
      backend: 'webgpu' as const, colorSpace: 'srgb' as const, alpha: 'premultiplied' as const,
      nativeOperation: entry.nativeOperation,
      resources: entry.resource ? [{ id: entry.resource, source: 'generated' as const, version: 1 as const }] : [],
      provenance: { implementation: 'VideoCut Web', assets: entry.resource ? 'procedural' as const : 'none' as const, redistribution: 'project-license' as const },
      acceptance: { editable: true as const, preview: true as const, export: true as const, nativeRoundtrip: true as const }
    });
  }));
}

// Parameters are the model's objects, not a second copy in the library or renderer.
export const EFFECT_PACKAGES = packages('effect', EFFECT_TEMPLATES);
export const TRANSITION_PACKAGES = packages('transition', TRANSITION_TEMPLATES);
export const VISUAL_PACKAGES = Object.freeze([...EFFECT_PACKAGES, ...TRANSITION_PACKAGES]);
export function visualPackage(kind: VisualPackage['kind'], id: string): VisualPackage {
  const found = VISUAL_PACKAGES.find((value) => value.kind === kind && value.id === id);
  if (!found) throw new Error(`未开放的模板：${kind}/${id}`);
  return found;
}
