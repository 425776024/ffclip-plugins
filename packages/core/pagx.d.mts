// Generated from implementation by scripts/build-types.mjs. Do not edit.
/** Shared admission boundary: a self-contained document driven by one absolute clock.
 * @param {string} xml
 */
export function parsePagx(xml: string): any;
/** @param {string} xml */
export function pagxMetadata(xml: string): {
    width: number;
    height: number;
    animationDuration: number;
    frameRate: number;
    animated: boolean;
};
/** @param {import('./types.js').PagxContent} content */
export function validatePagxContent(content: import("./types.js").PagxContent): import("./types.js").PagxContent;
/** @param {string} xml
 * @param {{duration?: number, transparent?: boolean}} [options]
 */
export function createPagxContent(xml: string, options?: {
    duration?: number;
    transparent?: boolean;
}): import("./types.js").PagxContent;
/** Parse editable semantic values; animated base values are displayed as read-only.
 * @param {string} xml
 */
export function pagxFields(xml: string): {
    key: string;
    group: any;
    label: any;
    type: any;
    value: any;
    min: any;
    max: any;
    animated: boolean;
}[];
/** Atomically update the parsed properties and source canvas/clock.
 * @param {import('./types.js').PagxContent} content
 * @param {Record<string,string>} values
 * @param {{width?:number,height?:number,duration?:number,transparent?:boolean}} [options]
 */
export function updatePagxContent(content: import("./types.js").PagxContent, values: Record<string, string>, options?: {
    width?: number;
    height?: number;
    duration?: number;
    transparent?: boolean;
}): import("./types.js").PagxContent;
/** @param {import('./types.js').PagxContent} content @param {string} key @param {string} value */
export function setPagxField(content: import("./types.js").PagxContent, key: string, value: string): import("./types.js").PagxContent;
export const PAGX_MAX_BYTES: number;
