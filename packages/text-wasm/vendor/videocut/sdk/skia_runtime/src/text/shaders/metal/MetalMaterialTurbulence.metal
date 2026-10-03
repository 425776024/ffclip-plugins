#include <metal_stdlib>
using namespace metal;

struct MaterialTurbulenceUniforms {
    float2 textureSize, viewportSize, scale, offset;
    float rotation, evolution, complexity, brightness;
    float contrast, range, subImpact, subScale;
    float subRotation, spriteMode, cycle, padding0;
    float2 subOffset, padding1;
    float4 spriteToOutput0, spriteToOutput1, outputToSprite0, outputToSprite1;
};
struct MaterialTurbulenceVertex {
    float4 position [[position]];
    float2 uv;
};
vertex MaterialTurbulenceVertex materialTurbulenceVertex(uint index [[vertex_id]]) {
    constexpr float2 corners[] = {{0,0}, {1,0}, {1,1}, {1,1}, {0,1}, {0,0}};
    float2 uv = corners[index];
    return {float4(uv * 2.0 - 1.0, 0.0, 1.0), uv};
}

float hash21(float2 point) {
    float2 p = fract(point * 1324.518);
    p += dot(p, p.yx + 22.541);
    return fract((p.x + p.y) * p.y);
}

float2 gradient(float2 point, float2 seed, bool firstOctave,
                constant MaterialTurbulenceUniforms &u) {
    float n = hash21(point);
    float phase = seed.x + n;
    float phase0 = floor(phase);
    float phase1 = floor(phase + 1.0);
    if (u.spriteMode > 0.5 && u.cycle >= 2.0) {
        phase0 = floor(phase - u.cycle * floor(phase / u.cycle));
        phase1 = floor(phase + 1.0 - u.cycle * floor((phase + 1.0) / u.cycle));
    }
    // Preserve the captured Metal grouping: the first octave folds its seed
    // multiplier to a constant; later octaves fuse it with the phase scale.
    float scale0 = fma(phase0, 4.412, 34.532);
    float scale1 = fma(phase1, 4.412, 34.532);
    scale0 = firstOctave ? scale0 + seed.y * 0.8031 : fma(seed.y, 0.8031, scale0);
    scale1 = firstOctave ? scale1 + seed.y * 0.8031 : fma(seed.y, 0.8031, scale1);
    float2 p = fract(point * scale0);
    p += dot(p, p.yx + 15.434);
    float2 first = fract(fma(p + p.yx + 0.523, p.yx, float2(n)));
    p = fract(point * scale1);
    p += dot(p, p.yx + 15.434);
    float2 second = fract(fma(p + p.yx + 0.523, p.yx, float2(n)));
    return mix(first, second, fract(phase)) * 2.0 - 1.0;
}

float2 rotateUv(float2 uv, float angle, constant MaterialTurbulenceUniforms &u) {
    uv.y *= u.viewportSize.y / u.viewportSize.x;
    float s = sin(angle), c = cos(angle);
    uv = float2x2(float2(c, s), float2(-s, c)) * (uv - 0.5) + 0.5;
    uv.y *= u.viewportSize.x / u.viewportSize.y;
    return uv;
}

float interpolate(float2 uv, float2 seed, bool firstOctave,
                  constant MaterialTurbulenceUniforms &u) {
    float2 gridSize = u.spriteMode > 0.5
        ? 720.0 * u.viewportSize / min(u.viewportSize.x, u.viewportSize.y) : u.viewportSize;
    float2 grid = (uv * gridSize) * 0.01;
    float2 cell = floor(grid), p = fract(grid);
    float2 weight = ((p * p) * p) * fma(p, p * 6.0 - 15.0, float2(10.0));
    float n00 = dot(gradient(cell, seed, firstOctave, u), p);
    float n10 = dot(gradient(cell + float2(1,0), seed, firstOctave, u), p - float2(1,0));
    float n01 = dot(gradient(cell + float2(0,1), seed, firstOctave, u), p - float2(0,1));
    float n11 = dot(gradient(cell + 1.0, seed, firstOctave, u), p - 1.0);
    float value = mix(mix(n00, n10, weight.x), mix(n01, n11, weight.x), weight.y);
    if (u.spriteMode > 0.5) value = 2.0 / (1.0 + exp(-3.0 * value)) - 1.0;
    return fma(value, 0.5, 0.5);
}

float2 nextOctave(float2 uv, constant MaterialTurbulenceUniforms &u) {
    float2 ratio = 720.0 * u.viewportSize / min(u.viewportSize.x, u.viewportSize.y);
    uv -= u.subOffset / ratio;
    uv = rotateUv(uv, u.subRotation * 0.01745329238474369, u);
    return uv * (100.0 / u.subScale);
}

float noise(float2 uv, float seed, constant MaterialTurbulenceUniforms &u) {
    float layers = clamp(u.complexity, 1.0, 10.0);
    float fullLayers = floor(layers), partialLayer = fract(layers);
    float sum = interpolate(uv, float2(u.evolution, 1.0 + seed), true, u);
    float normalization = 1.0, weight = u.subImpact;
    for (int layer = 2; layer <= 10; ++layer) {
        if (float(layer) > fullLayers) break;
        uv = nextOctave(uv, u);
        sum = fma(interpolate(uv, float2(u.evolution, float(layer) + seed), false, u), weight, sum);
        normalization += weight;
        weight *= u.subImpact;
    }
    uv = nextOctave(uv, u);
    sum = fma(interpolate(uv, float2(u.evolution, floor(fullLayers + 1.0) + seed), false, u)
              * weight, partialLayer, sum);
    normalization = fma(weight, partialLayer, normalization);
    return clamp(sum / normalization, 0.0, 1.0);
}

float adjust(float n, constant MaterialTurbulenceUniforms &u) {
    n += u.brightness;
    if (u.spriteMode > 0.5) return (n - 0.5) * u.contrast * 10.0 + 0.5;
    return fma(n - 0.5, u.contrast > 0.0 ? fma(u.contrast, 10.0, 1.0) : u.contrast + 1.0, 0.5);
}

fragment float4 materialTurbulenceFragment(MaterialTurbulenceVertex in [[stage_in]],
    constant MaterialTurbulenceUniforms &u [[buffer(0)]],
    texture2d<float> inputTexture [[texture(0)]]) {
    constexpr sampler linearClamp(coord::normalized, filter::linear, address::clamp_to_edge);
    if (u.spriteMode > 0.5) {
        float3 position = float3(in.position.x, u.textureSize.y - in.position.y, 1.0);
        float2 sourceUv = float2(dot(u.outputToSprite0.xyz, position),
                                dot(u.outputToSprite1.xyz, position));
        float2 uv = rotateUv(sourceUv - u.offset, u.rotation * 0.01745329238474369, u);
        uv = uv / u.scale - 10.0;
        float2 displacement = float2(adjust(noise(uv, 2.11, u), u),
                                     adjust(noise(uv, 9.71, u), u)) - 0.5;
        displacement.y *= u.viewportSize.x / u.viewportSize.y;
        float2 warped = sourceUv + displacement * clamp(u.scale.x, 0.01, 1.0) * u.range;
        warped = mix(sourceUv, warped, smoothstep(float2(0.0), float2(0.25),
                                                min(sourceUv, 1.0 - sourceUv)));
        float3 point = float3(warped, 1.0);
        float2 samplePosition = float2(dot(u.spriteToOutput0.xyz, point),
                                       dot(u.spriteToOutput1.xyz, point));
        return inputTexture.sample(linearClamp,
            float2(samplePosition.x / u.textureSize.x, 1.0 - samplePosition.y / u.textureSize.y));
    }
    float2 uv = rotateUv(in.uv - 0.5, u.rotation * 0.01745329238474369, u);
    uv = fma(uv, 1.0 / u.scale, float2(0.5)) - u.offset;
    float2 displacement = float2(adjust(noise(uv, 0.0, u), u),
                                 adjust(noise(uv, 0.6221, u), u)) - 0.5;
    return inputTexture.sample(linearClamp,
        fma(displacement, float2(clamp(u.scale.x, 0.01, 1.0) * u.range), in.uv));
}
