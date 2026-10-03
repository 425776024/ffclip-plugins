// Generated from implementation by scripts/build-types.mjs. Do not edit.
/** One interaction, one immutable authored base and at most one commit. */
export class EditTransaction {
    get active(): boolean;
    get version(): number | null;
    get project(): import("./types.js").Project | null;
    /** @param {{version: number; project: import('./types.js').Project}} snapshot */
    begin(snapshot: {
        version: number;
        project: import("./types.js").Project;
    }): import("./types.js").Project | null;
    /** @param {import('./commands.js').EditorCommand[]} operations */
    update(operations: import("./commands.js").EditorCommand[]): import("./types.js").Project;
    /** @param {number | {version: number}} snapshotOrVersion */
    invalidate(snapshotOrVersion: number | {
        version: number;
    }): boolean;
    commit(): {
        version: number;
        operations: import("./commands.js").EditorCommand[];
        project: import("./types.js").Project | null;
        changed: boolean;
    };
    cancel(): import("./types.js").Project | null;
    #private;
}
