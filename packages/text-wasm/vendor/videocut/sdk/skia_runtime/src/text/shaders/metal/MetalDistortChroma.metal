
#include <metal_stdlib>
using namespace metal;

struct DistortVertex {
  packed_float2 position;
  packed_float2 uv;
};

struct DistortVaryings {
  float4 position [[position]];
  float2 uv [[user(locn0)]];
};

vertex DistortVaryings qtTextDistortChromaVertex(
    const device DistortVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  const DistortVertex input = vertices[vertexId];
  DistortVaryings output;
  output.position = sign(float4(float2(input.position), 0.0f, 1.0f));
  output.uv = float2(input.uv);
  output.position.z = (output.position.z + output.position.w) * 0.5f;
  return output;
}

inline float4 qtPackScalar(float value) {
  float4 packed = float4(0.0f);
  value *= 255.0f;
  packed.x = floor(value) / 255.0f;
  value = fract(value);
  value *= 255.0f;
  packed.y = floor(value) / 255.0f;
  value = fract(value);
  value *= 255.0f;
  packed.z = floor(value) / 255.0f;
  value = fract(value);
  packed.w = value;
  return packed;
}

inline float qtDecodeScalar(const float4 packed) {
  return ((packed.x + packed.y / 255.0f) + packed.z / 65025.0f) +
         packed.w / 16581375.0f;
}

fragment float4 qtTextDistortChromaLensFragment(
    DistortVaryings input [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]]) {
  const float4 sampled = source.sample(sourceSampler, input.uv);
  const float grayscale = dot(
      float3(0.2989999949932098388671875f,
             0.58700001239776611328125f,
             0.114000000059604644775390625f),
      sampled.rgb);
  return qtPackScalar(grayscale);
}

struct DistortBlurUniforms {
  float stride;
  float angle;
  float4 screenParams;
  int steps;
};

inline float qtDistortBlurWeight(const float value) {
  return exp(((-0.5f) * (value * value)) /
             0.0900000035762786865234375f);
}

fragment float4 qtTextDistortChromaBlurFragment(
    DistortVaryings input [[stage_in]],
    constant DistortBlurUniforms &uniforms [[buffer(0)]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]]) {
  const float angle =
      (uniforms.angle / 180.0f) * 3.141592502593994140625f;
  const float2 direction =
      float2(cos(angle), sin(angle)) /
      ((uniforms.screenParams.xy * 720.0f) /
       float2(fast::min(uniforms.screenParams.x,
                        uniforms.screenParams.y)));
  float accumulated = 0.0f;
  float accumulatedWeight = 0.0f;
  for (int sampleIndex = 0; sampleIndex < 1000; ++sampleIndex) {
    if (sampleIndex >= uniforms.steps)
      break;
    const float index = float(sampleIndex);
    const float weight = qtDistortBlurWeight(index / float(uniforms.steps));
    const float4 positive = source.sample(
        sourceSampler, input.uv + direction * index * uniforms.stride);
    const float4 negative = source.sample(
        sourceSampler, input.uv + direction * -index * uniforms.stride);
    accumulated +=
        ((qtDecodeScalar(positive) + qtDecodeScalar(negative)) * weight) /
        2.0f;
    accumulatedWeight += weight;
  }
  float blurred = accumulated / accumulatedWeight;
  return qtPackScalar(blurred);
}

struct DistortFinalUniforms {
  float rotateWarpDirection;
  float amountRelX;
  float amountRelY;
  int wrapModeX;
  int wrapModeY;
  int steps;
  float warpRed;
  float warpBlue;
  float4 screenParams;
  float amount;
  float3 color1;
  float3 color2;
  float3 color3;
  float colorMix;
};

template <typename Tx, typename Ty>
inline Tx qtGlslMod(Tx x, Ty y) {
  return x - y * floor(x / y);
}

inline float qtMirrorCoordinate(const float value) {
  const float fractional = fract(value);
  const float parity = floor(qtGlslMod(value, 2.0f));
  return (fractional + parity) - ((fractional * parity) * 2.0f);
}

inline float2 qtWrapCoordinate(float2 coordinate,
                               const int wrapModeX,
                               const int wrapModeY,
                               const float2 aspect) {
  if (wrapModeX == 0)
    coordinate.x = fast::clamp(coordinate.x, 0.0f, 1.0f);
  else if (wrapModeX == 1)
    coordinate.x = qtGlslMod(coordinate.x, aspect.x);
  else if (wrapModeX == 2)
    coordinate.x = qtMirrorCoordinate(coordinate.x);

  if (wrapModeY == 0)
    coordinate.y = fast::clamp(coordinate.y, 0.0f, 1.0f);
  else if (wrapModeY == 1)
    coordinate.y = qtGlslMod(coordinate.y, aspect.y);
  else if (wrapModeY == 2)
    coordinate.y = qtMirrorCoordinate(coordinate.y);
  return coordinate;
}

inline float3 qtChromaWeight(const float phase) {
  if (phase < 0.5f) {
    const float position = 2.0f * (phase * 2.0f);
    return float3(1.0f, 0.0f, 0.0f) *
               fast::clamp(2.0f - position, 0.0f, 1.0f) +
           float3(0.0f, 1.0f, 0.0f) *
               fast::clamp(position, 0.0f, 1.0f);
  }
  const float position = 2.0f * ((phase - 0.5f) * 2.0f);
  return float3(0.0f, 1.0f, 0.0f) *
             fast::clamp(2.0f - position, 0.0f, 1.0f) +
         float3(0.0f, 0.0f, 1.0f) *
             fast::clamp(position, 0.0f, 1.0f);
}

inline float2 qtLensGradient(const float2 uv,
                            const float amount,
                            const float2 aspect,
                            texture2d<float> lens,
                            sampler lensSampler) {
  const float2 delta = float2(abs(amount)) / (aspect * 1080.0f);
  const float x =
      qtDecodeScalar(lens.sample(lensSampler, uv + float2(delta.x, 0.0f))) -
      qtDecodeScalar(lens.sample(lensSampler, uv - float2(delta.x, 0.0f)));
  const float y =
      qtDecodeScalar(lens.sample(lensSampler, uv + float2(0.0f, delta.y))) -
      qtDecodeScalar(lens.sample(lensSampler, uv - float2(0.0f, delta.y)));
  return float2(x, y);
}

inline float2 qtRotate(const float2 value, const float radiansValue) {
  const float sine = sin(radiansValue);
  const float cosine = cos(radiansValue);
  return float2(value.x * cosine - value.y * sine,
                value.x * sine + value.y * cosine);
}

fragment float4 qtTextDistortChromaFinalFragment(
    DistortVaryings input [[stage_in]],
    constant DistortFinalUniforms &uniforms [[buffer(0)]],
    texture2d<float> lens [[texture(0)]],
    texture2d<float> source [[texture(1)]],
    sampler lensSampler [[sampler(0)]],
    sampler sourceSampler [[sampler(1)]]) {
  const float2 aspect =
      uniforms.screenParams.xy /
      float2(fast::min(uniforms.screenParams.x, uniforms.screenParams.y));
  float2 gradient =
      qtLensGradient(input.uv, uniforms.amount, aspect, lens, lensSampler) *
      float2(uniforms.amountRelX, uniforms.amountRelY);
  gradient = qtRotate(
      gradient,
      uniforms.rotateWarpDirection * 0.0174532925100000000000000f);
  const float2 displacement =
      (gradient * uniforms.amount) *
      float2(uniforms.amountRelX, uniforms.amountRelY);

  float4 accumulated = float4(0.0f);
  float4 accumulatedWeight = float4(0.0f);
  const float stepCount = float(uniforms.steps);
  for (int sampleIndex = 0; sampleIndex < 100; ++sampleIndex) {
    if (sampleIndex >= uniforms.steps)
      break;
    const float phase = float(sampleIndex) / stepCount;
    float2 coordinate =
        input.uv + displacement * mix(uniforms.warpRed,
                                      uniforms.warpBlue, phase);
    coordinate = qtWrapCoordinate(coordinate, uniforms.wrapModeX,
                                  uniforms.wrapModeY, aspect);
    const float4 sampled = source.sample(sourceSampler, coordinate);
    const float4 weight = float4(qtChromaWeight(phase), 1.0f);
    accumulated += sampled * weight;
    accumulatedWeight += weight;
  }
  const float4 result = accumulated / accumulatedWeight;
  return mix(result,
             float4(result.rgb *
                        ((uniforms.color1 + uniforms.color2) +
                         uniforms.color3),
                    result.a),
             float4(uniforms.colorMix));
}
