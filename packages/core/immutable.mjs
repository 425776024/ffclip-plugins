// Generated from core/immutable.mts. Do not edit.
/** Copy-on-write draft for the JSON document. Unchanged branches retain identity. */
export function createProjectDraft(base) {
    const states = new WeakMap(), proxies = new WeakMap();
    const object = (value) => value !== null && typeof value === 'object';
    function changed(state) {
        if (state.modified)
            return;
        state.modified = true;
        for (const parent of state.parents)
            changed(parent);
    }
    function link(state, parent) {
        if (parent) {
            state.parents.add(parent);
            if (state.modified)
                changed(parent);
        }
        return state.proxy;
    }
    function wrap(value, parent) {
        if (!object(value))
            return value;
        if (proxies.has(value))
            return link(proxies.get(value), parent);
        if (states.has(value))
            return link(states.get(value), parent);
        const state = { base: value, copy: null, proxy: null, modified: false, parents: new Set() };
        const source = () => state.copy ?? state.base;
        state.proxy = new Proxy(Array.isArray(value) ? [] : {}, {
            get(_target, key) { return wrap(Reflect.get(source(), key), state); },
            set(_target, key, next) {
                const current = source()[key], raw = proxies.get(next)?.base ?? next;
                if (Object.is(current, raw) && Object.hasOwn(source(), key))
                    return true;
                state.copy ??= Array.isArray(value) ? value.slice() : { ...value };
                state.copy[key] = next;
                changed(state);
                return true;
            },
            deleteProperty(_target, key) {
                if (!Object.hasOwn(source(), key))
                    return true;
                state.copy ??= Array.isArray(value) ? value.slice() : { ...value };
                delete state.copy[key];
                changed(state);
                return true;
            },
            ownKeys() { return Reflect.ownKeys(source()); },
            has(_target, key) { return key in source(); },
            getOwnPropertyDescriptor(_target, key) {
                const descriptor = Object.getOwnPropertyDescriptor(source(), key);
                if (!descriptor)
                    return undefined;
                return { ...descriptor, configurable: key !== 'length' || !Array.isArray(value) };
            }
        });
        states.set(value, state);
        proxies.set(state.proxy, state);
        return link(state, parent);
    }
    function finish(value, cache = new WeakMap()) {
        if (!object(value))
            return value;
        const state = proxies.get(value) ?? states.get(value);
        if (state && !state.modified)
            return state.base;
        const original = state?.base ?? value, source = state?.copy ?? original;
        if (cache.has(original))
            return cache.get(original);
        let result = source, changed = source !== original;
        for (const key of Object.keys(source)) {
            const child = finish(source[key], cache);
            if (child !== source[key]) {
                if (result === original)
                    result = Array.isArray(source) ? source.slice() : { ...source };
                result[key] = child;
                changed = true;
            }
        }
        if (changed && Object.keys(result).length === Object.keys(original).length &&
            (!Array.isArray(result) || result.length === original.length) &&
            Object.keys(result).every((key) => Object.hasOwn(original, key) && result[key] === original[key]))
            result = original;
        cache.set(original, result);
        return result;
    }
    return { draft: wrap(base), finish: () => finish(base) };
}
const owned = (value) => value === undefined ? value : JSON.parse(JSON.stringify(value));
const keyed = (value) => Array.isArray(value) && value.every((v) => v && typeof v === 'object' && typeof v.id === 'string');
/** Stable IDs avoid copying all following clips when a collection is inserted/reordered. */
export function documentPatches(before, after, path = [], output = []) {
    if (before === after)
        return output;
    if (keyed(before) && keyed(after)) {
        const old = new Map(before.map((v) => [v.id, v])), next = new Map(after.map((v) => [v.id, v]));
        for (const id of old.keys())
            if (!next.has(id))
                output.push({ op: 'remove', path: [...path, { id }] });
        for (const [id, value] of next) {
            if (!old.has(id))
                output.push({ op: 'set', path: [...path, { id }], value: owned(value) });
            else
                documentPatches(old.get(id), value, [...path, { id }], output);
        }
        if (before.length !== after.length || before.some((v, i) => v.id !== after[i]?.id))
            output.push({ op: 'order', path, value: after.map((v) => v.id) });
        return output;
    }
    if (before && after && typeof before === 'object' && typeof after === 'object' &&
        !Array.isArray(before) && !Array.isArray(after)) {
        for (const key of Object.keys(before))
            if (!Object.hasOwn(after, key))
                output.push({ op: 'remove', path: [...path, key] });
        for (const key of Object.keys(after)) {
            if (!Object.hasOwn(before, key))
                output.push({ op: 'set', path: [...path, key], value: owned(after[key]) });
            else
                documentPatches(before[key], after[key], [...path, key], output);
        }
        return output;
    }
    if (JSON.stringify(before) !== JSON.stringify(after))
        output.push({ op: 'set', path, value: owned(after) });
    return output;
}
export function applyDocumentPatches(project, patches) {
    const state = createProjectDraft(project);
    for (const patch of patches) {
        if (!patch.path.length)
            throw new Error('History cannot replace the entire document');
        let owner = state.draft;
        const keyAt = (container, part, append = false) => {
            if (typeof part === 'object') {
                if (!Array.isArray(container))
                    throw new Error('历史路径已失效');
                const index = container.findIndex((value) => value.id === part.id);
                if (index < 0 && !append)
                    throw new Error('历史对象已失效');
                return index < 0 ? container.length : index;
            }
            if (['__proto__', 'constructor', 'prototype'].includes(part))
                throw new Error('历史路径无效');
            return part;
        };
        for (const part of patch.path.slice(0, -1))
            owner = owner[keyAt(owner, part)];
        const key = keyAt(owner, patch.path.at(-1), patch.op === 'set');
        if (patch.op === 'remove') {
            if (Array.isArray(owner))
                owner.splice(Number(key), 1);
            else
                delete owner[key];
        }
        else if (patch.op === 'set')
            owner[key] = owned(patch.value);
        else if (patch.op === 'order') {
            const entries = new Map(owner[key].map((value) => [value.id, value]));
            if (entries.size !== patch.value.length || patch.value.some((id) => !entries.has(id)))
                throw new Error('历史顺序已失效');
            owner[key] = patch.value.map((id) => entries.get(id));
        }
        else
            throw new Error('历史补丁无效');
    }
    return state.finish();
}
