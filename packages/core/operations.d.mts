// Generated from implementation by scripts/build-types.mjs. Do not edit.
export function editTimeline(project: import("./types.js").Project, operations: import("./commands.js").EditorCommand[]): {
    project: import("./types.js").Project;
    operations: import("./types.js").CommandResult[];
    changes: {
        itemIds: string[];
        trackIds: string[];
        impacts: string[];
    };
    patches: import("./immutable.mjs").DocumentPatch[];
    inversePatches: import("./immutable.mjs").DocumentPatch[];
};
