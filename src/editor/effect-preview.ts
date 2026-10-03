import { addAsset, createProject, editTimeline, ticks, type Asset, type EffectInstance, type TransitionInstance } from '../../packages/core/project.mjs';
import { SceneRenderer } from '../../packages/render/renderer';
import type { VisualPackage } from '../../packages/render/catalog';

/** Bounded gallery: one compositor, sequential jobs, seven small cached PNGs. */
export class EffectPreviewGallery {
  private canvas = document.createElement('canvas');
  private renderer = new SceneRenderer(this.canvas, (id) => this.urls.get(id)!);
  private urls = new Map<string, string>();
  private posters = new Map<string, string>();
  private queue: Promise<unknown> = Promise.resolve();
  private abort = new AbortController();
  private scenes?: Promise<Asset[]>;
  private async assets() {
    return this.scenes ??= (async () => {
      const assets: Asset[] = [];
      for (let i = 0; i < 2; i++) {
        const canvas = document.createElement('canvas');
        canvas.width = 640; canvas.height = 360;
        const ctx = canvas.getContext('2d')!;
        const gradient = ctx.createLinearGradient(0, 0, 640, 360);
        gradient.addColorStop(0, i ? '#b13c73' : '#075c87');
        gradient.addColorStop(1, i ? '#ffc965' : '#67e3c5');
        ctx.fillStyle = gradient; ctx.fillRect(0, 0, 640, 360);
        ctx.fillStyle = i ? '#f9e0a4' : '#f5f7dd';
        ctx.beginPath(); ctx.arc(i ? 440 : 200, 114, 50, 0, Math.PI * 2); ctx.fill();
        ctx.fillStyle = i ? '#542753' : '#114963';
        ctx.beginPath(); ctx.moveTo(0, 360); ctx.lineTo(168, 158);
        ctx.lineTo(340, 326); ctx.lineTo(500, 188); ctx.lineTo(640, 360); ctx.fill();
        for (let x = 32; x < 640; x += 48) {
          ctx.fillStyle = 'rgba(255,255,255,.45)'; ctx.fillRect(x, 316, 22, 6);
        }
        const blob = await new Promise<Blob>((resolve, reject) => canvas.toBlob((b) => b ? resolve(b) : reject(new Error('预览图片生成失败'))));
        if (this.abort.signal.aborted) throw new DOMException('已取消', 'AbortError');
        const id = `gallery-${i}`;
        this.urls.set(id, URL.createObjectURL(blob));
        assets.push({ id, name: '模板演示', kind: 'image', path: `/gallery/${i}.png`, size: blob.size,
          width: 640, height: 360, duration: ticks(5), hasAudio: false, sourceIdentity: `procedural-gallery-v1-${i}` });
      }
      return assets;
    })();
  }
  poster(pack: VisualPackage): Promise<string> {
    const key = `${pack.kind}:${pack.id}@${pack.version}`;
    const existing = this.posters.get(key);
    if (existing) return Promise.resolve(existing);
    const job = this.queue.then(async () => {
      if (this.abort.signal.aborted) throw new DOMException('已取消', 'AbortError');
      const assets = await this.assets(), project = createProject('模板预览');
      project.canvas = { width: 640, height: 360 };
      const first = addAsset(project, assets[0]);
      let candidate = project, time = ticks(1);
      if (pack.kind === 'effect') candidate = editTimeline(project, [{ action: 'add_effect', itemId: first.id, templateId: pack.id as EffectInstance['templateId'] }]).project;
      else {
        const next = addAsset(project, assets[1], { trackId: project.timeline.tracks[0].id });
        candidate = editTimeline(project, [{ action: 'add_transition', fromItemId: first.id, toItemId: next.id, templateId: pack.id as TransitionInstance['templateId'], durationSeconds: 1 }]).project;
        time = ticks(4.85);
      }
      const dpr = Math.min(2, window.devicePixelRatio || 1);
      const report = await this.renderer.render(candidate, time, Math.round(240 * dpr), Math.round(135 * dpr), this.abort.signal);
      if (report.backend !== 'webgpu') throw new Error('此模板需要 WebGPU');
      const result = this.canvas.toDataURL('image/png');
      this.posters.set(key, result);
      return result;
    });
    this.queue = job.catch(() => {});
    return job;
  }
  dispose() {
    this.abort.abort();
    void this.queue.finally(() => {
      this.renderer.dispose();
      for (const url of this.urls.values()) URL.revokeObjectURL(url);
      this.urls.clear(); this.posters.clear();
    });
  }
}
