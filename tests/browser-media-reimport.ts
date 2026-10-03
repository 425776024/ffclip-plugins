import { VideoCutClient } from '../packages/client/index.mjs';
import { createProject, addAsset } from '../packages/core/project.mjs';
import { MediaEngine } from '../packages/media/browser';

const query = new URLSearchParams(location.search);
const root =
  query.get('fixtures') ||
  '/Users/jxinfa/WebstormProjects/videocut/.local/architecture-qa/render-results';
const status = document.querySelector('#status')!;
const check = (condition: unknown, message: string) => {
  if (!condition) throw Error(message);
};
async function run() {
  const report: any = {
    kind: 'media_reimport',
    phase: 'importing',
    started: new Date().toISOString(),
    results: []
  };
  const publish = async () => {
    status.textContent = JSON.stringify(report, null, 2);
    await fetch('http://127.0.0.1:4332/report', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-QA-Report': 'render' },
      body: JSON.stringify(report)
    }).catch(() => {});
  };
  const client = new VideoCutClient(location.origin),
    engine = new MediaEngine();
  let session = '';
  try {
    await publish();
    await client.connect();
    const reference = new Float32Array(
      await (await fetch(`/@fs${root}/aac-sync-reference.f32`)).arrayBuffer()
    );
    const names = ['aac-sync-corrected.mp4', 'aac-sync-corrected.webm'];
    if (query.has('external')) names.push('opus-external.webm', 'opus-external.ogg');
    const assets = await Promise.all(
      names.map((name) =>
        client.importMedia(`${root}/${name}`).then((value) => value.asset || value)
      )
    );
    const project = createProject('Media reimport acceptance');
    assets.forEach((asset) => addAsset(project, asset));
    session = (await client.createSession(project)).id;
    for (const asset of assets) {
      report.phase = asset.name;
      await publish();
      const url = client.mediaUrl(session, asset.id),
        metadata = await engine.probe(url);
      check(
        metadata.sourceOrigin === 0 && asset.firstTimestamp === 0,
        'Negative codec preroll was normalized into a source offset'
      );
      check(
        Math.abs(metadata.duration - 3) <= 0.0011 && Math.abs(asset.duration - 360000) <= 121,
        'Reimport changed the three-second presentation duration beyond the container time quantum'
      );
      const blocks = await engine.audioWindow(url, 0, 3);
      const actual = new Float32Array(144000);
      for (const block of blocks)
        actual.set(
          block.data[0].subarray(
            0,
            Math.max(0, actual.length - Math.round(block.timestamp * 48000))
          ),
          Math.min(actual.length, Math.round(block.timestamp * 48000))
        );
      const markers = [];
      for (const sample of [0, 12000, 60000, 108000, 142000]) {
        let best = -Infinity,
          lagSamples = 0;
        for (let lag = -2400; lag <= 2400; lag++) {
          let product = 0,
            a2 = 0,
            b2 = 0;
          for (let i = 0; i < 1024 && sample + i < reference.length; i++) {
            const index = sample + i + lag;
            const a = reference[sample + i],
              // Keep the reference energy over the entire marker window. A lag
              // with one overlapping sample must not win with correlation 1.
              b = index < 0 || index >= actual.length ? 0 : actual[index];
            product += a * b;
            a2 += a * a;
            b2 += b * b;
          }
          const correlation = product / Math.sqrt(a2 * b2 || 1);
          if (correlation > best) {
            best = correlation;
            lagSamples = lag;
          }
        }
        markers.push({ sample, lagSamples, correlation: best });
      }
      const value = {
        path: asset.path,
        metadata,
        assetFirstTimestamp: asset.firstTimestamp,
        assetDuration: asset.duration,
        blockFrames: blocks.map((b) => b.numberOfFrames),
        markers
      };
      report.results.push(value);
      await publish();
      check(
        markers.every((marker) => Math.abs(marker.lagSamples) <= 48 && marker.correlation > 0.9),
        'Decoded browser reimport has >1 ms timing drift or inaccurate PCM markers'
      );
    }
    report.phase = 'complete';
  } catch (error) {
    report.phase = 'failed';
    report.error = error instanceof Error ? error.stack : String(error);
  } finally {
    if (session) await client.closeSession(session).catch(() => {});
    engine.dispose();
    await publish();
  }
}
document.querySelector('#run')!.addEventListener('click', () => void run());
if (query.has('autorun')) void run();
