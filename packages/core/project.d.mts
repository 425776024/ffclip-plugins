// Generated from implementation by scripts/build-types.mjs. Do not edit.
export function assertTextExportSupported(project: import("./types.js").Project): void;
export function createProject(name?: string): import("./types.js").Project;
export function duration(project: import("./types.js").Project): number;
export function addAsset(project: import("./types.js").Project, asset: import("./types.js").Asset, options?: {
    validate?: boolean;
    start?: number;
    trackId?: string;
}): import("./types.js").Item;
export function addText(project: import("./types.js").Project, options?: {
    validate?: boolean;
    content?: string;
    start?: number;
    length?: number;
    fontSize?: number;
    color?: string;
    trackId?: string;
    template?: import("./types.js").TextContent["template"];
}): import("./types.js").Item;
export function addHtmlClip(project: import("./types.js").Project, options: {
    html: import("./types.js").HtmlContent;
    name?: string;
    start?: number;
    length?: number;
    trackId?: string;
    validate?: boolean;
}): import("./types.js").Item;
export function findItem(project: import("./types.js").Project, id: string): {
    track: import("./types.js").Track;
    item: import("./types.js").Item;
};
export function splitItem(project: import("./types.js").Project, id: string, at: number): import("./types.js").Item;
export function removeItem(project: import("./types.js").Project, id: string): void;
export function moveItem(project: import("./types.js").Project, id: string, begin: number, trackId?: string): void;
export function trimItem(project: import("./types.js").Project, id: string, begin: number, end: number): void;
export function validateProject(project: unknown): import("./types.js").Project;
export const TEXT_TEMPLATES: {
    id: any;
    name: any;
    tag: string;
    text: any;
    timeUs: number;
    category: string;
}[];
export const TEMPLATE_EXPORT_NOTICE: "MP4 \u6309\u4F5C\u54C1\u5206\u8FA8\u7387\u9010\u5E27\u6E32\u67D3\uFF1B\u5BFC\u51FA\u671F\u95F4\u8BF7\u4FDD\u6301\u9875\u9762\u6253\u5F00\u3002";
/** VideoCut authored editing model. Time values use the native 120,000 Hz clock. */
export const TIME_BASE: 120000;
export const FORMAT: "videocut.edit-session";
/** @type {(seconds: number) => number} */
export const ticks: (seconds: number) => number;
/** @type {(value: number) => number} */
export const seconds: (value: number) => number;
/** @type {(prefix: string) => string} */
export const identity: (prefix: string) => string;
/** @type {<T>(value: T) => T} */
export const clone: <T>(value: T) => T;
export { CommandHistory } from "./history.mjs";
export * from "./model.mjs";
export { editTimeline } from "./operations.mjs";
export { EditTransaction } from "./transaction.mjs";
export type AssetKind = import("./types.js").AssetKind;
export type Asset = import("./types.js").Asset;
export type Item = import("./types.js").Item;
export type Track = import("./types.js").Track;
export type Project = import("./types.js").Project;
export type TextContent = import("./types.js").TextContent;
export type TextTemplate = import("./types.js").TextTemplate;
export type VisualProperties = import("./types.js").VisualProperties;
export type AudioProperties = import("./types.js").AudioProperties;
export type Keyframe = import("./types.js").Keyframe;
export type AutomationBinding = import("./types.js").AutomationBinding;
export type EffectInstance = import("./types.js").EffectInstance;
export type TransitionInstance = import("./types.js").TransitionInstance;
export type ItemRelation = import("./types.js").ItemRelation;
export type PropertyDescriptor = import("./types.js").PropertyDescriptor;
export type EffectTemplate = import("./types.js").EffectTemplate;
export type CommandResult = import("./types.js").CommandResult;
export type FrameLayer = import("./types.js").FrameLayer;
export type EditorCommand = import("./commands.js").EditorCommand;
export { TEMPLATE_PARTS, TEMPLATE_COMPOSITION_RULES } from "../text-wasm/src/recipes.mjs";
export { createProjectDraft, documentPatches, applyDocumentPatches } from "./immutable.mjs";
