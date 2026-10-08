import { VideoCutClient } from '../packages/client/index.mjs';
import { renderTemplateExport } from '../src/editor/template-export';

async function endToEnd(input: any) {
  const client = new VideoCutClient(location.origin);
  await client.connect();
  const events = new EventSource(client.eventsUrl(input.session.id));
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 120000);
  try {
    await new Promise<void>((resolve, reject) => {
      events.onopen = () => resolve();
      events.onerror = () => reject(new Error('Export event stream failed'));
      controller.signal.addEventListener('abort', () => reject(new Error('Export timed out')), {
        once: true
      });
    });
    const started = performance.now();
    const pending = client.renderVideo(
      input.session.id,
      input.session.version,
      input.directory,
      'mp4'
    );
    pending.catch(() => {});
    let jobId = '';
    for (let attempt = 0; attempt < 400; attempt++) {
      controller.signal.throwIfAborted();
      const job = await client.renderStatus(input.session.id);
      if (job.id || job.jobId) {
        jobId = String(job.id || job.jobId);
        break;
      }
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
    if (!jobId) throw new Error('Export job did not become available');
    const claimed = await renderTemplateExport(
      client,
      input.session.id,
      jobId,
      controller.signal,
      () => {},
      { preferredEncoding: 'browser' }
    );
    if (!claimed) throw new Error('Export worker did not claim the job');
    const receipt = await pending;
    return { elapsedMs: performance.now() - started, receipt };
  } finally {
    clearTimeout(timeout);
    events.close();
  }
}

// One fresh worker per trial keeps renderer caches independent.
(window as any).runBenchmark = (input: any) =>
  input.mode === 'endtoend'
    ? endToEnd(input).catch((error) => ({ error: error.message }))
    : new Promise((resolve) => {
        const worker = new Worker(new URL('./browser-pagx-benchmark-worker.ts', import.meta.url), {
          type: 'module'
        });
        worker.onmessage = ({ data }) => {
          if (data.progress) {
            (window as any).benchmarkProgress = data;
            return;
          }
          for (const file of data.files || []) {
            let binary = '';
            for (const byte of file.bytes) binary += String.fromCharCode(byte);
            file.base64 = btoa(binary);
            delete file.bytes;
          }
          resolve(data);
          worker.terminate();
        };
        worker.onerror = (e) => {
          resolve({ error: e.message });
          worker.terminate();
        };
        worker.postMessage(input);
      });
