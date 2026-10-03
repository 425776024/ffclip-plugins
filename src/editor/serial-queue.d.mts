export function createSerialQueue(): <T>(task: () => Promise<T>) => Promise<T>;
