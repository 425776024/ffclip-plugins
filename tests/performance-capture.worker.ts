// The baseline artifact remains byte-for-byte frozen. This wrapper only captures
// its owned OffscreenCanvas after the unchanged worker has acknowledged a frame.
const scope = self as any;
const devices = new Set<any>();
const requestAdapter = navigator.gpu.requestAdapter.bind(navigator.gpu);
(navigator.gpu as any).requestAdapter = async (...args: any[]) => {
  const adapter = await requestAdapter(...args);
  if (adapter) {
    const requestDevice = adapter.requestDevice.bind(adapter);
    (adapter as any).requestDevice = async (...options: any[]) => {
      const device = await requestDevice(...options);
      devices.add(device);
      return device;
    };
  }
  return adapter;
};
let canvas: OffscreenCanvas | undefined;
const queued: MessageEvent[] = [];
scope.addEventListener('message', (event: MessageEvent) => {
  if (event.data.type === 'init') canvas = event.data.canvas;
});
scope.onmessage = (event: MessageEvent) => queued.push(event);
const backend = new URL(scope.location.href).searchParams.get('backend');
if (backend === 'baseline')
  await import('/@fs/Users/jxinfa/WebstormProjects/videocut/.local/architecture-baseline/package/dist/web/assets/worker-C3JgJ-tK.js');
else await import('../packages/render/worker.ts');
const delegate = scope.onmessage;
scope.onmessage = async (event: MessageEvent) => {
  const data = event.data;
  if (data.type === 'dispose') {
    try {
      await delegate.call(scope, event);
      await Promise.all(
        [...devices].map((device) => device.queue.onSubmittedWorkDone().catch(() => {}))
      );
      for (const device of devices) device.destroy();
      devices.clear();
      canvas = undefined;
      scope.postMessage({ type: 'disposed', id: data.id });
    } catch (error) {
      scope.postMessage({ type: 'error', id: data.id, error: String(error) });
    }
    return;
  }
  if (data.type !== 'capture') {
    if (data.type === 'init') canvas = data.canvas;
    await delegate.call(scope, event);
    return;
  }
  try {
    if (!canvas) throw new Error('Initialize the canvas before capture');
    const blob = await canvas.convertToBlob({ type: 'image/png' }),
      bytes = await blob.arrayBuffer();
    scope.postMessage(
      { type: 'captured', id: data.id, width: canvas.width, height: canvas.height, bytes },
      [bytes]
    );
  } catch (error) {
    scope.postMessage({ type: 'error', id: data.id, error: String(error) });
  }
};
for (const event of queued) void scope.onmessage(event);
