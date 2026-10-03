import { MediaEngine, type MediaFrame } from './browser';

const engine = new MediaEngine();
const requests = new Map<number, AbortController>();
const scope = globalThis as unknown as { onmessage: (event: MessageEvent) => void; postMessage: (message: unknown, transfer?: Transferable[]) => void };

// Cache-owned bitmaps must not be transferred; each RPC returns its own bitmap.
async function transferFrame(frame: MediaFrame) {
  try {
    return { frame: await createImageBitmap(frame.frame), timestamp: frame.timestamp, duration: frame.duration, width: frame.width, height: frame.height };
  } finally { frame.close(); }
}

scope.onmessage = async ({ data }) => {
  if (data.type === 'cancel') { requests.get(data.id)?.abort(); return; }
  const { id, method, args = [] } = data;
  const controller = new AbortController(); requests.set(id, controller);
  const signal = controller.signal;
  try {
    let result: unknown;
    const transfer: Transferable[] = [];
    if (method === 'probe') result = await engine.probe(args[0], { ...args[1], signal });
    else if (method === 'waveform') result = await engine.waveform(args[0], args[1], args[2], args[3], { signal });
    else if (method === 'videoFrame' || method === 'image') {
      const original = method === 'image' ? await engine.image(args[0], { ...args[1], signal }) : await engine.videoFrame(args[0], args[1], { ...args[2], signal });
      const value = await transferFrame(original); result = value; transfer.push(value.frame);
    } else if (method === 'thumbnails') {
      const originals = await engine.thumbnails(args[0], args[1], { ...args[2], signal });
      const values = [];
      try {
        for (const original of originals) values.push(await transferFrame(original));
      } catch (error) {
        for (const original of originals) original.close();
        for (const value of values) value.frame.close();
        throw error;
      }
      result = values; transfer.push(...values.map((value) => value.frame));
    } else if (method === 'stats') result = engine.stats();
    else if (method === 'persistentUsage') result = await engine.persistentUsage();
    else if (method === 'invalidate') { engine.invalidate(args[0]); result = null; }
    else throw new Error('Unknown media worker method');
    if (signal.aborted) {
      for (const resource of transfer) if (resource instanceof ImageBitmap) resource.close();
      return;
    }
    scope.postMessage({ id, result }, transfer);
  } catch (error) {
    if (!signal.aborted) scope.postMessage({ id, error: { name: error instanceof Error ? error.name : 'Error', message: error instanceof Error ? error.message : String(error) } });
  } finally { requests.delete(id); }
};
