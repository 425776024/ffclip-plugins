/** Retain events arriving while the preceding asynchronous task is finishing. */
export function createSerialQueue() {
  let tail = Promise.resolve();
  return (task) => {
    const result = tail.then(task);
    tail = result.catch(() => {});
    return result;
  };
}
