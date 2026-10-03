// Generated from implementation by scripts/build-types.mjs. Do not edit.
export function readPath(object: unknown, path: string): unknown;
export function writePath(object: object, path: string, value: unknown): void;
export function getPropertyDescriptor(item: import("./types.js").Item, property: string): import("./types.js").PropertyDescriptor | undefined;
export function isPropertyApplicable(project: import("./types.js").Project, item: import("./types.js").Item, property: string): boolean;
export function selectionClosure(project: import("./types.js").Project, itemIds: string[], options?: {
    expand?: boolean;
    groups?: boolean;
    links?: boolean;
}): string[];
export function validateProperty(property: string, value: unknown, clip?: import("./types.js").Item["clip"]): unknown;
/** JSON Schema is derived from the same range/type contract used by the Inspector. */
/** @param {import('./types.js').PropertyDescriptor} descriptor */
export function descriptorInputSchema(descriptor: import("./types.js").PropertyDescriptor, { seconds: useSeconds }?: {
    seconds?: boolean | undefined;
}): {
    description: string;
    pattern?: string | undefined;
    maxLength?: number | undefined;
    minimum?: number | undefined;
    maximum?: number | undefined;
    enum?: string[] | undefined;
    type: string;
};
export function propertyInputSchema(property: any, options?: {}): {
    description: string;
    pattern?: string | undefined;
    maxLength?: number | undefined;
    minimum?: number | undefined;
    maximum?: number | undefined;
    enum?: string[] | undefined;
    type: string;
};
export function defaultParameters(template: import("./types.js").EffectTemplate): Record<string, number | boolean | string>;
export function enrichClip(clip: import("./types.js").Item["clip"]): import("./types.js").Item["clip"];
export function validateHtmlContent(content: import("./types.js").HtmlContent): import("./types.js").HtmlContent;
export function validateExtensions(project: import("./types.js").Project, seen: Set<string>): void;
export function mapTimelineToSource(item: import("./types.js").Item, time: number, options?: {
    clamp?: boolean;
}): number;
export function trimSourceRange(item: import("./types.js").Item, begin: number, end: number): {
    begin: number;
    end: number;
};
export function mapSourceToTimeline(item: import("./types.js").Item, time: number, options?: {
    clamp?: boolean;
}): number;
export function quantizeTime(time: number, frameRate: import("./types.js").Project["frameRate"]): number;
export function sampleProperty(item: import("./types.js").Item, property: string, itemLocalTime: number): number | boolean | string;
export function sliceAutomation(item: import("./types.js").Item, begin: number, end: number, options?: {
    freshIds?: boolean;
}): Record<string, import("./types.js").AutomationBinding>;
export function transitionWindow(project: import("./types.js").Project, transition: import("./types.js").TransitionInstance): {
    begin: number;
    end: number;
};
export function evaluateFrame(project: import("./types.js").Project, time: number): {
    time: number;
    canvas: import("./types.js").Project["canvas"];
    layers: import("./types.js").FrameLayer[];
    audio: import("./types.js").FrameLayer[];
};
export function evaluateEffects(item: import("./types.js").Item, time: number): import("./types.js").EffectInstance[];
export function renewEffectIdentities(clip: import("./types.js").Item["clip"]): void;
export function evaluateVisual(item: import("./types.js").Item, time: number): Required<import("./types.js").VisualProperties>;
export function evaluateAudio(item: import("./types.js").Item, time: number): Required<import("./types.js").AudioProperties>;
export function activeTransitions(project: import("./types.js").Project, time: number): (import("./types.js").TransitionInstance & {
    begin: number;
    end: number;
    progress: number;
})[];
/** @type {Readonly<Record<string, import('./types.js').PropertyDescriptor>>} */
export const PROPERTY_DESCRIPTORS: Readonly<Record<string, import("./types.js").PropertyDescriptor>>;
/** @type {readonly import('./types.js').EffectTemplate[]} */
export const EFFECT_TEMPLATES: readonly import("./types.js").EffectTemplate[];
/** @type {readonly import('./types.js').EffectTemplate[]} */
export const TRANSITION_TEMPLATES: readonly import("./types.js").EffectTemplate[];
export const HTML_MAX_BYTES: 1048576;
export const HTML_MAX_SIDE: 4096;
export const HTML_MAX_PIXELS: 8388608;
