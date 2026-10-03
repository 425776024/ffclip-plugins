export class RenderBudgetError extends Error {
  readonly code = 'RENDER_BUDGET';
  constructor(
    readonly requiredBytes: number,
    readonly budgetBytes: number
  ) {
    super(
      `当前画面需要至少 ${(requiredBytes / 1048576).toFixed(1)} MiB GPU 工作集，预算为 ${(budgetBytes / 1048576).toFixed(1)} MiB。`
    );
    this.name = 'RenderBudgetError';
  }
}

/** Rebuild priority, then LRU. Current command-buffer dependencies cannot be evicted. */
let accessClock = 0;
export class RenderResourceCache<T> {
  private entries = new Map<
    string,
    { value: T; bytes: number; access: number; priority: number }
  >();
  private pins = new Set<string>();
  bytes = 0;
  evictions = 0;
  peakBytes = 0;
  constructor(
    public budgetBytes: number,
    private release: (value: T, key: string) => void
  ) {}
  beginFrame() {
    this.pins.clear();
    this.evictions = 0;
  }
  get(key: string, pin = true): T | undefined {
    const entry = this.entries.get(key);
    if (!entry) return;
    this.entries.delete(key);
    this.entries.set(key, entry);
    entry.access = ++accessClock;
    if (pin) this.pins.add(key);
    return entry.value;
  }
  has(key: string) {
    return this.entries.has(key);
  }
  delete(key: string) {
    const entry = this.entries.get(key);
    if (!entry) return;
    this.entries.delete(key);
    this.pins.delete(key);
    this.bytes -= entry.bytes;
    this.release(entry.value, key);
  }
  reserve(bytes: number) {
    while (this.bytes + bytes > this.budgetBytes) {
      const candidate = this.oldestEvictable();
      if (!candidate) break;
      this.evict(candidate.key);
    }
    if (this.bytes + bytes > this.budgetBytes)
      throw new RenderBudgetError(this.bytes + bytes, this.budgetBytes);
  }
  set(key: string, value: T, bytes: number) {
    this.delete(key);
    this.reserve(bytes);
    this.entries.set(key, { value, bytes, access: ++accessClock, priority: 0 });
    this.pins.add(key);
    this.bytes += bytes;
    this.peakBytes = Math.max(this.peakBytes, this.bytes);
    return value;
  }
  setBudget(bytes: number) {
    if (!Number.isSafeInteger(bytes) || bytes < 1024)
      throw new RangeError('Invalid render resource budget');
    this.budgetBytes = bytes;
    this.reserve(0);
  }
  get workingSetBytes() {
    let bytes = 0;
    for (const key of this.pins) bytes += this.entries.get(key)?.bytes || 0;
    return bytes;
  }
  releasePins() {
    this.pins.clear();
  }
  setPriority(key: string, priority: number) {
    if (!Number.isFinite(priority) || priority < 0)
      throw new RangeError('Invalid resource priority');
    const entry = this.entries.get(key);
    if (entry) entry.priority = priority;
  }
  resetPriorities() {
    for (const entry of this.entries.values()) entry.priority = 0;
  }
  oldestEvictable() {
    let candidate: { key: string; access: number; priority: number } | undefined;
    for (const [key, entry] of this.entries) {
      if (this.pins.has(key)) continue;
      if (!candidate || entry.priority < candidate.priority)
        candidate = { key, access: entry.access, priority: entry.priority };
      // Map order is LRU; no nonnegative priority can precede the first zero.
      if (candidate.priority === 0) break;
    }
    return candidate;
  }
  evict(key: string) {
    this.delete(key);
    this.evictions++;
  }
  clear() {
    for (const key of [...this.entries.keys()]) this.delete(key);
  }
}

/** One device/context budget includes scene resources and borrowed text-compositor allocations. */
export class RenderMemoryPool {
  peakBytes = 0;
  private owners = new Map<RenderResourceCache<any>, number>();
  private external = new Map<object, number>();
  get budgetBytes() {
    return Math.min(512 * 1048576, ...this.owners.values());
  }
  get externalBytes() {
    let n = 0;
    for (const bytes of this.external.values()) n += bytes;
    return n;
  }
  get bytes() {
    let n = this.externalBytes;
    for (const owner of this.owners.keys()) n += owner.bytes;
    return n;
  }
  register(cache: RenderResourceCache<any>, budget: number) {
    this.owners.set(cache, budget);
  }
  unregister(cache: RenderResourceCache<any>) {
    this.owners.delete(cache);
  }
  setBudget(cache: RenderResourceCache<any>, bytes: number) {
    this.owners.set(cache, bytes);
    this.reserve(0);
  }
  reserve(bytes: number) {
    while (this.bytes + bytes > this.budgetBytes) {
      let candidate:
        | { owner: RenderResourceCache<any>; key: string; access: number; priority: number }
        | undefined;
      for (const owner of this.owners.keys()) {
        const oldest = owner.oldestEvictable();
        if (
          oldest &&
          (!candidate ||
            oldest.priority < candidate.priority ||
            (oldest.priority === candidate.priority && oldest.access < candidate.access))
        )
          candidate = { owner, ...oldest };
      }
      if (!candidate) throw new RenderBudgetError(this.bytes + bytes, this.budgetBytes);
      candidate.owner.evict(candidate.key);
    }
  }
  retainExternal(token: object, bytes: number) {
    this.reserve(bytes);
    this.external.set(token, bytes);
    this.recordUsage();
  }
  releaseExternal(token: object) {
    this.external.delete(token);
  }
  recordUsage() {
    this.peakBytes = Math.max(this.peakBytes, this.bytes);
  }
}
