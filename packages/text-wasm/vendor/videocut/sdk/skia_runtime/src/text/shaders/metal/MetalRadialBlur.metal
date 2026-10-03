
#include <metal_stdlib>
using namespace metal;

struct RadialVertex {
  packed_float2 position;
  packed_float2 uv;
};

struct RadialUniforms {
  int inverseGammaCorrection;
  float gamma;
  float intensity;
  int blurType;
  float2 center;
  float quality;
  float sampleScale;
  float sampleBias;
  float weightDecay;
  float normalizationSample;
  float dither;
  int borderType;
  float lightIntensity;
  int blurAlpha;
  float lightTransferMode;
  float screenWidth;
  float screenHeight;
};

struct RadialVaryings {
  float4 position [[position]];
  float2 uv [[user(locn0)]];
};

vertex RadialVaryings qtTextRadialBlurVertex(
    const device RadialVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  const RadialVertex input = vertices[vertexId];
  RadialVaryings output;
  output.position = sign(float4(float2(input.position), 0.0f, 1.0f));
  output.uv = float2(input.uv);
  output.position.z = (output.position.z + output.position.w) * 0.5f;
  return output;
}

inline float radialHash(const float2 value) {
  const float2 q0 = fract(value * 13.5170001983642578125f);
  const float2 q = q0 + float2(dot(q0, q0.yx + float2(22.5410003662109375f)));
  return fract((q.x + q.y) * q.y);
}

inline float mirrorCoordinate(float value) {
  value = abs(value);
  return abs(floor(ceil(value) / 2.0f) * 2.0f - value);
}

inline float4 sampleAdjusted(texture2d<float> source,
                             sampler sourceSampler,
                             const float2 uv,
                             constant RadialUniforms &uniforms) {
  float4 color = source.sample(sourceSampler, uv);
  if (uniforms.inverseGammaCorrection == 1) {
    color.rgb = pow(color.rgb, float3(uniforms.gamma));
  }
  return color;
}

inline void accumulateAt(texture2d<float> source,
                         sampler sourceSampler,
                         float2 uv,
                         const float weight,
                         constant RadialUniforms &uniforms,
                         thread float4 &accumulated,
                         thread float &accumulatedWeight) {
  const bool outside = uv.x < 0.0f || uv.y < 0.0f ||
                       uv.x > 1.0f || uv.y > 1.0f;
  if (outside) {
    if (uniforms.borderType != 0) {
      uv.x = mirrorCoordinate(uv.x);
      uv.y = mirrorCoordinate(uv.y);
      accumulated += sampleAdjusted(source, sourceSampler, uv, uniforms) * weight;
    }
  } else {
    accumulated += sampleAdjusted(source, sourceSampler, uv, uniforms) * weight;
  }
  accumulatedWeight += weight;
}

inline float2 rotateUv(float2 uv,
                       const float2 center,
                       const float angle,
                       const float aspect) {
  const float sine = sin(angle);
  const float cosine = cos(angle);
  uv -= center;
  uv.y /= aspect;
  uv = float2(cosine * uv.x - sine * uv.y,
              sine * uv.x + cosine * uv.y);
  uv.y *= aspect;
  return uv + center;
}

inline float4 lightTransfer(const float4 source,
                            const float4 blurred,
                            const float mode) {
  if (mode < 0.5f) {
    return blurred;
  }
  if (mode < 1.5f) {
    return blurred + source;
  }
  if (mode < 2.5f) {
    return fast::max(blurred, source);
  }
  if (mode < 3.5f) {
    return float4(1.0f) -
           (float4(1.0f) - blurred) * (float4(1.0f) - source);
  }
  return blurred;
}

fragment float4 qtTextRadialBlurFragment(
    RadialVaryings input [[stage_in]],
    constant RadialUniforms &uniforms [[buffer(0)]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]]) {
  float adjustedIntensity = uniforms.intensity;
  const bool rotateMode = uniforms.blurType >= 3;
  const bool symmetric = uniforms.blurType == 2 || uniforms.blurType == 4;
  if (symmetric) {
    adjustedIntensity *= 0.5f;
  }

  const float4 original =
      sampleAdjusted(source, sourceSampler, input.uv, uniforms);
  float4 accumulated = original;
  float accumulatedWeight = 1.0f;
  float currentWeight = 1.0f;
  float extent = length((input.uv - uniforms.center) * adjustedIntensity);
  extent = sign(extent) *
           (0.89999997615814208984375f * abs(extent) +
            0.100000001490116119384765625f);
  const float sampleCount = fast::min(
      (uniforms.sampleScale * uniforms.quality) * extent +
          uniforms.sampleBias,
      128.0f);
  const float weightStep = pow(
      pow(uniforms.weightDecay, uniforms.normalizationSample),
      1.0f / sampleCount);

  if (!rotateMode) {
    const float2 step =
        ((input.uv - uniforms.center) * adjustedIntensity) / sampleCount;
    float2 origin = input.uv;
    if (uniforms.dither > 9.9999997473787516355514526367188e-06f) {
      origin += step *
                (uniforms.dither * (radialHash(input.uv) * 2.0f - 1.0f));
    }
    for (int sampleIndex = 1; sampleIndex <= 128; ++sampleIndex) {
      const float index = float(sampleIndex);
      if (index > sampleCount) {
        break;
      }
      currentWeight *= weightStep;
      const float2 delta = step * index;
      accumulateAt(source, sourceSampler, origin - delta, currentWeight,
                   uniforms, accumulated, accumulatedWeight);
      if (symmetric) {
        accumulateAt(source, sourceSampler, origin + delta, currentWeight,
                     uniforms, accumulated, accumulatedWeight);
      }
    }
  } else {
    const float angleStep =
        6.28318500518798828125f * adjustedIntensity / sampleCount;
    float angleDither = 0.0f;
    if (uniforms.dither > 9.9999997473787516355514526367188e-06f) {
      angleDither = uniforms.dither *
                    (radialHash(input.uv) * 2.0f - 1.0f) * angleStep;
    }
    const float aspect = uniforms.screenWidth / uniforms.screenHeight;
    for (int sampleIndex = 1; sampleIndex <= 128; ++sampleIndex) {
      const float index = float(sampleIndex);
      if (index > sampleCount) {
        break;
      }
      currentWeight *= weightStep;
      accumulateAt(source, sourceSampler,
                   rotateUv(input.uv, uniforms.center,
                            index * angleStep + angleDither, aspect),
                   currentWeight, uniforms, accumulated, accumulatedWeight);
      if (symmetric) {
        accumulateAt(source, sourceSampler,
                     rotateUv(input.uv, uniforms.center,
                              -index * angleStep + angleDither, aspect),
                     currentWeight, uniforms, accumulated,
                     accumulatedWeight);
      }
    }
  }

  float4 blurred = accumulated / accumulatedWeight;
  if (uniforms.inverseGammaCorrection == 1) {
    blurred.rgb = pow(blurred.rgb, float3(1.0f / uniforms.gamma));
  }
  blurred.rgb *= uniforms.lightIntensity;
  if (uniforms.blurAlpha == 0) {
    blurred.a = original.a;
  }
  return lightTransfer(original, blurred, uniforms.lightTransferMode);
}
