import { clone, editTimeline } from './project.mjs';
/** One interaction, one immutable authored base and at most one commit. */
export class EditTransaction {
  /** @type {{version: number; project: import('./types.js').Project} | null} */
  #base = null;
  /** @type {ReturnType<typeof editTimeline> | null} */
  #candidate = null;
  /** @type {import('./commands.js').EditorCommand[]} */
  #operations = [];
  get active() {
    return this.#base !== null;
  }
  get version() {
    return this.#base?.version ?? null;
  }
  get project() {
    return this.#candidate?.project ?? this.#base?.project ?? null;
  }
  /** @param {{version: number; project: import('./types.js').Project}} snapshot */
  begin(snapshot) {
    if (this.active) throw new Error('已有未结束的编辑手势');
    if (!snapshot || !Number.isSafeInteger(snapshot.version)) throw new Error('编辑版本无效');
    this.#base = { version: snapshot.version, project: snapshot.project };
    this.#candidate = null;
    this.#operations = [];
    return this.project;
  }
  /** @param {import('./commands.js').EditorCommand[]} operations */
  update(operations) {
    if (!this.#base) throw new Error('编辑草稿已失效');
    const candidate = editTimeline(this.#base.project, operations);
    // Failed updates retain the last valid candidate.
    this.#candidate = candidate;
    this.#operations = clone(operations);
    return candidate.project;
  }
  /** @param {number | {version: number}} snapshotOrVersion */
  invalidate(snapshotOrVersion) {
    const version =
      typeof snapshotOrVersion === 'number' ? snapshotOrVersion : snapshotOrVersion.version;
    if (this.#base && version !== this.#base.version) {
      this.cancel();
      return true;
    }
    return false;
  }
  commit() {
    if (!this.#base) throw new Error('编辑草稿已失效');
    const receipt = {
      version: this.#base.version,
      operations: clone(this.#operations),
      project: this.project,
      changed: Boolean(this.#candidate?.patches.length)
    };
    this.cancel();
    return receipt;
  }
  cancel() {
    const project = this.#base?.project ?? null;
    this.#base = null;
    this.#candidate = null;
    this.#operations = [];
    return project;
  }
}
