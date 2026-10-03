import {
  evaluateVisual,
  evaluateEffects,
  mapTimelineToSource,
  activeTransitions,
  type Project
} from '../core/project.mjs';
import { parseColor } from './plan.mjs';
import { htmlSourceTime } from './html';

/** Compile topology separately from evaluated values. Audio edits do not change video dependencies. */
export class SceneGraph {
  private shape = '';
  private nodes: ReadonlyArray<{
    trackId: string;
    itemIds: readonly string[];
    transitionIds: readonly string[];
  }> = [];
  builds = 0;
  evaluate(project: Project, time: number, width: number, height: number) {
    const tracks = project.timeline.tracks.filter((t) => t.type !== 'audio');
    const shape = JSON.stringify([
      tracks.map((t) => [
        t.id,
        t.visible,
        t.items
          .filter((i) => i.clip.type !== 'audio')
          .map((i) => [
            i.id,
            i.enabled,
            i.clip.type,
            i.clip.assetId,
            i.clip.text?.template?.id,
            i.clip.effects?.map((f) => [f.id, f.templateId])
          ])
      ]),
      (project.timeline.transitions || []).map((t) => [
        t.id,
        t.fromItemId,
        t.toItemId,
        t.templateId
      ])
    ]);
    if (shape !== this.shape) {
      this.shape = shape;
      // Store only topology identities: mutable projections are rebound each evaluation,
      // while audio/parameter edits retain these compiled node descriptors.
      this.nodes = [...tracks].reverse().map((t) => ({
        trackId: t.id,
        itemIds: t.items.filter((i) => i.clip.type !== 'audio').map((i) => i.id),
        transitionIds: (project.timeline.transitions || [])
          .filter((tr) => t.items.some((i) => i.id === tr.fromItemId))
          .map((tr) => tr.id)
      }));
      this.builds++;
    }
    const byTrack = new Map(tracks.map((t) => [t.id, t]));
    const assets = new Map(project.assets.map((a) => [a.id, a]));
    const all = new Map(
      project.timeline.tracks.flatMap((track) =>
        track.items.map((item) => [item.id, { track, item }] as const)
      )
    );
    const active = activeTransitions(project, time);
    const endpoints = new Set(active.flatMap((t) => [t.fromItemId, t.toItemId]));
    const layer = ({ track, item }: any) => ({
      track,
      item,
      asset: assets.get(item.clip.assetId),
      visual: evaluateVisual(item, time),
      sourceTime: mapTimelineToSource(item, time, { clamp: false }),
      effects: evaluateEffects(item, time)
    });
    const layers: any[] = [];
    const byTransition = new Map(active.map((tr) => [tr.id, tr]));
    for (const node of this.nodes) {
      const track = byTrack.get(node.trackId)!;
      if (!track.visible) continue;
      for (const id of node.itemIds) {
        const item = all.get(id)!.item;
        if (
          item.enabled &&
          item.clip.type !== 'audio' &&
          !endpoints.has(item.id) &&
          time >= item.placement.begin &&
          time < item.placement.end
        )
          layers.push({ kind: 'layer', layer: layer({ track, item }) });
      }
      for (const id of node.transitionIds) {
        const tr = byTransition.get(id);
        if (!tr) continue;
        const from = all.get(tr.fromItemId),
          to = all.get(tr.toItemId);
        if (!from || !to || from.track.id !== track.id || !from.item.enabled || !to.item.enabled)
          continue;
        layers.push({
          kind: 'transition',
          from: layer(from),
          to: layer(to),
          transition: {
            ...tr,
            style:
              (
                {
                  dissolve: 'cross_dissolve',
                  fade: 'fade_color',
                  wipe: 'wipe',
                  slide: 'slide'
                } as Record<string, string>
              )[tr.templateId] || tr.templateId,
            direction: tr.parameters?.direction || 'right',
            color: parseColor(tr.parameters?.color)
          }
        });
      }
    }
    return {
      project,
      time,
      width,
      height,
      layers,
      background: parseColor(
        (project.canvas as any).backgroundColor || (project.canvas as any).background
      )
    };
  }
}

/** Bounded dependency labels prevent nested composite signatures from growing with graph depth. */
export function renderIdentity(...parts: unknown[]) {
  const text = JSON.stringify(parts);
  let a = 2166136261,
    b = 3335557771;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    a = Math.imul(a ^ c, 16777619);
    b = Math.imul(b ^ c, 2246822519);
  }
  return (a >>> 0).toString(36) + ':' + (b >>> 0).toString(36) + ':' + text.length;
}

export function visualFrameIdentity(plan: any, mediaUrl: (id: string) => string) {
  const layer = (value: any) => [
    value.item.id,
    value.asset
      ? [
          value.asset.sourceIdentity || mediaUrl(value.asset.id),
          value.asset.width,
          value.asset.height
        ]
      : null,
    value.visual,
    value.effects,
    value.item.clip.text || null,
    value.item.clip.html || null,
    value.item.clip.html
      ? htmlSourceTime(value.item.clip.html, value.sourceTime)
      : value.asset?.kind === 'video' || value.item.clip.text?.template
        ? value.sourceTime
        : null
  ];
  return JSON.stringify([
    plan.project.canvas.width,
    plan.project.canvas.height,
    plan.width,
    plan.height,
    plan.background,
    plan.layers.map((entry: any) =>
      entry.kind === 'layer'
        ? layer(entry.layer)
        : [layer(entry.from), layer(entry.to), entry.transition]
    )
  ]);
}

/** Bake effects in a translation-independent padded surface, then place the result on the project canvas. */
export function localEffectGeometry(geometry: any, effects: any[]) {
  const [a, b, c, d, e, f] = geometry.matrix;
  let padding = 0;
  for (const effect of effects) {
    if (effect.enabled === false) continue;
    const p = effect.parameters || {};
    if (effect.templateId === 'blur') padding += Math.ceil((p.radius ?? 8) * geometry.renderScale);
    if (effect.templateId === 'glow')
      padding += Math.ceil(3 * (p.radius ?? 12) * geometry.renderScale);
  }
  const xs = [0, a, c, a + c],
    ys = [0, b, d, b + d];
  const x = Math.min(...xs) - padding,
    y = Math.min(...ys) - padding;
  const width = Math.max(1, Math.ceil(Math.max(...xs) - x + padding)),
    height = Math.max(1, Math.ceil(Math.max(...ys) - y + padding));
  const det = a * d - b * c,
    safe = Math.abs(det) < 1e-12 ? 1e-12 : det;
  return {
    width,
    height,
    x: e + x,
    y: f + y,
    geometry: {
      ...geometry,
      inverse: [
        (d * width) / safe,
        (-c * height) / safe,
        (d * x - c * y) / safe,
        (-b * width) / safe,
        (a * height) / safe,
        (a * y - b * x) / safe
      ]
    }
  };
}
