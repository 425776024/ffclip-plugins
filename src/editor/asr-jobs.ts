import type { VideoCutClient } from '../../packages/client/index.mjs';
import type { AsrJob } from '../../packages/client/types';
import { AsrWorkerClient } from '../../packages/asr/client';

/** Each open editor can run agent requests; the server grants only one worker a job. */
export class AsrJobCoordinator {
  private disposed = false;
  private refreshing?: Promise<void>;
  private refreshQueued = false;
  private active?: {
    id: string;
    worker?: string;
    abort: AbortController;
    runtime?: AsrWorkerClient;
    heartbeat?: ReturnType<typeof setInterval>;
  };
  private readonly clientId = crypto.randomUUID();
  private readonly endpoint: string;
  private eventSequence = 0;
  private observed: AsrJob | null = null;
  private receive(job: AsrJob | null) {
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
      const job = JSON.parse((event as MessageEvent).data) as AsrJob;
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
    private readonly onJob: (job: AsrJob | null) => void
  ) {
    this.endpoint = `/sessions/${encodeURIComponent(sessionId)}/asr-job`;
    for (const event of ['asr-request', 'asr-cancel', 'asr-progress'])
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
        const job = await this.client.request<AsrJob | null>(this.endpoint);
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

  private async run(request: AsrJob) {
    if (!request.id || this.disposed || this.active) return;
    const active = { id: request.id, abort: new AbortController() } as NonNullable<
      AsrJobCoordinator['active']
    >;
    this.active = active;
    let progressPosting = false;
    let pendingProgress: { phase: string; progress: number } | undefined;
    const post = <T>(action: string, body: Record<string, unknown>) =>
      this.client.request<T>(`${this.endpoint}/${action}`, {
        method: 'POST',
        headers: active.worker ? { 'x-asr-worker': active.worker } : {},
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
      const claimed = await post<{ worker: string; job: AsrJob }>('claim', {
        clientId: this.clientId
      });
      active.worker = claimed.worker;
      const job = claimed.job;
      if (!active.worker) return;
      if (this.disposed) {
        await post('fail', { error: '识别浏览器已关闭，请重新发起。' });
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
      const prepared = await post<{ modelBaseUrl: string }>('prepare', {});
      if (this.disposed || active.abort.signal.aborted) return;
      active.runtime = new AsrWorkerClient();
      const result = await active.runtime.transcribe(
        {
          mediaUrl: this.client.mediaUrl(this.sessionId, job.assetId || ''),
          modelBaseUrl: new URL(prepared.modelBaseUrl, this.client.url).href,
          sourceBegin: job.sourceBegin ?? 0,
          sourceEnd: job.sourceEnd ?? 0,
          language: job.language ?? 'auto',
          backend: job.backend ?? 'auto'
        },
        {
          signal: active.abort.signal,
          onProgress: (progress) => {
            if (this.disposed || active.abort.signal.aborted) return;
            pendingProgress = { phase: progress.phase, progress: progress.progress };
            this.receive({
              ...job,
              state: 'running',
              ...pendingProgress,
              actualBackend: progress.backend
            });
            void flushProgress();
          }
        }
      );
      if (this.disposed || active.abort.signal.aborted) return;
      const finished = await post<AsrJob>('result', { ...result });
      if (!this.disposed && this.observed?.id === finished.id) this.receive(finished);
    } catch (error) {
      // A competing editor's successful claim is an expected outcome.
      if (!active.worker || active.abort.signal.aborted || this.disposed) return;
      try {
        const failed = await post<AsrJob>('fail', {
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
    for (const event of ['asr-request', 'asr-cancel', 'asr-progress'])
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
          headers: { 'x-asr-worker': active.worker },
          keepalive: true,
          body: JSON.stringify({ id: active.id, error: '识别浏览器已关闭，请重新发起。' })
        })
        .catch(() => {});
  }
}
