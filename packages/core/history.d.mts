// Generated from implementation by scripts/build-types.mjs. Do not edit.
import { type DocumentPatch } from './immutable.mjs';
import type { Project, CommandResult } from './types.js';
import type { EditorCommand } from './commands.js';
declare const ticket: unique symbol;
export interface CommandChanges {
    itemIds: string[];
    trackIds: string[];
    impacts: string[];
}
interface HistoryEntry {
    patches: DocumentPatch[];
    inversePatches: DocumentPatch[];
    changes: CommandChanges;
    bytes: number;
}
export interface PreparedCommand {
    project: Project;
    operations: CommandResult[];
    changes: CommandChanges;
    patches: DocumentPatch[];
    inversePatches: DocumentPatch[];
    [ticket]: {
        owner: CommandHistory;
        generation: number;
        action: string;
        entry: HistoryEntry;
    };
}
/** Session-authoritative history: byte-bounded document deltas, never project snapshots. */
export declare class CommandHistory {
    #private;
    constructor({ byteLimit }?: {
        byteLimit?: number | undefined;
    });
    get state(): {
        canUndo: boolean;
        canRedo: boolean;
        undoCount: number;
        redoCount: number;
        bytes: number;
    };
    clear(): void;
    prepare(project: Project, operations: EditorCommand[]): PreparedCommand;
    accept(candidate: PreparedCommand): void;
}
export {};
