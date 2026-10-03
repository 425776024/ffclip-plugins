
#include <metal_stdlib>
using namespace metal;

struct GaussianVertex {
  packed_float2 position;
  packed_float2 uv;
};

struct GaussianVaryings {
  float4 position [[position]];
  float2 uv [[user(texturecoord)]];
};

struct GaussianUniforms {
  float4 screenParams;
  int inverseGammaCorrection;
  float gamma;
  float sampleCount;
  float sigma;
  float spaceDither;
  float stepX;
  float stepY;
  int borderType;
  int blurAlpha;
};

vertex GaussianVaryings qtTextGaussianVertex(
    const device GaussianVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  GaussianVertex input = vertices[vertexId];
  GaussianVaryings output;
  output.position = float4(float2(input.position), 0.0f, 1.0f);
  output.uv = float2(input.uv);
  return output;
}

static inline __attribute__((always_inline))
float4 qtTextGaussianSample(texture2d<float> source, sampler sourceSampler,
                            float2 uv, constant GaussianUniforms &uniforms) {
  float4 color = source.sample(sourceSampler, uv);
  if (uniforms.inverseGammaCorrection == 1) {
    float4 original = color;
    float3 corrected = pow(original.xyz, float3(uniforms.gamma));
    color.x = corrected.x;
    color.y = corrected.y;
    color.z = corrected.z;
  }
  return color;
}

static inline __attribute__((always_inline))
float qtTextGaussianWeight(float distanceValue, float sigma) {
  return exp((((-0.5f) * distanceValue) * distanceValue) /
             (sigma * sigma));
}

fragment float4 qtTextGaussianXFragment(
    GaussianVaryings input [[stage_in]],
    constant GaussianUniforms &uniforms [[buffer(0)]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]]) {
  if (uniforms.sampleCount <
      9.9999997473787516355514526367188e-06f) {
    return source.sample(sourceSampler, input.uv);
  }
  float2 parameter = input.uv;
  float4 center = qtTextGaussianSample(source, sourceSampler, parameter,
                                       uniforms);
  float centerDistance = 0.0f;
  float centerWeight = qtTextGaussianWeight(centerDistance, uniforms.sigma);
  float normalization = centerWeight;
  float4 accumulated = center * centerWeight;
  float2 sampleUv = input.uv;
  for (int index = 1; index <= 1024; ++index) {
    float sampleIndex = float(index);
    if (sampleIndex > uniforms.sampleCount)
      break;
    float distanceValue = sampleIndex * uniforms.stepX;
    float weight = qtTextGaussianWeight(distanceValue, uniforms.sigma);
    sampleUv.x = input.uv.x - distanceValue;
    if (sampleUv.x >= 0.0f) {
      accumulated += qtTextGaussianSample(source, sourceSampler, sampleUv,
                                           uniforms) * weight;
      normalization += weight;
    }
    sampleUv.x = input.uv.x + distanceValue;
    if (sampleUv.x <= 1.0f) {
      accumulated += qtTextGaussianSample(source, sourceSampler, sampleUv,
                                           uniforms) * weight;
      normalization += weight;
    }
  }
  accumulated /= float4(normalization);
  if (uniforms.inverseGammaCorrection == 1) {
    float4 original = accumulated;
    float3 corrected = pow(original.xyz, float3(1.0f / uniforms.gamma));
    accumulated.x = corrected.x;
    accumulated.y = corrected.y;
    accumulated.z = corrected.z;
  }
  if (uniforms.blurAlpha == 0)
    accumulated.w = center.w;
  return accumulated;
}

fragment float4 qtTextGaussianYFragment(
    GaussianVaryings input [[stage_in]],
    constant GaussianUniforms &uniforms [[buffer(0)]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]]) {
  if (uniforms.sampleCount <
      9.9999997473787516355514526367188e-06f) {
    return source.sample(sourceSampler, input.uv);
  }
  float2 parameter = input.uv;
  float4 center = qtTextGaussianSample(source, sourceSampler, parameter,
                                       uniforms);
  float centerDistance = 0.0f;
  float centerWeight = qtTextGaussianWeight(centerDistance, uniforms.sigma);
  float normalization = centerWeight;
  float4 accumulated = center * centerWeight;
  float2 sampleUv = input.uv;
  for (int index = 1; index <= 1024; ++index) {
    float sampleIndex = float(index);
    if (sampleIndex > uniforms.sampleCount)
      break;
    float distanceValue = sampleIndex * uniforms.stepY;
    float weight = qtTextGaussianWeight(distanceValue, uniforms.sigma);
    sampleUv.y = input.uv.y - distanceValue;
    if (sampleUv.y >= 0.0f) {
      accumulated += qtTextGaussianSample(source, sourceSampler, sampleUv,
                                           uniforms) * weight;
      normalization += weight;
    }
    sampleUv.y = input.uv.y + distanceValue;
    if (sampleUv.y <= 1.0f) {
      accumulated += qtTextGaussianSample(source, sourceSampler, sampleUv,
                                           uniforms) * weight;
      normalization += weight;
    }
  }
  accumulated /= float4(normalization);
  if (uniforms.inverseGammaCorrection == 1) {
    float4 original = accumulated;
    float3 corrected = pow(original.xyz, float3(1.0f / uniforms.gamma));
    accumulated.x = corrected.x;
    accumulated.y = corrected.y;
    accumulated.z = corrected.z;
  }
  if (uniforms.blurAlpha == 0)
    accumulated.w = center.w;
  return accumulated;
}
