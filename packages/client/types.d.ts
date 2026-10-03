// Generated from implementation by scripts/build-types.mjs. Do not edit.
import type { Project, Asset } from '../core/types.js';
export interface Snapshot {
    id: string;
    version: number;
    project: Project;
    history: {
        canUndo: boolean;
        canRedo: boolean;
        undoCount: number;
        redoCount: number;
        bytes: number;
    };
    previewUrl: string;
    /** Actual .vcutweb or native .vcut directory, or null until saved/opened on disk. */
    projectPath: string | null;
    /** Starter previews defer the optional vision setup dialog. */
    example?: 'starter';
}
export interface SessionSummary {
    id: string;
    name: string;
    version: number;
    previewUrl: string;
    projectPath: string | null;
}
/** Structured API failures retain recovery information for both UI and MCP. */
export interface ApiErrorPayload {
    error: string;
    code?: string;
    previewUrl?: string;
    id?: string;
    version?: number;
    project?: Project;
    history?: Snapshot['history'];
}
export interface FileEntry {
    name: string;
    path: string;
    directory: boolean;
}
export interface FileListing {
    path: string;
    parent: string | null;
    entries: FileEntry[];
}
export interface ConnectionInfo {
    token: string;
    roots: string[];
    nativeExport: boolean;
    previewControl?: boolean;
    htmlClips?: {
        available: boolean;
        runtime: string;
        gsap: string;
        alpha: boolean;
        clock: number;
    };
}
export interface SoftwareUpdateStatus {
    packageName: string;
    currentVersion: string;
    latestVersion: string | null;
    available: boolean;
    canInstall: boolean;
    state: 'idle' | 'installing' | 'installed' | 'error';
    checkedAt: string | null;
    installedVersion: string | null;
    error: string | null;
    checkError: string | null;
    phase: 'saving' | 'installing' | null;
    savedProjects: {
        name: string;
        path: string;
    }[];
}
export interface RenderStatus {
    id?: string;
    state?: string;
    status?: string;
    progress?: number;
    error?: string;
    path?: string;
    [key: string]: unknown;
}
export type TtsDtype = 'fp32';
export type TtsBackend = 'auto' | 'webgpu' | 'wasm';
export interface VisionOptions {
    version: number;
    path?: string;
    assetId?: string;
    itemId?: string;
    prompt?: string;
    beginSeconds?: number;
    endSeconds?: number;
    maxFrames?: number;
    maxNewTokens?: number;
    backend?: TtsBackend;
}
export interface VisionFrame {
    requestedSeconds: number;
    sourceSeconds: number;
    durationSeconds: number;
    timelineSeconds?: number;
    text: string;
}
export interface VisionSample {
    requestedSeconds: number;
    sourceSeconds: number;
    durationSeconds: number;
}
export interface VisionSegment {
    startSeconds: number;
    endSeconds: number;
    text: string;
    samples: VisionSample[];
    timelineStartSeconds?: number;
    timelineEndSeconds?: number;
}
export interface VisionDescribeOptions {
    /** Omit to use the most recently active open preview. */
    id?: string;
    backend?: TtsBackend;
    maxNewTokens?: number;
    /** Wait up to this duration for text; 0 returns the queued job. Default 30000, maximum 60000. */
    timeoutMs?: number;
    signal?: AbortSignal;
}
export interface VisionVideoDescribeOptions extends VisionDescribeOptions {
    beginSeconds?: number;
    endSeconds?: number;
    segmentSeconds?: number;
    maxSegments?: number;
    framesPerSegment?: number;
}
export interface VisionModelStatus {
    model: string;
    repository: string;
    revision: string;
    license: string;
    modelBaseUrl: string;
    consent: 'unasked' | 'enabled' | 'declined';
    promptRequested: boolean;
    installed: boolean;
    size: number;
    install: {
        state: 'idle' | 'downloading' | 'ready' | 'error' | 'cancelled';
        progress: number;
        error?: string;
        files: {
            path: string;
            size: number;
            downloaded: number;
        }[];
    };
}
export interface VisionJob {
    id?: string;
    sessionId?: string;
    state: 'idle' | 'queued' | 'running' | 'completed' | 'cancelled' | 'error';
    phase?: string;
    progress?: number;
    version?: number;
    model?: string;
    modelBaseUrl?: string;
    assetId?: string;
    itemId?: string;
    path?: string;
    sourceIdentity?: string;
    kind?: 'image' | 'video';
    sampleTimes?: number[];
    prompt?: string;
    maxNewTokens?: number;
    backend?: TtsBackend;
    sourceBegin?: number;
    sourceEnd?: number;
    frames?: VisionFrame[];
    resultFormat?: 'frames' | 'description' | 'segments';
    segmentPlan?: {
        startSeconds: number;
        endSeconds: number;
        sampleTimes: number[];
    }[];
    sampling?: {
        strategy: 'uniform-intervals';
        requestedSegmentSeconds: number;
        intervalSeconds: number;
        framesPerSegment: number;
        totalSamples: number;
        coarsened: boolean;
    };
    text?: string;
    segments?: VisionSegment[];
    waitTimedOut?: boolean;
    sampled?: boolean;
    actualBackend?: 'webgpu' | 'wasm';
    error?: string;
    previewUrl?: string;
}
export type AsrLanguage = 'auto' | 'zh' | 'en' | 'ja' | 'ko' | 'fr' | 'de' | 'es' | 'ru';
export interface AsrSegment {
    text: string;
    start: number;
    end: number;
}
export interface AsrOptions {
    version: number;
    itemId?: string;
    assetId?: string;
    language?: AsrLanguage;
    backend?: TtsBackend;
    insert?: boolean;
    startSeconds?: number;
}
export interface AsrModelStatus {
    model: 'whisper-base';
    revision: string;
    modelBaseUrl: string;
    installed: boolean;
    size: number;
    install: {
        state: 'idle' | 'downloading' | 'ready' | 'error' | 'cancelled';
        progress: number;
        error?: string;
    };
}
export interface AsrJob extends Partial<AsrOptions> {
    id?: string;
    state: 'idle' | 'queued' | 'running' | 'completed' | 'conflict' | 'cancelled' | 'error';
    phase?: string;
    progress?: number;
    modelBaseUrl?: string;
    sourceBegin?: number;
    sourceEnd?: number;
    rate?: number;
    duration?: number;
    text?: string;
    segments?: AsrSegment[];
    actualBackend?: 'webgpu' | 'wasm';
    itemIds?: string[];
    resultVersion?: number;
    error?: string;
    previewUrl?: string;
}
export interface TtsVoice {
    id: string;
    name: string;
    language: 'zh' | 'en';
    gender: 'female' | 'male';
    installed: boolean;
}
export interface TtsInstallStatus {
    state: 'idle' | 'downloading' | 'ready' | 'cancelled' | 'error';
    dtype?: TtsDtype;
    voices?: string[];
    progress?: number;
    files?: unknown[];
    error?: string;
}
export interface TtsCatalog {
    model: string;
    revision: string;
    modelBaseUrl: string;
    defaultVoice: string;
    voices: TtsVoice[];
    installedDtypes: TtsDtype[];
    installedVoices?: string[];
    install?: TtsInstallStatus;
}
export interface TtsSynthesizeOptions {
    text: string;
    version: number;
    voice?: string;
    speed?: number;
    backend?: TtsBackend;
    dtype?: TtsDtype;
    startSeconds?: number;
    trackId?: string;
    insert?: boolean;
}
export interface TtsJob {
    id?: string;
    state: 'idle' | 'queued' | 'running' | 'completed' | 'conflict' | 'cancelled' | 'error';
    phase?: string;
    progress?: number;
    text?: string;
    voice?: string;
    speed?: number;
    dtype?: TtsDtype;
    backend?: TtsBackend;
    actualBackend?: 'webgpu' | 'wasm';
    modelBaseUrl?: string;
    version?: number;
    startSeconds?: number;
    trackId?: string;
    insert?: boolean;
    asset?: Asset;
    path?: string;
    itemId?: string;
    resultVersion?: number;
    error?: string;
    previewUrl?: string;
}
