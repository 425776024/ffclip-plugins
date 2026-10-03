// Generated from implementation by scripts/build-types.mjs. Do not edit.
/**
 * @param {import('./types.js').ServerOptions} [options]
 * @returns {Promise<{url: string; initialSession: import('../client/types.js').Snapshot | undefined; close(): Promise<void>}>}
 */
export function startServer({ roots, port, staticDir, nativeBridge, htmlRenderer, initialProject, initialProjectPath, initialDemo, ffprobe, ffmpeg, ttsModelDir, asrModelDir, visionModelDir }?: import("./types.js").ServerOptions): Promise<{
    url: string;
    initialSession: import("../client/types.js").Snapshot | undefined;
    close(): Promise<void>;
}>;
export type ServerOptions = import("./types.js").ServerOptions;
