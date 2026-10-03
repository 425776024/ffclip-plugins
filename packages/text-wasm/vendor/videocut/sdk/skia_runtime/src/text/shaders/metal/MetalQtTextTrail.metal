
#include <metal_stdlib>
using namespace metal;

struct TrailVertex {
  packed_float4 position;
  packed_float2 uv;
};

struct TrailVaryings {
  float4 position [[position]];
  float2 uv [[user(locn0)]];
};

struct TrailTimeVaryings {
  float4 position [[position]];
  float2 uv [[user(locn0)]];
  float2 uv1 [[user(locn1)]];
};

vertex TrailVaryings qtTextTrailVertex(
    const device TrailVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  TrailVaryings output;
  output.position = float4(vertices[vertexId].position);
  output.uv = float2(vertices[vertexId].uv);
  output.position.z = (output.position.z + output.position.w) * 0.5f;
  return output;
}

vertex TrailTimeVaryings qtTextTrailTimeVertex(
    const device TrailVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  TrailTimeVaryings output;
  output.position = float4(vertices[vertexId].position);
  output.uv = float2(vertices[vertexId].uv);
  // The closed fixture has equal input/output dimensions and x=y=.5, making
  // LumiTrail.uvTransform exactly identity.
  output.uv1 = output.uv;
  output.position.z = (output.position.z + output.position.w) * 0.5f;
  return output;
}

struct TrailBlurUniforms {
  float sigma;
  int samples;
  float stepValue;
  float2 direction;
};

static inline __attribute__((always_inline))
float qtTextTrailGaussianWeight(float distanceValue, float sigma) {
  return exp((((-0.5f) * distanceValue) * distanceValue) /
             (sigma * sigma));
}

fragment float4 qtTextTrailBlurFragment(
    TrailVaryings input [[stage_in]],
    constant TrailBlurUniforms &uniforms [[buffer(0)]],
    texture2d<float> inputTexture [[texture(0)]],
    sampler inputSampler [[sampler(0)]]) {
  float centerWeight = qtTextTrailGaussianWeight(0.0f, uniforms.sigma);
  float normalization = centerWeight;
  float4 accumulated = inputTexture.sample(inputSampler, input.uv) *
                       centerWeight;
  for (int index = 1; index < 64; ++index) {
    if (index > uniforms.samples)
      break;
    float distanceValue = uniforms.stepValue * float(index);
    float weight = qtTextTrailGaussianWeight(distanceValue, uniforms.sigma);
    float2 offset = uniforms.direction * distanceValue;
    accumulated = accumulated +
                  inputTexture.sample(inputSampler, input.uv + offset) *
                      weight +
                  inputTexture.sample(inputSampler, input.uv - offset) *
                      weight;
    normalization += weight + weight;
  }
  return accumulated / float4(normalization);
}

struct TrailTimeUniforms {
  float passPreviousHistory;
  float decayStep;
  float4 screenParams;
  float authoredBlur;
};

fragment float4 qtTextTrailTimeFragment(
    TrailTimeVaryings input [[stage_in]],
    constant TrailTimeUniforms &uniforms [[buffer(0)]],
    texture2d<float> oldTexture [[texture(0)]],
    texture2d<float> newTexture [[texture(1)]],
    sampler oldSampler [[sampler(0)]],
    sampler newSampler [[sampler(1)]]) {
  float4 oldValue = oldTexture.sample(oldSampler, input.uv);
  float newAlpha = newTexture.sample(newSampler, input.uv1).a;
  float2 texel = float2(1.0f) / uniforms.screenParams.xy;
  float edgeBase = 1.5f;
  if (uniforms.authoredBlur <
      0.00999999977648258209228515625f) {
    edgeBase = 1.5f;
  } else {
    edgeBase = 0.0f;
  }
  float radius = edgeBase + 0.5f;
  float distanceToEdge = 0.0f;
  if (newAlpha >= 0.5f) {
    float nearestSquared = radius * radius;
    for (float x = 0.0f; x <= radius; x += 1.0f) {
      float xSquared = x * x;
      if (xSquared > nearestSquared)
        break;
      for (float y = 0.0f; y <= radius; y += 1.0f) {
        float candidateSquared = xSquared + y * y;
        if (candidateSquared > nearestSquared)
          break;
        bool outside = false;
        if (newTexture.sample(newSampler,
                              input.uv1 + float2(x, y) * texel).a < 0.5f) {
          outside = true;
        }
        if (!outside && y > 0.0f &&
            newTexture.sample(newSampler,
                              input.uv1 + float2(x, -y) * texel).a < 0.5f) {
          outside = true;
        }
        if (!outside && x > 0.0f &&
            newTexture.sample(newSampler,
                              input.uv1 + float2(-x, y) * texel).a < 0.5f) {
          outside = true;
        }
        if (!outside && x > 0.0f && y > 0.0f &&
            newTexture.sample(newSampler,
                              input.uv1 + float2(-x, -y) * texel).a < 0.5f) {
          outside = true;
        }
        if (outside)
          nearestSquared = candidateSquared;
      }
    }
    distanceToEdge = sqrt(nearestSquared);
  }
  float fresh = newAlpha *
                smoothstep(edgeBase - 0.5f, edgeBase + 0.5f,
                           distanceToEdge);
  float history = fast::max(
      oldValue.a * uniforms.passPreviousHistory - uniforms.decayStep, 0.0f);
  float output = fresh + (1.0f - fresh) * history;
  return float4(output);
}

fragment float4 qtTextTrailPassFragment(
    TrailVaryings input [[stage_in]],
    texture2d<float> timeTexture [[texture(0)]],
    sampler timeSampler [[sampler(0)]]) {
  return timeTexture.sample(timeSampler, input.uv);
}

template<typename Tx, typename Ty>
inline Tx qtTextTrailMod(Tx x, Ty y) {
  return x - y * floor(x / y);
}

static inline __attribute__((always_inline))
float3 qtTextTrailRgbToHsv(float3 color) {
  float4 first = mix(float4(color.zy, -1.0f, 0.666666686534881591796875f),
                     float4(color.yz, 0.0f, -0.3333333432674407958984375f),
                     float4(step(color.z, color.y)));
  float4 second = mix(float4(first.xyw, color.x),
                      float4(color.x, first.yzx),
                      float4(step(first.x, color.x)));
  float chroma = second.x - fast::min(second.w, second.y);
  return float3(abs(second.z +
                    ((second.w - second.y) /
                     ((6.0f * chroma) +
                      1.0000000133514319600180897396058e-10f))),
                chroma /
                    (second.x +
                     1.0000000133514319600180897396058e-10f),
                second.x);
}

static inline __attribute__((always_inline))
float3 qtTextTrailHsvToRgb(float3 color) {
  return mix(float3(1.0f),
             fast::clamp(abs((fract(color.xxx +
                                    float3(1.0f,
                                           0.666666686534881591796875f,
                                           0.3333333432674407958984375f)) *
                              6.0f) -
                             float3(3.0f)) -
                             float3(1.0f),
                         float3(0.0f), float3(1.0f)),
             float3(color.y)) * color.z;
}

struct TrailHintUniforms {
  float hintOffset;
  float hintMin;
  float hintMax;
  float hintHue;
  int baseEnabled;
  float4 baseHint;
};

fragment float4 qtTextTrailHintFragment(
    TrailVaryings input [[stage_in]],
    constant TrailHintUniforms &uniforms [[buffer(0)]],
    texture2d<float> timeTexture [[texture(0)]],
    texture2d<float> hintTexture [[texture(1)]],
    texture2d<float> baseTexture [[texture(2)]],
    sampler timeSampler [[sampler(0)]],
    sampler hintSampler [[sampler(1)]],
    sampler baseSampler [[sampler(2)]]) {
  float historyAlpha = timeTexture.sample(timeSampler, input.uv).a;
  float profileX = mix(
      uniforms.hintMin, uniforms.hintMax,
      abs(qtTextTrailMod(((1.0f - historyAlpha) + uniforms.hintOffset) +
                             1.0f,
                         2.0f) -
          1.0f));
  float4 hint = hintTexture.sample(hintSampler, float2(profileX, 0.5f)) *
                historyAlpha;
  float3 straight = fast::clamp(
      hint.xyz /
          float3(hint.a +
                 1.0000000133514319600180897396058e-10f),
      float3(0.0f), float3(1.0f));
  float3 hsv = qtTextTrailRgbToHsv(straight);
  hsv.x = fract(hsv.x + uniforms.hintHue);
  hint = float4(qtTextTrailHsvToRgb(hsv) * hint.a, hint.a);
  if (uniforms.baseEnabled != 0) {
    float4 base = baseTexture.sample(baseSampler, input.uv) *
                  uniforms.baseHint;
    hint = base + hint * (1.0f - base.a);
  }
  return hint;
}
