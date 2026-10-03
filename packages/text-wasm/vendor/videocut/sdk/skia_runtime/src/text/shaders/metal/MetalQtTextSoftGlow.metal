
#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct SoftGlowVertexInput {
  float2 a_position [[attribute(0)]];
  float2 a_texcoord0 [[attribute(1)]];
};

struct SoftGlowVertexOutput {
  float2 v_uv [[user(locn0)]];
  float4 gl_Position [[position]];
};

vertex SoftGlowVertexOutput qtTextSoftGlowVertex(
    SoftGlowVertexInput in [[stage_in]]) {
  SoftGlowVertexOutput out = {};
  out.gl_Position = sign(float4(in.a_position, 0.0, 1.0));
  out.v_uv = in.a_texcoord0;
  out.gl_Position.z = (out.gl_Position.z + out.gl_Position.w) * 0.5;
  return out;
}

struct SoftGlowThresholdUniforms {
  int u_thresholdType;
  float u_thresholdLow;
  float u_thresholdHigh;
  float u_thresholdSmooth;
  float u_grayScale;
};

static inline __attribute__((always_inline))
float qtTextSoftGlowThresholdS(thread float& value,
                               thread const float& low,
                               thread const float& high) {
  if ((value <= low) || (value > high)) {
    value = 0.0;
  } else {
    value = (value - low) / (1.0 - low);
  }
  return value;
}

static inline __attribute__((always_inline))
float4 qtTextSoftGlowThresholdRgb(thread float4& color,
                                 thread const float& low,
                                 thread const float& high) {
  float red = color.x;
  float redLow = low;
  float redHigh = high;
  float retainedRed = qtTextSoftGlowThresholdS(red, redLow, redHigh);
  float green = color.y;
  float greenLow = low;
  float greenHigh = high;
  float retainedGreen = qtTextSoftGlowThresholdS(green, greenLow, greenHigh);
  float blue = color.z;
  float blueLow = low;
  float blueHigh = high;
  float retainedBlue = qtTextSoftGlowThresholdS(blue, blueLow, blueHigh);
  float originalRed = color.x;
  float originalGreen = color.y;
  float originalBlue = color.z;
  float4 original = color;
  float3 scaled = original.xyz *
      (((retainedRed + retainedGreen) + retainedBlue) /
       fast::max((originalRed + originalGreen) + originalBlue,
                 9.9999997473787516355514526367188e-06));
  color.x = scaled.x;
  color.y = scaled.y;
  color.z = scaled.z;
  return color;
}

static inline __attribute__((always_inline))
float qtTextSoftGlowThresholdD(thread float& value,
                               thread const float& low,
                               thread const float& high,
                               thread const float& smoothValue) {
  if (value <= low) {
    value = ((smoothValue * value) * value) /
            fast::max(low, 9.9999997473787516355514526367188e-06);
  } else if (value > high) {
    value = smoothValue * (((value * value) - (high * value)) + high);
  }
  return value;
}

static inline __attribute__((always_inline))
float4 qtTextSoftGlowThresholdDetailed(thread float4& color,
                                      thread const float& low,
                                      thread const float& high,
                                      thread const float& smoothValue) {
  float red = color.x;
  float redLow = low;
  float redHigh = high;
  float redSmooth = smoothValue;
  color.x = qtTextSoftGlowThresholdD(red, redLow, redHigh, redSmooth);
  float green = color.y;
  float greenLow = low;
  float greenHigh = high;
  float greenSmooth = smoothValue;
  color.y = qtTextSoftGlowThresholdD(green, greenLow, greenHigh, greenSmooth);
  float blue = color.z;
  float blueLow = low;
  float blueHigh = high;
  float blueSmooth = smoothValue;
  color.z = qtTextSoftGlowThresholdD(blue, blueLow, blueHigh, blueSmooth);
  return color;
}

static inline __attribute__((always_inline))
float qtTextSoftGlowLuminance(thread const float3& color) {
  return dot(color, float3(0.2125999927520751953125,
                           0.715200006961822509765625,
                           0.072200000286102294921875));
}

fragment float4 qtTextSoftGlowThresholdFragment(
    SoftGlowVertexOutput in [[stage_in]],
    constant SoftGlowThresholdUniforms& buffer [[buffer(0)]],
    texture2d<float> inputTexture [[texture(0)]],
    sampler inputSampler [[sampler(0)]]) {
  float4 color = inputTexture.sample(inputSampler, in.v_uv);
  if (buffer.u_thresholdType == 0) {
    float4 parameter = color;
    float low = buffer.u_thresholdLow;
    float high = buffer.u_thresholdHigh;
    color = qtTextSoftGlowThresholdRgb(parameter, low, high);
  } else {
    float4 parameter = color;
    float low = buffer.u_thresholdLow;
    float high = buffer.u_thresholdHigh;
    float smoothValue = buffer.u_thresholdSmooth;
    float4 detailed = qtTextSoftGlowThresholdDetailed(
        parameter, low, high, smoothValue);
    color = detailed;
    float3 luminanceParameter = detailed.xyz;
    float4 original = color;
    float3 mixed = mix(original.xyz,
                       float3(qtTextSoftGlowLuminance(luminanceParameter)),
                       float3(buffer.u_grayScale));
    color.x = mixed.x;
    color.y = mixed.y;
    color.z = mixed.z;
  }
  return color;
}

fragment float4 qtTextSoftGlowCopyFragment(
    SoftGlowVertexOutput in [[stage_in]],
    texture2d<float> inputTexture [[texture(0)]],
    sampler inputSampler [[sampler(0)]]) {
  return inputTexture.sample(inputSampler, in.v_uv);
}

struct SoftGlowXUniforms {
  float u_sampleX;
  float u_sigmaX;
  float u_stepX;
};

struct SoftGlowYUniforms {
  float u_sampleY;
  float u_sigmaY;
  float u_stepY;
  float u_exposure;
};

static inline __attribute__((always_inline))
float qtTextSoftGlowGaussian(thread const float& distanceValue,
                            thread const float& sigma) {
  return exp((((-0.5) * distanceValue) * distanceValue) / (sigma * sigma));
}

fragment float4 qtTextSoftGlowXFragment(
    SoftGlowVertexOutput in [[stage_in]],
    constant SoftGlowXUniforms& buffer [[buffer(0)]],
    texture2d<float> inputTexture [[texture(0)]],
    sampler inputSampler [[sampler(0)]]) {
  if (buffer.u_sampleX < 9.9999997473787516355514526367188e-06) {
    return inputTexture.sample(inputSampler, in.v_uv);
  }
  float zero = 0.0;
  float sigma = buffer.u_sigmaX;
  float centerWeight = qtTextSoftGlowGaussian(zero, sigma);
  float normalization = centerWeight;
  float4 accumulated = inputTexture.sample(inputSampler, in.v_uv) *
                       centerWeight;
  float2 sampleUv = in.v_uv;
  for (int index = 1; index <= 1024; ++index) {
    float sampleIndex = float(index);
    if (sampleIndex > buffer.u_sampleX)
      break;
    float distanceValue = sampleIndex * buffer.u_stepX;
    float distanceParameter = distanceValue;
    float sigmaParameter = buffer.u_sigmaX;
    float weight = qtTextSoftGlowGaussian(distanceParameter, sigmaParameter);
    sampleUv.x = in.v_uv.x - distanceValue;
    if (sampleUv.x >= 0.0) {
      accumulated += inputTexture.sample(inputSampler, sampleUv) * weight;
      normalization += weight;
    }
    sampleUv.x = in.v_uv.x + distanceValue;
    if (sampleUv.x <= 1.0) {
      accumulated += inputTexture.sample(inputSampler, sampleUv) * weight;
      normalization += weight;
    }
  }
  float4 value = accumulated;
  float4 normalized = value / float4(normalization);
  accumulated = normalized;
  return normalized;
}

fragment float4 qtTextSoftGlowYFragment(
    SoftGlowVertexOutput in [[stage_in]],
    constant SoftGlowYUniforms& buffer [[buffer(0)]],
    texture2d<float> inputTexture [[texture(0)]],
    sampler inputSampler [[sampler(0)]]) {
  if (buffer.u_sampleY < 9.9999997473787516355514526367188e-06) {
    return inputTexture.sample(inputSampler, in.v_uv);
  }
  float zero = 0.0;
  float sigma = buffer.u_sigmaY;
  float centerWeight = qtTextSoftGlowGaussian(zero, sigma);
  float normalization = centerWeight;
  float4 accumulated = inputTexture.sample(inputSampler, in.v_uv) *
                       centerWeight;
  float2 sampleUv = in.v_uv;
  for (int index = 1; index <= 1024; ++index) {
    float sampleIndex = float(index);
    if (sampleIndex > buffer.u_sampleY)
      break;
    float distanceValue = sampleIndex * buffer.u_stepY;
    float distanceParameter = distanceValue;
    float sigmaParameter = buffer.u_sigmaY;
    float weight = qtTextSoftGlowGaussian(distanceParameter, sigmaParameter);
    sampleUv.y = in.v_uv.y - distanceValue;
    if (sampleUv.y >= 0.0) {
      accumulated += inputTexture.sample(inputSampler, sampleUv) * weight;
      normalization += weight;
    }
    sampleUv.y = in.v_uv.y + distanceValue;
    if (sampleUv.y <= 1.0) {
      accumulated += inputTexture.sample(inputSampler, sampleUv) * weight;
      normalization += weight;
    }
  }
  float4 value = accumulated;
  float4 normalized = value / float4(normalization);
  accumulated = normalized;
  float3 exposed = normalized.xyz * buffer.u_exposure;
  accumulated.x = exposed.x;
  accumulated.y = exposed.y;
  accumulated.z = exposed.z;
  return fast::clamp(accumulated, float4(0.0), float4(1.0));
}

struct SoftGlowBlendUniforms {
  float u_exposure;
  float3 u_glowColor;
  int u_displayGlow;
};

static inline __attribute__((always_inline))
float4 qtTextSoftGlowScreen(thread const float4& glow,
                           thread const float4& input) {
  return (glow + input) - (glow * input);
}

fragment float4 qtTextSoftGlowBlendFragment(
    SoftGlowVertexOutput in [[stage_in]],
    constant SoftGlowBlendUniforms& buffer [[buffer(0)]],
    texture2d<float> inputTexture [[texture(0)]],
    texture2d<float> glowTexture [[texture(1)]],
    sampler inputSampler [[sampler(0)]],
    sampler glowSampler [[sampler(1)]]) {
  float4 inputColor = inputTexture.sample(inputSampler, in.v_uv);
  float4 glow = float4(0.0);
  if (buffer.u_exposure > 9.9999997473787516355514526367188e-06) {
    glow = glowTexture.sample(glowSampler, in.v_uv);
  }
  float4 originalGlow = glow;
  float3 tinted = originalGlow.xyz * buffer.u_glowColor;
  glow.x = tinted.x;
  glow.y = tinted.y;
  glow.z = tinted.z;
  float4 glowParameter = glow;
  float4 inputParameter = inputColor;
  if (buffer.u_displayGlow == 1) {
    return fast::clamp(glow, float4(0.0), float4(1.0));
  }
  return fast::clamp(qtTextSoftGlowScreen(glowParameter, inputParameter),
                     float4(0.0), float4(1.0));
}

// LumiDeepGlow captured shader family.  These entry points retain the
// translator's float operation order and every RGBA8 render-target boundary;
// the C++ executor below supplies the bounded serialized pass topology.
struct DeepGlowPreprocessUniforms {
  float u_glowFromAlpha;
  int u_ca;
  float u_redOffset;
  float u_greenOffset;
  float u_blueOffset;
  int u_gamma;
  float u_gammaValue;
};

fragment float4 qtTextDeepGlowPreprocessFragment(
    SoftGlowVertexOutput in [[stage_in]],
    constant DeepGlowPreprocessUniforms& buffer [[buffer(0)]],
    texture2d<float> inputTexture [[texture(0)]],
    sampler inputSampler [[sampler(0)]]) {
  float4 sampled = inputTexture.sample(inputSampler, in.v_uv);
  float4 color = sampled;
  color = mix(sampled, float4(color.w), float4(buffer.u_glowFromAlpha));
  if (buffer.u_ca == 1) {
    float4 shifted = inputTexture.sample(
        inputSampler,
        in.v_uv + float2(-buffer.u_redOffset, buffer.u_redOffset)).yzwx;
    float shiftedAlpha = shifted.w;
    float shiftedChannel = shifted.x;
    float red = shiftedChannel * shiftedAlpha;
    shifted.x = red;
    shifted = inputTexture.sample(
        inputSampler,
        in.v_uv + float2(-buffer.u_greenOffset, buffer.u_greenOffset)).yzwx;
    shiftedAlpha = shifted.w;
    shiftedChannel = shifted.y;
    float green = shiftedChannel * shiftedAlpha;
    shifted.y = green;
    shifted = inputTexture.sample(
        inputSampler,
        in.v_uv + float2(-buffer.u_blueOffset, buffer.u_blueOffset)).yzwx;
    shiftedAlpha = shifted.w;
    shiftedChannel = shifted.z;
    float blue = shiftedChannel * shiftedAlpha;
    color = float4(red, green, blue, 1.0);
  }
  if (buffer.u_gamma == 1)
    color = pow(color, float4(buffer.u_gammaValue));
  return color;
}

fragment float4 qtTextDeepGlowDownscaleFragment(
    SoftGlowVertexOutput in [[stage_in]],
    texture2d<float> inputTexture [[texture(0)]],
    sampler inputSampler [[sampler(0)]]) {
  return inputTexture.sample(inputSampler, in.v_uv);
}

struct DeepGlowBlurUniforms {
  float u_gammaValue;
  float u_stepsInt;
  float u_angle;
  float2 u_aspect;
  float u_rotate;
  float u_steps;
  float u_stride;
  float u_sigma;
};

static inline __attribute__((always_inline))
float2 qtTextDeepGlowRotate(thread const float2& value,
                           thread const float& angle) {
  float sine = sin(angle);
  float cosine = cos(angle);
  return float2x2(float2(cosine, -sine), float2(sine, cosine)) * value;
}

static inline __attribute__((always_inline))
float qtTextDeepGlowGaussian(thread const float& value,
                            thread const float& sigma) {
  return exp((-(value * value)) / ((2.0 * sigma) * sigma)) /
         (2.5066282749176025390625 * sigma);
}

static inline __attribute__((always_inline))
float3 qtTextDeepGlowGamma(thread const float3& value,
                          constant float& gammaValue) {
  return float3(pow(value, float3(gammaValue)));
}

fragment float4 qtTextDeepGlowBlurFragment(
    SoftGlowVertexOutput in [[stage_in]],
    constant DeepGlowBlurUniforms& buffer [[buffer(0)]],
    texture2d<float> inputTexture [[texture(0)]],
    sampler inputSampler [[sampler(0)]]) {
  float radiansAngle = (buffer.u_angle / 180.0) *
                       3.141592502593994140625;
  float2 direction = float2(cos(radiansAngle), sin(radiansAngle)) /
                     buffer.u_aspect;
  float rotateRadians = buffer.u_rotate * 0.01745329251;
  direction = qtTextDeepGlowRotate(direction, rotateRadians);

  float zero = 0.0;
  float sigma = buffer.u_sigma;
  float normalization = qtTextDeepGlowGaussian(zero, sigma);
  float centerWeight = normalization;
  float3 center = inputTexture.sample(inputSampler, in.v_uv).xyz;
  float3 accumulated = float3(0.0);
  int boundedSteps = int(buffer.u_steps);
  int increment = int(buffer.u_stepsInt);
  for (int tap = 1; tap < 32; tap += increment) {
    if (tap >= boundedSteps)
      break;
    float tapValue = float(tap);
    float normalizedTap = (tapValue / buffer.u_steps) * 15.0;
    float tapSigma = buffer.u_sigma;
    float weight = qtTextDeepGlowGaussian(normalizedTap, tapSigma);
    float2 offset = (direction * tapValue) * buffer.u_stride;
    float3 positive = inputTexture.sample(inputSampler, in.v_uv + offset).xyz;
    float3 negative = inputTexture.sample(inputSampler, in.v_uv - offset).xyz;
    accumulated +=
        (qtTextDeepGlowGamma(positive, buffer.u_gammaValue) +
         qtTextDeepGlowGamma(negative, buffer.u_gammaValue)) * weight;
    normalization += weight * 2.0;
  }
  float3 color =
      (accumulated +
       qtTextDeepGlowGamma(center, buffer.u_gammaValue) * centerWeight) /
      float3(normalization);
  return float4(pow(color, float3(1.0 / buffer.u_gammaValue)), 1.0);
}

struct DeepGlowCompositeUniforms {
  int u_blendMode;
  float u_opacity;
  float u_gammaValue;
  int u_comp;
  float u_mult;
};

static inline __attribute__((always_inline))
float qtTextDeepGlowScreenChannel(thread const float& left,
                                 thread const float& right) {
  return 1.0 - ((1.0 - left) * (1.0 - right));
}

static inline __attribute__((always_inline))
float3 qtTextDeepGlowScreen(thread const float3& left,
                           thread const float3& right) {
  float lx = left.x;
  float rx = right.x;
  float ly = left.y;
  float ry = right.y;
  float lz = left.z;
  float rz = right.z;
  return float3(qtTextDeepGlowScreenChannel(lx, rx),
                qtTextDeepGlowScreenChannel(ly, ry),
                qtTextDeepGlowScreenChannel(lz, rz));
}

static inline __attribute__((always_inline))
float3 qtTextDeepGlowBlendScreen(thread const float3& left,
                                thread const float3& right,
                                thread const float& opacity) {
  float3 l = left;
  float3 r = right;
  return qtTextDeepGlowScreen(l, r) * opacity + left * (1.0 - opacity);
}

static inline __attribute__((always_inline))
float3 qtTextDeepGlowBlendAdd(thread const float3& left,
                             thread const float3& right,
                             thread const float& opacity) {
  float3 l = left;
  float3 r = right;
  return fast::min(l + r, float3(1.0)) * opacity +
         left * (1.0 - opacity);
}

static inline __attribute__((always_inline))
float4 qtTextDeepGlowCompositeBlend(
    thread const float4& left, thread const float4& right,
    constant int& blendMode, constant float& opacity) {
  float3 color;
  if (blendMode == 0) {
    float3 l = left.xyz / float3(fast::max(left.w,
        0.001000000047497451305389404296875));
    float3 r = right.xyz / float3(fast::max(left.w,
        0.001000000047497451305389404296875));
    float amount = opacity;
    color = (left.xyz * (1.0 - right.w)) +
            (right.xyz * (1.0 - left.w)) +
            qtTextDeepGlowBlendScreen(l, r, amount) * (left.w * right.w);
  } else {
    float3 l = left.xyz / float3(fast::max(left.w,
        0.001000000047497451305389404296875));
    float3 r = right.xyz / float3(fast::max(left.w,
        0.001000000047497451305389404296875));
    float amount = opacity;
    color = (left.xyz * (1.0 - right.w)) +
            (right.xyz * (1.0 - left.w)) +
            qtTextDeepGlowBlendAdd(l, r, amount) * (left.w * right.w);
  }
  return float4(color, left.w + right.w * (1.0 - left.w));
}

fragment float4 qtTextDeepGlowCompositeFragment(
    SoftGlowVertexOutput in [[stage_in]],
    constant DeepGlowCompositeUniforms& buffer [[buffer(0)]],
    texture2d<float> inputTexture [[texture(0)]],
    texture2d<float> blurTexture [[texture(1)]],
    sampler inputSampler [[sampler(0)]],
    sampler blurSampler [[sampler(1)]]) {
  float4 input = inputTexture.sample(inputSampler, in.v_uv);
  float4 blur = blurTexture.sample(blurSampler, in.v_uv);
  blur = float4(pow(blur.xyz, float3(buffer.u_gammaValue)), blur.w);
  float4 color;
  if (buffer.u_comp == 1) {
    input = float4(pow(input.xyz, float3(buffer.u_gammaValue)), input.w);
    color = qtTextDeepGlowCompositeBlend(
        input, blur, buffer.u_blendMode, buffer.u_opacity);
  } else {
    color = blur * buffer.u_opacity;
  }
  color *= buffer.u_mult;
  color = float4(pow(color.xyz,
                     float3(1.0 / buffer.u_gammaValue)), color.w);
  return fast::clamp(color, float4(0.0), float4(1.0));
}

struct DeepGlowPostprocessUniforms {
  int u_blendMode;
  int u_tint;
  int u_tintMode;
  float3 u_tintColor;
  float u_tintMix;
  float u_srcOpacity;
};

static inline __attribute__((always_inline))
float qtTextDeepGlowOverlayChannel(thread const float& left,
                                  thread const float& right) {
  if (left < 0.5)
    return (2.0 * left) * right;
  return 1.0 - ((2.0 * (1.0 - left)) * (1.0 - right));
}

static inline __attribute__((always_inline))
float3 qtTextDeepGlowSoftLight(thread const float3& left,
                             thread const float3& right) {
  return ((float3(1.0) - float3(2.0) * right) * pow(left, float3(2.0))) +
         ((float3(2.0) * right) * left);
}

static inline __attribute__((always_inline))
float4 qtTextDeepGlowAlphaFromMaximum(thread const float4& color) {
  float maximum = fast::max(fast::max(color.x, color.y), color.z);
  return maximum > 0.0 ? float4(color.xyz, maximum) : float4(0.0);
}

static inline __attribute__((always_inline))
float4 qtTextDeepGlowFinalBlend(thread const float4& left,
                               thread const float4& right,
                               constant int& blendMode) {
  float3 color;
  if (blendMode == 0) {
    float3 l = left.xyz;
    float3 r = right.xyz;
    color = qtTextDeepGlowScreen(l, r);
  } else {
    color = fast::min(left.xyz + right.xyz, float3(1.0));
  }
  return float4(color,
                (left.w + right.w) - (left.w * right.w));
}

fragment float4 qtTextDeepGlowPostprocessFragment(
    SoftGlowVertexOutput in [[stage_in]],
    constant DeepGlowPostprocessUniforms& buffer [[buffer(0)]],
    texture2d<float> blurTexture [[texture(0)]],
    texture2d<float> inputTexture [[texture(1)]],
    sampler blurSampler [[sampler(0)]],
    sampler inputSampler [[sampler(1)]]) {
  float4 glow = blurTexture.sample(blurSampler, in.v_uv);
  float4 source = inputTexture.sample(inputSampler, in.v_uv);
  if (buffer.u_tint == 1) {
    float4 tinted;
    tinted.w = glow.w;
    if (buffer.u_tintMode == 0) {
      float3 value = buffer.u_tintColor *
          dot(glow.xyz, float3(0.2989999949932098388671875,
                               0.58700001239776611328125,
                               0.114000000059604644775390625));
      tinted.xyz = value;
    } else if (buffer.u_tintMode == 1) {
      tinted.xyz = glow.xyz * buffer.u_tintColor;
    } else if (buffer.u_tintMode == 2) {
      float3 left = pow(glow.xyz, float3(1.0));
      float3 right = pow(buffer.u_tintColor, float3(1.0));
      float lx = left.x;
      float rx = right.x;
      float ly = left.y;
      float ry = right.y;
      float lz = left.z;
      float rz = right.z;
      tinted.xyz = float3(qtTextDeepGlowOverlayChannel(lx, rx),
                          qtTextDeepGlowOverlayChannel(ly, ry),
                          qtTextDeepGlowOverlayChannel(lz, rz));
    } else {
      glow.xyz = fast::min(glow.xyz, float3(1.0));
      float3 l = glow.xyz;
      float3 r = buffer.u_tintColor;
      tinted.xyz = qtTextDeepGlowSoftLight(l, r);
    }
    glow = mix(glow, tinted, float4(buffer.u_tintMix));
  }
  glow = qtTextDeepGlowAlphaFromMaximum(glow);
  float4 left = source;
  float4 right = glow;
  return mix(glow, qtTextDeepGlowFinalBlend(left, right, buffer.u_blendMode),
             float4(buffer.u_srcOpacity));
}
