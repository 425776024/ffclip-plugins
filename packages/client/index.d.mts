// Generated from implementation by scripts/build-types.mjs. Do not edit.
export class VideoCutError extends Error {
    /** @param {number} status
     * @param {import('./types.js').ApiErrorPayload} details */
    constructor(status: number, details: import("./types.js").ApiErrorPayload);
    status: number;
    details: import("./types.js").ApiErrorPayload;
    code: string | undefined;
    previewUrl: string | undefined;
    version: number | undefined;
    snapshot: import("./types.js").ApiErrorPayload;
}
export class VideoCutClient {
    /** @param {string} [url] */
    constructor(url?: string);
    url: string;
    token: string;
    /** @returns {Promise<import('./types.js').ConnectionInfo>} */
    connect(): Promise<import("./types.js").ConnectionInfo>;
    info: any;
    /** @template T
     * @param {string} path
     * @param {RequestInit} [options]
     * @returns {Promise<T>} */
    request<T>(path: string, options?: RequestInit): Promise<T>;
    /** @param {import('../core/types.js').Project} [project]
     * @returns {Promise<import('./types.js').Snapshot>} */
    createSession(project?: import("../core/types.js").Project): Promise<import("./types.js").Snapshot>;
    /** Create a fresh editable starter with HTML animation, styled titles, narration and captions.
     * @param {{ locale?: 'zh' | 'en'; name?: string }} [options]
     * @returns {Promise<import('./types.js').Snapshot>} */
    initializeDemo(options?: {
        locale?: "zh" | "en";
        name?: string;
    }): Promise<import("./types.js").Snapshot>;
    /** @param {string} id
     * @returns {Promise<import('./types.js').Snapshot>} */
    getSession(id: string): Promise<import("./types.js").Snapshot>;
    /** Find live cuts by their readable name, preview URL or saved local directory.
     * @returns {Promise<import('./types.js').SessionSummary[]>} */
    listSessions(): Promise<import("./types.js").SessionSummary[]>;
    /** @param {string} previewPath
     * @returns {Promise<import('./types.js').Snapshot>} */
    resolveSession(previewPath: string): Promise<import("./types.js").Snapshot>;
    /** @param {string} id
     * @param {import('../core/types.js').Project} project
     * @param {number} version
     * @returns {Promise<import('./types.js').Snapshot>} */
    updateSession(id: string, project: import("../core/types.js").Project, version: number): Promise<import("./types.js").Snapshot>;
    /** @param {string} id
     * @param {import('../core/commands.js').EditorCommand[]} operations
     * @param {number} version
     * @returns {Promise<import('./types.js').Snapshot & {operations: import('../core/types.js').CommandResult[]; changes?: import('../core/history.mjs').CommandChanges}>} */
    editSession(id: string, operations: import("../core/commands.js").EditorCommand[], version: number): Promise<import("./types.js").Snapshot & {
        operations: import("../core/types.js").CommandResult[];
        changes?: import("../core/history.mjs").CommandChanges;
    }>;
    /** @param {string} path
     * @returns {Promise<import('./types.js').Snapshot>} */
    openProject(path: string): Promise<import("./types.js").Snapshot>;
    /** @param {string} id
     * @returns {Promise<import('./types.js').RenderStatus>} */
    renderStatus(id: string): Promise<import("./types.js").RenderStatus>;
    /** @param {string} id
     * @returns {Promise<{closed: boolean}>} */
    closeSession(id: string): Promise<{
        closed: boolean;
    }>;
    /** @param {string} [path]
     * @returns {Promise<import('./types.js').FileListing>} */
    listFiles(path?: string): Promise<import("./types.js").FileListing>;
    /** @param {string} path
     * @returns {Promise<import('../core/types.js').Asset>} */
    importMedia(path: string): Promise<import("../core/types.js").Asset>;
    /** Read copied OS files on an explicit paste gesture and probe local media.
     * @returns {Promise<{assets: import('../core/types.js').Asset[], skipped: {name: string, error: string}[]}>} */
    importClipboardMedia(): Promise<{
        assets: import("../core/types.js").Asset[];
        skipped: {
            name: string;
            error: string;
        }[];
    }>;
    /** @returns {Promise<import('./types.js').TtsCatalog>} */
    listTtsVoices(): Promise<import("./types.js").TtsCatalog>;
    /** Fixed Base cache status; does not trigger a download.
     * @returns {Promise<import('./types.js').AsrModelStatus>} */
    asrModelStatus(): Promise<import("./types.js").AsrModelStatus>;
    /** Automatically ensure Whisper Base, then recognize local media in the open preview browser.
     * @param {string} id
     * @param {import('./types.js').AsrOptions} options
     * @returns {Promise<import('./types.js').AsrJob>} */
    transcribeSpeech(id: string, options: import("./types.js").AsrOptions): Promise<import("./types.js").AsrJob>;
    /** @param {string} id
     * @returns {Promise<import('./types.js').AsrJob>} */
    asrStatus(id: string): Promise<import("./types.js").AsrJob>;
    /** @param {string} id
     * @returns {Promise<import('./types.js').AsrJob>} */
    cancelAsr(id: string): Promise<import("./types.js").AsrJob>;
    /** Read consent and verified model status without downloading.
     * @returns {Promise<import('./types.js').VisionModelStatus>} */
    visionModelStatus(): Promise<import("./types.js").VisionModelStatus>;
    /** Ask the initialization dialog for consent; never approves or downloads a model.
     * @returns {Promise<import('./types.js').VisionModelStatus & {setupUrl: string}>} */
    requestVisionSetup(): Promise<import("./types.js").VisionModelStatus & {
        setupUrl: string;
    }>;
    /** Queue read-only image/video understanding in the open preview browser.
     * @param {string} id
     * @param {import('./types.js').VisionOptions} options
     * @returns {Promise<import('./types.js').VisionJob>} */
    analyzeMedia(id: string, options: import("./types.js").VisionOptions): Promise<import("./types.js").VisionJob>;
    /** Describe an authorized local image according to prompt. Uses the latest open preview unless id is supplied.
     * @param {string} path @param {string} prompt
     * @param {import('./types.js').VisionDescribeOptions} [options]
     * @returns {Promise<import('./types.js').VisionJob>} */
    describeImage(path: string, prompt: string, options?: import("./types.js").VisionDescribeOptions): Promise<import("./types.js").VisionJob>;
    /** Automatically sample video intervals; completed results contain timestamped text and segments.
     * @param {string} path @param {string} prompt
     * @param {import('./types.js').VisionVideoDescribeOptions} [options]
     * @returns {Promise<import('./types.js').VisionJob>} */
    describeVideo(path: string, prompt: string, options?: import("./types.js").VisionVideoDescribeOptions): Promise<import("./types.js").VisionJob>;
    /** Wait for this exact job, retaining progress with waitTimedOut on deadline. Abort cancels only this job.
     * @param {string} id @param {string} jobId
     * @param {import('./types.js').VisionDescribeOptions} [options]
     * @returns {Promise<import('./types.js').VisionJob>} */
    waitVision(id: string, jobId: string, options?: import("./types.js").VisionDescribeOptions): Promise<import("./types.js").VisionJob>;
    /** @param {string} id
     * @param {string} [jobId]
     * @returns {Promise<import('./types.js').VisionJob>} */
    visionStatus(id: string, jobId?: string): Promise<import("./types.js").VisionJob>;
    /** @param {string} id
     * @param {string} [jobId]
     * @returns {Promise<import('./types.js').VisionJob>} */
    cancelVision(id: string, jobId?: string): Promise<import("./types.js").VisionJob>;
    /** Explicitly download the pinned local model and selected voices; never runs inference.
     * @param {{dtype?:import('./types.js').TtsDtype, voices?:string[]}} [options]
     * @returns {Promise<import('./types.js').TtsInstallStatus>} */
    installTtsModel(options?: {
        dtype?: import("./types.js").TtsDtype;
        voices?: string[];
    }): Promise<import("./types.js").TtsInstallStatus>;
    /** @returns {Promise<import('./types.js').TtsInstallStatus>} */
    ttsModelStatus(): Promise<import("./types.js").TtsInstallStatus>;
    /** @returns {Promise<import('./types.js').TtsInstallStatus>} */
    cancelTtsModelInstall(): Promise<import("./types.js").TtsInstallStatus>;
    /** Queue browser inference; the preview page must stay open until completion.
     * @param {string} id
     * @param {import('./types.js').TtsSynthesizeOptions} options
     * @returns {Promise<import('./types.js').TtsJob>} */
    synthesizeSpeech(id: string, options: import("./types.js").TtsSynthesizeOptions): Promise<import("./types.js").TtsJob>;
    /** @param {string} id
     * @returns {Promise<import('./types.js').TtsJob>} */
    ttsStatus(id: string): Promise<import("./types.js").TtsJob>;
    /** @param {string} id
     * @returns {Promise<import('./types.js').TtsJob>} */
    cancelTts(id: string): Promise<import("./types.js").TtsJob>;
    /** @param {string} path
     * @param {Partial<import('../core/types.js').HtmlContent>} [options]
     * @returns {Promise<{name:string;html:import('../core/types.js').HtmlContent}>} */
    importHtml(path: string, options?: Partial<import("../core/types.js").HtmlContent>): Promise<{
        name: string;
        html: import("../core/types.js").HtmlContent;
    }>;
    /** @param {string} id
     * @param {number} version
     * @param {string} [directory]
     * @returns {Promise<{path:string;version:number}>} */
    exportProject(id: string, version: number, directory?: string): Promise<{
        path: string;
        version: number;
    }>;
    /** Save a complete, portable browser project with media and frozen motion resources.
     * @param {string} id
     * @param {number} version
     * @param {string} [directory]
     * @returns {Promise<{path:string;version:number;format:string;files:number}>} */
    saveProject(id: string, version: number, directory?: string): Promise<{
        path: string;
        version: number;
        format: string;
        files: number;
    }>;
    /** @param {string} id
     * @param {number} version
     * @param {string} [directory]
     * @param {'mp4'|'webm'} [format]
     * @returns {Promise<{path:string;version:number;format:string}>} */
    renderVideo(id: string, version: number, directory?: string, format?: "mp4" | "webm"): Promise<{
        path: string;
        version: number;
        format: string;
    }>;
    /** Reveal a completed export in the local file manager.
     * @param {string} id
     * @param {string} path
     * @returns {Promise<{opened:boolean}>} */
    revealExport(id: string, path: string): Promise<{
        opened: boolean;
    }>;
    /** @param {string} id
     * @param {string} path
     * @returns {string} */
    exportOutputUrl(id: string, path: string): string;
    /** @param {string} id
     * @param {'play'|'pause'|'seek'} action
     * @param {number} [timeSeconds]
     * @returns {Promise<any>} */
    controlPreview(id: string, action: "play" | "pause" | "seek", timeSeconds?: number): Promise<any>;
    /** @param {string} id
     * @returns {Promise<any>} */
    previewStatus(id: string): Promise<any>;
    /** @param {string} id
     * @param {Record<string,unknown>} report
     * @returns {Promise<any>} */
    reportPreview(id: string, report: Record<string, unknown>): Promise<any>;
    /** @param {string} id
     * @param {string} asset
     * @returns {string} */
    mediaUrl(id: string, asset: string): string;
    /** @param {string} id
     * @returns {string} */
    eventsUrl(id: string): string;
}
export * from "../core/project.mjs";
export type Snapshot = import("./types.js").Snapshot;
export type FileEntry = import("./types.js").FileEntry;
export type FileListing = import("./types.js").FileListing;
export type ConnectionInfo = import("./types.js").ConnectionInfo;
export type RenderStatus = import("./types.js").RenderStatus;
