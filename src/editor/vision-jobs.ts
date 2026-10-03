import type { VideoCutClient } from '../../packages/client/index.mjs';
import type { VisionJob } from '../../packages/client/types';
import { VisionWorkerClient } from '../../packages/vision/client';

/** Each open editor can run agent requests; the server grants only one worker a job. */
export class VisionJobCoordinator {
  private disposed = false;
  private refreshing?: Promise<void>;
  private refreshQueued = false;
  private active?: {
    id: string;
    worker?: string;
    abort: AbortController;
    runtime?: VisionWorkerClient;
    heartbeat?: ReturnType<typeof setInterval>;
  };
  private readonly clientId = crypto.randomUUID();
  private readonly endpoint: string;
  private eventSequence = 0;
  private observed: VisionJob | null = null;
  private receive(job: VisionJob | null) {
    if (this.disposed) return;
    // A slower GET or progress callback must not roll a terminal job back to running.
    if (
      job?.id &&
      job.id === this.observed?.id &&
      ['completed', 'conflict', 'cancelled', 'error'].includes(this.observed.state) &&
      ['queued', 'running'].includes(job.state)
    )
      return;
    this.observed = job;
    this.onJob(job);
    const active = this.active;
    if (active && job?.id === active.id && ['cancelled', 'error'].includes(job.state))
      active.abort.abort();
  }
  private readonly changed = (event: Event) => {
    if (this.disposed) return;
    this.eventSequence++;
    try {
      const job = JSON.parse((event as MessageEvent).data) as VisionJob;
      this.receive(job);
    } catch {
      // A reconnect refreshes authoritative state even if an event was incomplete.
    }
    void this.refresh();
  };
  private readonly opened = () => void this.refresh();
  private readonly pageHidden = () => this.dispose();

  constructor(
    private readonly client: VideoCutClient,
    private readonly sessionId: string,
    private readonly source: EventSource,
    private readonly onJob: (job: VisionJob | null) => void
  ) {
    this.endpoint = `/sessions/${encodeURIComponent(sessionId)}/vision-job`;
    for (const event of ['vision-request', 'vision-cancel', 'vision-progress'])
      source.addEventListener(event, this.changed);
    source.addEventListener('open', this.opened);
    window.addEventListener('pagehide', this.pageHidden);
    // Requests may have arrived before this editor registered its SSE handlers.
    void this.refresh();
  }

  async refresh() {
    if (this.disposed) return;
    if (this.refreshing) {
      this.refreshQueued = true;
      return this.refreshing;
    }
    this.refreshing = (async () => {
      try {
        const sequence = this.eventSequence;
        const job = await this.client.request<VisionJob | null>(this.endpoint);
        if (this.disposed) return;
        if (sequence === this.eventSequence) {
          this.receive(job);
          if (job?.state === 'queued' && job.id && !this.active) void this.run(job);
        }
      } catch {
        // SSE reconnects and the next task event retry temporary connection errors.
      }
    })().finally(() => {
      this.refreshing = undefined;
      if (this.refreshQueued) {
        this.refreshQueued = false;
        void this.refresh();
      }
    });
    return this.refreshing;
  }

  private async run(request: VisionJob) {
    if (!request.id || this.disposed || this.active) return;
    const active = { id: request.id, abort: new AbortController() } as NonNullable<
      VisionJobCoordinator['active']
    >;
    this.active = active;
    let progressPosting = false;
    let pendingProgress: { phase: string; progress: number } | undefined;
    const post = <T>(action: string, body: Record<string, unknown>) =>
      this.client.request<T>(`${this.endpoint}/${action}`, {
        method: 'POST',
        headers: active.worker ? { 'x-vision-worker': active.worker } : {},
        body: JSON.stringify({ id: active.id, ...body })
      });
    const flushProgress = async () => {
      if (progressPosting || !pendingProgress || active.abort.signal.aborted) return;
      progressPosting = true;
      try {
        while (pendingProgress && !active.abort.signal.aborted) {
          const progress = pendingProgress;
          pendingProgress = undefined;
          await post('progress', progress);
        }
      } catch {
        // The heartbeat validates ownership; progress delivery can be retried.
      } finally {
        progressPosting = false;
      }
    };
    try {
      const claimed = await post<{ worker: string; job: VisionJob }>('claim', {
        clientId: this.clientId
      });
      active.worker = claimed.worker;
      const job = claimed.job;
      if (!active.worker) return;
      if (this.disposed) {
        await post('fail', { error: '视觉分析浏览器已关闭，请重新提交。' });
        return;
      }
      if (active.abort.signal.aborted) return;
      this.receive(job);
      active.heartbeat = setInterval(() => {
        void post('heartbeat', {}).catch((error: unknown) => {
          const status = (error as { status?: number })?.status;
          if (status === 403 || status === 409 || status === 404) active.abort.abort();
        });
      }, 5000);
      active.runtime = new VisionWorkerClient();
      const mediaUrl = new URL(
        `/api/sessions/${encodeURIComponent(this.sessionId)}/vision-media`,
        this.client.url
      );
      mediaUrl.searchParams.set('token', this.client.token);
      mediaUrl.searchParams.set('job', active.id);
      const result = await active.runtime.analyze(
        {
          mediaUrl: mediaUrl.href,
          modelBaseUrl: new URL(job.modelBaseUrl || '/vision-models/fastvlm-0.5b/', this.client.url)
            .href,
          kind: job.kind!,
          sampleTimes: job.sampleTimes!,
          prompt: job.prompt!,
          maxNewTokens: job.maxNewTokens!,
          backend: job.backend || 'auto',
          resultFormat: job.resultFormat,
          segmentPlan: job.segmentPlan
        },
        {
          signal: active.abort.signal,
          onProgress: (progress) => {
            if (this.disposed || active.abort.signal.aborted) return;
            pendingProgress = progress;
            void flushProgress();
          }
        }
      );
      if (this.disposed || active.abort.signal.aborted) return;
      const finished = await post<VisionJob>(
        'result',
        result as unknown as Record<string, unknown>
      );
      if (!this.disposed && this.observed?.id === finished.id) this.receive(finished);
    } catch (error) {
      // A competing editor's successful claim is an expected outcome.
      if (!active.worker || active.abort.signal.aborted || this.disposed) return;
      try {
        const failed = await post<VisionJob>('fail', {
          error: error instanceof Error ? error.message : String(error)
        });
        if (this.observed?.id === failed.id) this.receive(failed);
      } catch {
        void this.refresh();
      }
    } finally {
      clearInterval(active.heartbeat);
      active.runtime?.dispose();
      if (this.active === active) this.active = undefined;
      // An agent can enqueue another job while this job's upload response is arriving.
      if (!this.disposed && active.worker) void this.refresh();
    }
  }

  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    for (const event of ['vision-request', 'vision-cancel', 'vision-progress'])
      this.source.removeEventListener(event, this.changed);
    this.source.removeEventListener('open', this.opened);
    window.removeEventListener('pagehide', this.pageHidden);
    const active = this.active;
    if (!active) return;
    clearInterval(active.heartbeat);
    active.abort.abort();
    active.runtime?.dispose();
    if (active.worker)
      void this.client
        .request(`${this.endpoint}/fail`, {
          method: 'POST',
          headers: { 'x-vision-worker': active.worker },
          keepalive: true,
          body: JSON.stringify({ id: active.id, error: '视觉分析浏览器已关闭，请重新提交。' })
        })
        .catch(() => {});
  }
}
