import { applyDocumentPatches, type Project } from '../core/project.mjs';
import type { DocumentPatch } from '../core/immutable.mjs';

export interface PresentationOverride {
  id: string;
  x: number;
  y: number;
  scaleX?: number;
  scaleY?: number;
  layoutWidth?: number;
}
export interface PreviewDocumentUpdate {
  project?: Project;
  patches?: DocumentPatch[];
  documentRevision?: number;
  baseRevision?: number;
}
/** A worker owns this immutable document; stale deltas request a full snapshot instead of guessing. */
export class PreviewDocument {
  project?: Project;
  revision = 0;
  receive(update: PreviewDocumentUpdate) {
    if (update.project) {
      this.project = update.project;
      this.revision = update.documentRevision ?? this.revision + 1;
      return true;
    }
    if (!this.project) return false;
    if (update.patches) {
      if (update.baseRevision !== this.revision || update.documentRevision !== this.revision + 1)
        return false;
      this.project = applyDocumentPatches(this.project, update.patches);
      this.revision = update.documentRevision;
    } else if (update.documentRevision !== undefined && update.documentRevision !== this.revision)
      return false;
    return true;
  }
}

/** Presentation is ephemeral: copy only the affected path, and never edit authored keyframes. */
export function presentationProject(project: Project, override?: PresentationOverride): Project {
  if (!override) return project;
  if (!Number.isFinite(override.x) || !Number.isFinite(override.y))
    throw new Error('Invalid preview position');
  if (
    override.layoutWidth !== undefined &&
    (!Number.isFinite(override.layoutWidth) ||
      override.layoutWidth < 1 ||
      override.layoutWidth > 65536)
  )
    throw new Error('Invalid preview text width');
  for (const key of ['scaleX', 'scaleY'] as const)
    if (
      override[key] !== undefined &&
      (!Number.isFinite(override[key]) || override[key]! < 0.01 || override[key]! > 20)
    )
      throw new Error('Invalid preview scale');
  const trackIndex = project.timeline.tracks.findIndex((track) =>
    track.items.some((item) => item.id === override.id)
  );
  if (trackIndex < 0) return project;
  const track = project.timeline.tracks[trackIndex],
    itemIndex = track.items.findIndex((item) => item.id === override.id),
    item = track.items[itemIndex];
  let automation = item.clip.automation;
  const changed = [
    'visual.positionX',
    'visual.positionY',
    ...(override.scaleX === undefined ? [] : ['visual.scaleX']),
    ...(override.scaleY === undefined ? [] : ['visual.scaleY'])
  ];
  if (automation && changed.some((key) => key in automation!)) {
    automation = { ...automation };
    for (const key of changed) delete automation[key];
  }
  const items = track.items.slice();
  items[itemIndex] = {
    ...item,
    clip: {
      ...item.clip,
      automation,
      ...(override.layoutWidth === undefined
        ? {}
        : {
            text: { ...item.clip.text!, layoutWidth: override.layoutWidth }
          }),
      visual: {
        ...item.clip.visual,
        positionX: override.x,
        positionY: override.y,
        ...(override.scaleX === undefined ? {} : { scaleX: override.scaleX }),
        ...(override.scaleY === undefined ? {} : { scaleY: override.scaleY })
      }
    }
  };
  const tracks = project.timeline.tracks.slice();
  tracks[trackIndex] = { ...track, items };
  return { ...project, timeline: { ...project.timeline, tracks } };
}
