(window as any).runPagx = (data: unknown) =>
  new Promise((resolve) => {
    const worker = new Worker(new URL('./browser-pagx-worker.ts', import.meta.url), {
      type: 'module'
    });
    worker.onmessage = ({ data }) => {
      if (data.output) {
        let raw = '';
        for (const byte of data.output) raw += String.fromCharCode(byte);
        data.base64 = btoa(raw);
        delete data.output;
      }
      resolve(data);
      worker.terminate();
    };
    worker.onerror = (e) => {
      resolve({ error: e.message });
      worker.terminate();
    };
    worker.postMessage(data);
  });
