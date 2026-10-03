import type { VideoCutClient } from '../../packages/client/index.mjs';
import type { ExportOptions } from '../../packages/render/export';
import type { Project } from '../../packages/core/project.mjs';
interface ClaimedExportJob {
  id: string;
  worker: string;
  project: Project;
  format: 'mp4' | 'webm';
  localEncoder: boolean;
}

/** The frozen project is composited in a worker; encoding uses detected capabilities. */
export async function renderTemplateExport(
  client: VideoCutClient,
  sessionId: string,
  jobId: string,
  signal: AbortSignal,
  onClaim: () => void,
  options: ExportOptions = {}
) {
  const endpoint = `/sessions/${sessionId}/render-job?job=${encodeURIComponent(jobId)}`;
  let claimed = false,
    worker: Worker | undefined,
    workerToken = '',
    sequence = 0,
    audioSequence = 0;
  const headers = () => ({ 'X-Render-Worker': workerToken });
  const cancel = () => {
    worker?.postMessage({ type: 'cancel' });
    void client
      .request(`${endpoint}&action=cancel`, { method: 'POST', headers: headers() })
      .catch(() => {});
  };
  try {
    const job = await client.request<ClaimedExportJob>(`${endpoint}&action=claim`, {
      method: 'POST',
      signal
    });
    claimed = true;
    workerToken = job.worker;
    onClaim();
    signal.addEventListener('abort', cancel, { once: true });
    if (signal.aborted) throw new DOMException('用户取消导出', 'AbortError');
    const urls = Object.fromEntries(
      job.project.assets.map(
        (a: {
          id: string;
          path: string;
          size: number;
          fingerprint?: unknown;
          mtimeMs?: number;
          sourceIdentity?: string;
        }) => {
          const url = new URL(client.mediaUrl(sessionId, a.id), location.href);
          url.searchParams.set('job', job.id);
          url.searchParams.set(
            'source',
            a.sourceIdentity ||
              JSON.stringify([a.id, a.path, a.size, a.fingerprint ?? null, a.mtimeMs ?? null])
          );
          return [a.id, url.href];
        }
      )
    );
    worker = new Worker(new URL('../../packages/render/worker.ts', import.meta.url), {
      type: 'module'
    });
    const result = await new Promise<{ size: number; frames: number; audioFrames: number }>(
      (resolve, reject) => {
        const current = worker!;
        const aborted = () => reject(new DOMException('用户取消导出', 'AbortError'));
        signal.addEventListener('abort', aborted, { once: true });
        const done = (
          error?: Error,
          result?: { size: number; frames: number; audioFrames: number }
        ) => {
          signal.removeEventListener('abort', aborted);
          error ? reject(error) : resolve(result!);
        };
        current.onerror = (event) => done(new Error(event.message || '导出工作线程失败'));
        current.onmessage = async ({ data }) => {
          try {
            if (data.type === 'configure') {
              await client.request(`${endpoint}&action=configure`, {
                method: 'POST',
                signal,
                headers: headers(),
                body: JSON.stringify(data.value)
              });
              current.postMessage({ type: 'ack', id: data.id });
            } else if (data.type === 'chunk') {
              await client.request(
                `${endpoint}&action=chunk&position=${data.position}&sequence=${sequence++}`,
                {
                  method: 'POST',
                  signal,
                  headers: { ...headers(), 'Content-Type': 'application/octet-stream' },
                  body: data.data
                }
              );
              current.postMessage({ type: 'ack', id: data.id });
            } else if (data.type === 'audio') {
              await client.request(
                `${endpoint}&action=audio&sampleOffset=${data.sampleOffset}&sampleRate=${data.sampleRate}&channels=${data.channels}&sequence=${audioSequence++}`,
                {
                  method: 'POST',
                  signal,
                  headers: { ...headers(), 'Content-Type': 'application/octet-stream' },
                  body: data.data
                }
              );
              current.postMessage({ type: 'ack', id: data.id });
            } else if (data.type === 'frame') {
              await client.request(`${endpoint}&action=frame&index=${data.index}`, {
                method: 'POST',
                signal,
                headers: { ...headers(), 'Content-Type': 'application/octet-stream' },
                body: data.data
              });
              current.postMessage({ type: 'ack', id: data.id });
            } else if (data.type === 'progress') {
              await client.request(`${endpoint}&action=progress`, {
                method: 'POST',
                signal,
                headers: headers(),
                body: JSON.stringify(data.value)
              });
            } else if (data.type === 'complete') done(undefined, data.result);
            else if (data.type === 'error' || data.type === 'cancelled')
              done(new Error(data.error));
          } catch (error) {
            current.postMessage({
              type: 'ack',
              id: data.id,
              error: error instanceof Error ? error.message : String(error)
            });
            done(error instanceof Error ? error : new Error(String(error)));
          }
        };
        current.postMessage({
          type: 'export',
          project: job.project,
          format: job.format || 'mp4',
          urls,
          localEncoder: job.localEncoder,
          preferredEncoding: options.preferredEncoding,
          htmlPrefetch: options.htmlPrefetch,
          textWorkers: options.textWorkers
        });
      }
    );
    await client.request(`${endpoint}&action=finish`, {
      method: 'POST',
      signal,
      headers: headers(),
      body: JSON.stringify(result)
    });
    return true;
  } catch (error: any) {
    if (!claimed && [404, 409].includes(error.status)) return false;
    if (claimed)
      await client
        .request(`${endpoint}&action=${signal.aborted ? 'cancel' : 'error'}`, {
          method: 'POST',
          headers: headers(),
          body: JSON.stringify({ error: error.message })
        })
        .catch(() => {});
    throw error;
  } finally {
    signal.removeEventListener('abort', cancel);
    worker?.terminate();
  }
}
