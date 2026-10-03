// Generated from implementation by scripts/build-types.mjs. Do not edit.
export type DocumentPath = Array<string | {
    id: string;
}>;
export type DocumentPatch = {
    op: 'remove';
    path: DocumentPath;
} | {
    op: 'set';
    path: DocumentPath;
    value: unknown;
} | {
    op: 'order';
    path: DocumentPath;
    value: string[];
};
/** Copy-on-write draft for the JSON document. Unchanged branches retain identity. */
export declare function createProjectDraft<T extends object>(base: T): {
    draft: T;
    finish: () => T;
};
/** Stable IDs avoid copying all following clips when a collection is inserted/reordered. */
export declare function documentPatches(before: any, after: any, path?: DocumentPath, output?: DocumentPatch[]): DocumentPatch[];
export declare function applyDocumentPatches<T extends object>(project: T, patches: readonly DocumentPatch[]): T;
