import { editTimeline, validateProject } from './project.mjs';
import { applyDocumentPatches, type DocumentPatch } from './immutable.mjs';
import type { Project, CommandResult } from './types.js';
import type { EditorCommand } from './commands.js';

const ticket = Symbol('history-ticket');
export interface CommandChanges { itemIds: string[]; trackIds: string[]; impacts: string[] }
interface HistoryEntry {
  patches: DocumentPatch[]; inversePatches: DocumentPatch[];
  changes: CommandChanges; bytes: number;
}
export interface PreparedCommand {
  project: Project; operations: CommandResult[]; changes: CommandChanges;
  patches: DocumentPatch[]; inversePatches: DocumentPatch[];
  [ticket]: { owner: CommandHistory; generation: number; action: string; entry: HistoryEntry };
}
/** Session-authoritative history: byte-bounded document deltas, never project snapshots. */
export class CommandHistory {
  #undo: HistoryEntry[] = [];
  #redo: HistoryEntry[] = [];
  #generation = 0;
  #limit: number;
  constructor({ byteLimit = 16 * 1024 * 1024 } = {}) {
    if (!Number.isSafeInteger(byteLimit) || byteLimit < 0) throw new Error('历史预算无效');
    this.#limit = byteLimit;
  }
  get state() {
    return {
      canUndo: this.#undo.length > 0, canRedo: this.#redo.length > 0,
      undoCount: this.#undo.length, redoCount: this.#redo.length,
      bytes: [...this.#undo, ...this.#redo].reduce((sum, entry) => sum + entry.bytes, 0)
    };
  }
  clear() { this.#undo = []; this.#redo = []; this.#generation++; }
  prepare(project: Project, operations: EditorCommand[]): PreparedCommand {
    if (!Array.isArray(operations) || !operations.length) throw new Error('提供剪辑操作');
    const historical = operations.some((op) => ['undo', 'redo'].includes(op.action));
    if (historical) {
      if (operations.length !== 1) throw new Error('撤销/重做必须单独提交');
      const action = operations[0].action;
      const entry = (action === 'undo' ? this.#undo : this.#redo).at(-1);
      if (!entry) throw new Error(action === 'undo' ? '没有可撤销操作' : '没有可重做操作');
      const patches = action === 'undo' ? entry.inversePatches : entry.patches;
      const candidate = applyDocumentPatches(project, patches);
      validateProject(candidate);
      return {
        project: candidate, operations: [{ action }], changes: entry.changes, patches,
        inversePatches: action === 'undo' ? entry.patches : entry.inversePatches,
        [ticket]: { owner: this, generation: this.#generation, action, entry }
      };
    }
    const result = editTimeline(project, operations);
    const entry: HistoryEntry = { patches: result.patches, inversePatches: result.inversePatches, changes: result.changes, bytes: 0 };
    entry.bytes = new TextEncoder().encode(JSON.stringify(entry)).byteLength;
    return { ...result, [ticket]: { owner: this, generation: this.#generation, action: 'edit', entry } };
  }
  accept(candidate: PreparedCommand) {
    const t = candidate[ticket];
    if (!t || t.owner !== this || t.generation !== this.#generation) throw new Error('编辑历史版本冲突');
    if (!candidate.patches.length) return;
    if (t.action === 'undo') this.#redo.push(this.#undo.pop()!);
    else if (t.action === 'redo') this.#undo.push(this.#redo.pop()!);
    else {
      this.#redo = [];
      this.#undo.push(t.entry);
      while (this.state.bytes > this.#limit && this.#undo.length) this.#undo.shift();
    }
    this.#generation++;
  }
}
