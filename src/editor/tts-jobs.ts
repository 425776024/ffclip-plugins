import type { VideoCutClient } from '../../packages/client/index.mjs';
import type { TtsJob } from '../../packages/client/types';
import { TtsWorkerClient } from '../../packages/tts/client';

/** Each open editor can run agent requests; the server grants only one worker a job. */
export class TtsJobCoordinator {
  private disposed = false;
  private refreshing?: Promise<void>;
  private refreshQueued = false;
  private active?: {
    id: string;
    worker?: string;
    abort: AbortController;
    runtime?: TtsWorkerClient;
    heartbeat?: ReturnType<typeof setInterval>;
  };
  private readonly clientId = crypto.randomUUID();
  private readonly endpoint: string;
  private eventSequence = 0;
  private observed: TtsJob | null = null;
  private receive(job: TtsJob | null) {
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
      const job = JSON.parse((event as MessageEvent).data) as TtsJob;
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
    sessionId: string,
    private readonly source: EventSource,
    private readonly onJob: (job: TtsJob | null) => void
  ) {
    this.endpoint = `/sessions/${encodeURIComponent(sessionId)}/tts-job`;
    for (const event of ['tts-request', 'tts-cancel', 'tts-progress'])
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
        const job = await this.client.request<TtsJob | null>(this.endpoint);
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

  private async run(request: TtsJob) {
    if (!request.id || this.disposed || this.active) return;
    const active = { id: request.id, abort: new AbortController() } as NonNullable<
      TtsJobCoordinator['active']
    >;
    this.active = active;
    let progressPosting = false;
    let pendingProgress: { phase: string; progress: number } | undefined;
    const post = <T>(action: string, body: Record<string, unknown>) =>
      this.client.request<T>(`${this.endpoint}/${action}`, {
        method: 'POST',
        headers: active.worker ? { 'x-tts-worker': active.worker } : {},
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
      const claimed = await post<{ worker: string; job: TtsJob }>('claim', {
        clientId: this.clientId
      });
      active.worker = claimed.worker;
      const job = claimed.job;
      if (!active.worker) return;
      if (this.disposed) {
        await post('fail', { error: '合成浏览器已关闭，请重新生成。' });
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
      active.runtime = new TtsWorkerClient();
      const result = await active.runtime.synthesize(
        {
          text: job.text || '',
          voice: job.voice || 'zf_001',
          speed: job.speed ?? 1,
          backend: job.backend || 'auto',
          dtype: job.dtype || 'fp32',
          modelBaseUrl: new URL(job.modelBaseUrl || '/tts-models/kokoro-v1.1-zh/', this.client.url)
            .href
        },
        {
          signal: active.abort.signal,
          onProgress: (progress) => {
            if (this.disposed || active.abort.signal.aborted) return;
            pendingProgress = { phase: progress.phase, progress: progress.progress };
            this.receive({ ...job, state: 'running', ...pendingProgress });
            void flushProgress();
          }
        }
      );
      if (this.disposed || active.abort.signal.aborted) return;
      const finished = await this.client.request<TtsJob>(
        `${this.endpoint}/result?job=${encodeURIComponent(active.id)}`,
        {
          method: 'POST',
          headers: {
            'Content-Type': 'audio/wav',
            'x-tts-worker': active.worker,
            'x-tts-backend': result.backend
          },
          body: result.wav
        }
      );
      if (!this.disposed && this.observed?.id === finished.id) this.receive(finished);
    } catch (error) {
      // A competing editor's successful claim is an expected outcome.
      if (!active.worker || active.abort.signal.aborted || this.disposed) return;
      try {
        const failed = await post<TtsJob>('fail', {
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
    for (const event of ['tts-request', 'tts-cancel', 'tts-progress'])
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
          headers: { 'x-tts-worker': active.worker },
          keepalive: true,
          body: JSON.stringify({ id: active.id, error: '合成浏览器已关闭，请重新生成。' })
        })
        .catch(() => {});
  }
}
