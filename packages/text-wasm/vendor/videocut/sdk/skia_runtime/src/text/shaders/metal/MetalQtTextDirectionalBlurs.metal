
#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct QtDirectionalCopyVertex {
  packed_float3 position;
  packed_float2 uv;
};

struct QtDirectionalQuadVertex {
  packed_float4 position;
  packed_float3 normal;
  packed_float2 uv;
};

struct QtDirectionalVaryings {
  float2 uv [[user(locn0)]];
  float4 position [[position]];
};

vertex QtDirectionalVaryings qtDirectionalCopyVertex(
    const device QtDirectionalCopyVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  QtDirectionalCopyVertex input = vertices[vertexId];
  QtDirectionalVaryings output;
  output.position = float4(float3(input.position), 1.0);
  output.uv = float2(input.uv);
  output.position.z = (output.position.z + output.position.w) * 0.5;
  return output;
}

vertex QtDirectionalVaryings qtDirectionalQuadVertex(
    const device QtDirectionalQuadVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  QtDirectionalQuadVertex input = vertices[vertexId];
  QtDirectionalVaryings output = {};
  output.position = sign(float4(float2(input.position.xy), 0.0, 1.0));
  output.uv = float2(input.uv);
  output.position.z = (output.position.z + output.position.w) * 0.5;
  return output;
}

[[early_fragment_tests]]
fragment float4 qtDirectionalDownsampleFragment(
    QtDirectionalVaryings input [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]]) {
  return source.sample(sourceSampler, input.uv);
}

struct QtDirectionalBlurUniforms {
  float4 u_ScreenParams;
  float u_sample;
  float u_sigma;
  float u_spaceDither;
  float u_stepX;
  float u_stepY;
  int u_borderType;
  int u_directionNum;
  float u_exposure;
};

static inline __attribute__((always_inline))
float qtDirectionalGaussian(thread const float& distanceValue,
                            thread const float& sigma) {
  return exp((((-0.5) * distanceValue) * distanceValue) / (sigma * sigma));
}

static inline __attribute__((always_inline))
float qtDirectionalDither(thread const float2& uv,
                          constant float4& screenParams) {
  float3 value = fract(float3((uv * screenParams.xy).xyx) *
                       0.103100001811981201171875);
  float3 mixed = value +
                 float3(dot(value,
                            value.yzx + float3(33.3300018310546875)));
  return (fract(fract((mixed.x + mixed.y) * mixed.z)) * 2.0) - 1.0;
}

static inline __attribute__((always_inline))
float qtDirectionalMirror(thread float& value) {
  value = abs(value);
  return abs((floor(ceil(value) / 2.0) * 2.0) - value);
}

fragment float4 qtDirectionalGaussianFragment(
    QtDirectionalVaryings input [[stage_in]],
    constant QtDirectionalBlurUniforms& buffer [[buffer(0)]],
    texture2d<float> inputTexture [[texture(0)]],
    sampler inputSampler [[sampler(0)]]) {
  if (buffer.u_sample <
      9.9999997473787516355514526367188e-06) {
    return inputTexture.sample(inputSampler, input.uv);
  }
  float zeroDistance = 0.0;
  float sigma = buffer.u_sigma;
  float centerWeight = qtDirectionalGaussian(zeroDistance, sigma);
  float normalization = centerWeight;
  float4 accumulated =
      inputTexture.sample(inputSampler, input.uv) * centerWeight;
  float2 centerUv = input.uv;
  if (buffer.u_spaceDither >
      9.9999997473787516355514526367188e-06) {
    float2 ditherUv = input.uv;
    centerUv += float2(buffer.u_stepX, buffer.u_stepY) *
                (buffer.u_spaceDither *
                 qtDirectionalDither(ditherUv, buffer.u_ScreenParams));
  }
  float2 sampleUv = centerUv;
  for (int index = 1; index <= 1024; ++index) {
    float sampleIndex = float(index);
    if (sampleIndex > buffer.u_sample)
      break;
    float2 offset = float2(buffer.u_stepX, buffer.u_stepY) * sampleIndex;
    float distanceValue = length(offset);
    float sampleSigma = buffer.u_sigma;
    float weight = qtDirectionalGaussian(distanceValue, sampleSigma);

    sampleUv = centerUv - offset;
    bool outside = sampleUv.x < 0.0;
    if (!outside)
      outside = sampleUv.y < 0.0;
    if (!outside)
      outside = sampleUv.x > 1.0;
    if (!outside)
      outside = sampleUv.y > 1.0;
    if (outside) {
      if (buffer.u_borderType == 1) {
        normalization += weight;
      } else if (buffer.u_borderType == 2) {
        float mirrorX = sampleUv.x;
        sampleUv.x = qtDirectionalMirror(mirrorX);
        float mirrorY = sampleUv.y;
        sampleUv.y = qtDirectionalMirror(mirrorY);
        accumulated += inputTexture.sample(inputSampler, sampleUv) * weight;
        normalization += weight;
      }
    } else {
      accumulated += inputTexture.sample(inputSampler, sampleUv) * weight;
      normalization += weight;
    }

    sampleUv = centerUv + offset;
    outside = sampleUv.x < 0.0;
    if (!outside)
      outside = sampleUv.y < 0.0;
    if (!outside)
      outside = sampleUv.x > 1.0;
    if (!outside)
      outside = sampleUv.y > 1.0;
    if (outside) {
      if (buffer.u_borderType == 1) {
        normalization += weight;
      } else if (buffer.u_borderType == 2) {
        float mirrorX = sampleUv.x;
        sampleUv.x = qtDirectionalMirror(mirrorX);
        float mirrorY = sampleUv.y;
        sampleUv.y = qtDirectionalMirror(mirrorY);
        accumulated += inputTexture.sample(inputSampler, sampleUv) * weight;
        normalization += weight;
      }
    } else {
      accumulated += inputTexture.sample(inputSampler, sampleUv) * weight;
      normalization += weight;
    }
  }
  accumulated /= float4(normalization);
  if (buffer.u_directionNum == 1) {
    float4 value = accumulated;
    float3 exposed = value.xyz * buffer.u_exposure;
    accumulated.x = exposed.x;
    accumulated.y = exposed.y;
    accumulated.z = exposed.z;
  }
  return fast::clamp(accumulated, float4(0.0), float4(1.0));
}

struct QtDirectionalBlendUniforms {
  int u_blendMode;
  int u_directionNum;
  float u_exposure;
};

static inline __attribute__((always_inline))
float4 qtDirectionalScreen(thread const float4& left,
                           thread const float4& right) {
  return (left + right) - (left * right);
}

static inline __attribute__((always_inline))
float4 qtDirectionalAdd(thread const float4& left,
                        thread const float4& right) {
  return left + right;
}

static inline __attribute__((always_inline))
float4 qtDirectionalBlend(thread const float4& left,
                          thread const float4& right,
                          constant int& blendMode) {
  if (blendMode == 0) {
    float4 first = left;
    float4 second = right;
    return qtDirectionalScreen(first, second);
  }
  float4 first = left;
  float4 second = right;
  return qtDirectionalAdd(first, second);
}

fragment float4 qtDirectionalMeanBlendFragment(
    QtDirectionalVaryings input [[stage_in]],
    constant QtDirectionalBlendUniforms& buffer [[buffer(0)]],
    texture2d<float> texture1 [[texture(0)]],
    texture2d<float> texture2 [[texture(1)]],
    texture2d<float> texture3 [[texture(2)]],
    texture2d<float> texture4 [[texture(3)]],
    sampler sampler1 [[sampler(0)]],
    sampler sampler2 [[sampler(1)]],
    sampler sampler3 [[sampler(2)]],
    sampler sampler4 [[sampler(3)]]) {
  float4 value = texture1.sample(sampler1, input.uv);
  if (buffer.u_directionNum >= 2) {
    float4 left = value;
    float4 right = texture2.sample(sampler2, input.uv);
    value = qtDirectionalBlend(left, right, buffer.u_blendMode);
  }
  if (buffer.u_directionNum >= 3) {
    float4 left = value;
    float4 right = texture3.sample(sampler3, input.uv);
    value = qtDirectionalBlend(left, right, buffer.u_blendMode);
  }
  if (buffer.u_directionNum >= 4) {
    float4 left = value;
    float4 right = texture4.sample(sampler4, input.uv);
    value = qtDirectionalBlend(left, right, buffer.u_blendMode);
  }
  if (buffer.u_blendMode == 2)
    value /= float4(float(buffer.u_directionNum));
  float4 exposedValue = value;
  float3 exposedRgb = exposedValue.xyz * buffer.u_exposure;
  value.x = exposedRgb.x;
  value.y = exposedRgb.y;
  value.z = exposedRgb.z;
  return fast::clamp(value, float4(0.0), float4(1.0));
}

struct QtAlphaOutlineUniforms {
  float offsetX;
  float offsetY;
  float ratio;
  float size;
  float scaleX;
  float scaleY;
  float4 outlineColor;
  float intensity;
};

fragment float4 qtAlphaOutlineFragment(
    QtDirectionalVaryings input [[stage_in]],
    constant QtAlphaOutlineUniforms& buffer [[buffer(0)]],
    texture2d<float> tDiffuse [[texture(0)]],
    sampler tDiffuseSampler [[sampler(0)]]) {
  float4 sampled = tDiffuse.sample(tDiffuseSampler, input.uv);
  half3 sampledHalfRgb = half4(sampled).xyz;
  float3 linearRgb = mix(
      pow((float3(sampledHalfRgb) +
           float3(0.054999999701976776123046875)) *
              float3(0.947867333889007568359375),
          float3(2.400000095367431640625)),
      float3(sampledHalfRgb) *
          float3(0.077399380505084991455078125),
      step(float3(sampledHalfRgb),
           float3(0.040449999272823333740234375)));
  float4 originalLinear = sampled;
  originalLinear.x = linearRgb.x;
  originalLinear.y = linearRgb.y;
  originalLinear.z = linearRgb.z;

  float2 outlineUv =
      input.uv - float2(buffer.offsetX, buffer.offsetY / buffer.ratio);
  float scaledSize =
      buffer.size * 0.001000000047497451305389404296875;
  float maxNeighborAlpha = 0.0;
  for (int index = 0; index < 16;) {
    float angle = float(half(index)) *
                  0.3926990926265716552734375;
    float2 direction = float2(cos(angle), sin(angle)) * scaledSize;
    maxNeighborAlpha = fast::max(
        maxNeighborAlpha,
        float(half4(tDiffuse.sample(
                        tDiffuseSampler,
                        outlineUv +
                            float2(direction.x * buffer.scaleX,
                                   direction.y *
                                       (buffer.scaleY / buffer.ratio))))
                  .w));
    ++index;
  }
  float outlineAlpha =
      fast::clamp(maxNeighborAlpha -
                      float(half4(tDiffuse.sample(tDiffuseSampler, outlineUv))
                                .w),
                  0.0, 1.0) *
      buffer.outlineColor.w;
  float3 linearOutline = mix(
      pow((buffer.outlineColor.xyz +
           float3(0.054999999701976776123046875)) *
              float3(0.947867333889007568359375),
          float3(2.400000095367431640625)),
      buffer.outlineColor.xyz *
          float3(0.077399380505084991455078125),
      step(buffer.outlineColor.xyz,
           float3(0.040449999272823333740234375)));
  float4 effectColor =
      float4(mix(originalLinear.xyz, linearOutline, float3(outlineAlpha)),
             fast::max(float(half4(sampled).w), outlineAlpha));
  float4 finalLinear =
      mix(originalLinear, effectColor, float4(buffer.intensity));
  float3 finalRgb = finalLinear.xyz;
  float3 srgb = mix(
      (pow(finalRgb, float3(0.4166666567325592041015625)) *
       1.05499994754791259765625) -
          float3(0.054999999701976776123046875),
      finalRgb * 12.9200000762939453125,
      step(finalRgb, float3(0.003130800090730190277099609375)));
  finalLinear.x = srgb.x;
  finalLinear.y = srgb.y;
  finalLinear.z = srgb.z;
  return finalLinear;
}
