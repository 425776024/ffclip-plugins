// Generated from implementation by scripts/build-types.mjs. Do not edit.
export function parseCurve(value: any): any[];
export function parseCube(source: any): {
    size: number;
    rows: number[][];
    min: number[];
    max: number[];
};
export const LOOK_NAMES: {
    none: string;
    grayscale: string;
    bright: string;
    warm: string;
    cool: string;
    vintage: string;
    film: string;
    japanese: string;
    dramatic: string;
    soft: string;
    'high-contrast': string;
    sepia: string;
    cyberpunk: string;
    fairy: string;
    cinematic: string;
};
export namespace COLOR_PARAMETERS {
    namespace brightness {
        export let type: string;
        export { label };
        export { min };
        export { max };
        export { value as default };
        export { unit };
        export let step: number;
        export let keyframe: boolean;
    }
    namespace contrast { }
    namespace saturation { }
    namespace hue { }
    namespace temperature { }
    namespace tint { }
    namespace exposure {
        let step_1: number;
        export { step_1 as step };
    }
    namespace gamma {
        let step_2: number;
        export { step_2 as step };
    }
    namespace highlights { }
    namespace shadows { }
    namespace grayscale { }
    namespace hslHue { }
    namespace hslSaturation { }
    namespace hslLightness { }
}
export const IDENTITY_CUBE: "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1";
export const RESTORED_EFFECTS: ({
    id: string;
    name: string;
    parameters: {
        brightness: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        contrast: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        saturation: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        hue: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        temperature: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        tint: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        exposure: {
            step: number;
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            keyframe: boolean;
        };
        gamma: {
            step: number;
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            keyframe: boolean;
        };
        highlights: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        shadows: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        grayscale: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        hslHue: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        hslSaturation: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        hslLightness: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
    };
} | {
    id: string;
    name: string;
    parameters: {
        preset: {
            type: string;
            label: string;
            values: string[];
            default: string;
            keyframe: boolean;
        };
        amount: {
            step: number;
            keyframe: boolean;
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
        };
        sharpness?: undefined;
        noise?: undefined;
        vignette?: undefined;
        cube?: undefined;
    };
} | {
    id: string;
    name: string;
    parameters: {
        sharpness: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        noise: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        vignette: {
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
            step: number;
            keyframe: boolean;
        };
        preset?: undefined;
        amount?: undefined;
        cube?: undefined;
    };
} | {
    id: string;
    name: string;
    parameters: {
        cube: {
            type: string;
            label: string;
            default: string;
            maxLength: number;
            keyframe: boolean;
        };
        amount: {
            step: number;
            keyframe: boolean;
            type: string;
            label: any;
            min: number;
            max: number;
            default: number;
            unit: string;
        };
        preset?: undefined;
        sharpness?: undefined;
        noise?: undefined;
        vignette?: undefined;
    };
})[];
