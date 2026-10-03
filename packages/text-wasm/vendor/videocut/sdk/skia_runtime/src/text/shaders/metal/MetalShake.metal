
#include <metal_stdlib>
using namespace metal;

struct ShakeVertex {
  packed_float2 position;
  packed_float2 uv;
};

struct ShakeMatrices {
  float4x4 uvR;
  float4x4 uvG;
  float4x4 uvB;
};

struct ShakeFillModes {
  int x;
  int y;
};

struct ShakeVaryings {
  float4 position [[position]];
  float2 uvR [[user(locn0)]];
  float2 uvG [[user(locn1)]];
  float2 uvB [[user(locn2)]];
};

vertex ShakeVaryings qtTextShakeVertex(
    const device ShakeVertex *vertices [[buffer(0)]],
    constant ShakeMatrices &matrices [[buffer(1)]],
    uint vertexId [[vertex_id]]) {
  const ShakeVertex input = vertices[vertexId];
  const float4 uv = float4(float2(input.uv), 0.0f, 1.0f);
  ShakeVaryings output;
  output.position = float4(float2(input.position), 0.0f, 1.0f);
  output.uvR = (matrices.uvR * uv).xy;
  output.uvG = (matrices.uvG * uv).xy;
  output.uvB = (matrices.uvB * uv).xy;
  return output;
}

inline float qtMod(const float value, const float divisor) {
  return value - divisor * floor(value / divisor);
}

inline float4 qtShakeSample(texture2d<float> source,
                            sampler sourceSampler,
                            float2 uv,
                            constant ShakeFillModes &fill) {
  float maskX = 1.0f;
  if (fill.x == 1) {
    uv.x = fract(uv.x);
  } else if (fill.x == 2) {
    uv.x = abs(qtMod(uv.x + 1.0f, 2.0f) - 1.0f);
  } else {
    maskX = step(0.0f, uv.x) * step(uv.x, 1.0f);
  }
  float maskY = 1.0f;
  if (fill.y == 1) {
    uv.y = fract(uv.y);
  } else if (fill.y == 2) {
    uv.y = abs(qtMod(uv.y + 1.0f, 2.0f) - 1.0f);
  } else {
    maskY = step(0.0f, uv.y) * step(uv.y, 1.0f);
  }
  return source.sample(sourceSampler, uv) * (maskX * maskY);
}

fragment float4 qtTextShakeFragment(
    ShakeVaryings input [[stage_in]],
    constant ShakeFillModes &fill [[buffer(0)]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]]) {
  const float4 red = qtShakeSample(source, sourceSampler, input.uvR, fill);
  const float4 green = qtShakeSample(source, sourceSampler, input.uvG, fill);
  const float4 blue = qtShakeSample(source, sourceSampler, input.uvB, fill);
  return float4(red.r, green.g, blue.b,
                fast::max(fast::max(red.a, green.a), blue.a));
}
