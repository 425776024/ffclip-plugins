
#include <metal_stdlib>
using namespace metal;

template<typename Tx, typename Ty>
inline Tx qtTextMod(Tx x, Ty y) {
  return x - y * floor(x / y);
}

struct TurbulenceVertex {
  packed_float2 position;
  packed_float2 uv;
};

struct TurbulenceVaryings {
  float4 position [[position]];
  float2 uv [[user(texturecoord)]];
};

struct TurbulenceNoiseUniforms {
  float cycle;
  float4 screenParams;
  float2 offset;
  float rotate;
  float2 scale;
  float type;
  float complexity;
  float evolution;
  float subImpact;
  float subScale;
  float subRotate;
  float2 subOffset;
};

struct TurbulenceDisplacementUniforms {
  float brightness;
  float contrast;
  float4 screenParams;
  float2 scale;
  float range;
  float type;
  float fixType;
  float motionTileType;
};

vertex TurbulenceVaryings qtTextTurbulenceVertex(
    const device TurbulenceVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  TurbulenceVertex input = vertices[vertexId];
  TurbulenceVaryings output;
  output.position = float4(float2(input.position), 0.0f, 1.0f);
  output.uv = float2(input.uv);
  return output;
}

static inline __attribute__((always_inline))
float2 turbulenceRotate(thread float2 &value, float radiansValue,
                        constant float4 &screenParams) {
  value.y *= screenParams.y / screenParams.x;
  float sine = sin(radiansValue);
  float cosine = cos(radiansValue);
  value -= float2(0.5f);
  value = float2x2(float2(cosine, sine), float2(-sine, cosine)) * value;
  value += float2(0.5f);
  value.y *= screenParams.x / screenParams.y;
  return value;
}

static inline __attribute__((always_inline))
float turbulenceHash(float2 value) {
  float2 first = fract(value * 1324.5179443359375f);
  float2 mixed = first +
      float2(dot(first, first.yx + float2(22.5410003662109375f)));
  return fract((mixed.x + mixed.y) * mixed.y);
}

static inline __attribute__((always_inline))
float2 turbulenceGradient(float2 cell, float2 timeSeed, float cycle) {
  float hashed = turbulenceHash(cell);
  float quarter = timeSeed.x * 0.25f + hashed;
  float quarter0 = floor(quarter);
  float quarter1 = quarter0 + 1.0f;
  bool cyclic = cycle >= 2.0f;
  if (cyclic) {
    quarter0 = floor(qtTextMod(quarter, cycle));
    quarter1 = floor(qtTextMod(quarter + 1.0f, cycle));
  }
  float2 firstBase = fract(
      cell * (34.532001495361328125f + quarter0 * 2.0f +
              timeSeed.y * 0.80309998989105224609375f));
  float2 firstMixed = firstBase + float2(dot(
      firstBase, firstBase.yx + float2(15.4340000152587890625f)));
  float2 firstYx = firstMixed.yx;
  float2 hashVector = float2(hashed);
  float2 first = fract(((firstMixed + firstYx +
                         float2(0.5230000019073486328125f)) * firstYx) +
                       hashVector);

  float2 secondBase = fract(
      cell * (34.532001495361328125f + quarter1 * 2.0f +
              timeSeed.y * 0.80309998989105224609375f));
  float2 secondMixed = secondBase + float2(dot(
      secondBase, secondBase.yx + float2(15.4340000152587890625f)));
  float2 secondYx = secondMixed.yx;
  float2 second = fract(((secondMixed + secondYx +
                          float2(0.5230000019073486328125f)) * secondYx) +
                        hashVector);
  second.y += 1.0f;
  float angle =
      mix(first.y, second.y, fract(quarter)) *
      3.141590118408203125f * 2.0f;

  float doubled = timeSeed.x * 2.0f + hashed;
  float doubled0 = floor(doubled);
  float doubled1 = doubled0 + 1.0f;
  if (cyclic) {
    doubled0 = floor(qtTextMod(doubled, cycle));
    doubled1 = floor(qtTextMod(doubled + 1.0f, cycle));
  }
  float2 thirdBase = fract(
      cell * (34.532001495361328125f + doubled0 * 2.0f +
              timeSeed.y * 0.80309998989105224609375f));
  float2 thirdMixed = thirdBase + float2(dot(
      thirdBase, thirdBase.yx + float2(15.4340000152587890625f)));
  float2 thirdYx = thirdMixed.yx;
  float2 third = fract(((thirdMixed + thirdYx +
                         float2(0.5230000019073486328125f)) * thirdYx) +
                       hashVector);

  float2 fourthBase = fract(
      cell * (34.532001495361328125f + doubled1 * 2.0f +
              timeSeed.y * 0.80309998989105224609375f));
  float2 fourthMixed = fourthBase + float2(dot(
      fourthBase, fourthBase.yx + float2(15.4340000152587890625f)));
  float2 fourthYx = fourthMixed.yx;
  float2 fourth = fract(((fourthMixed + fourthYx +
                          float2(0.5230000019073486328125f)) * fourthYx) +
                        hashVector);
  float magnitude =
      mix(0.75f, 1.0f, mix(third.x, fourth.x, fract(doubled))) * 1.0f;
  return float2(cos(angle), sin(angle)) * magnitude;
}

static inline __attribute__((always_inline))
float turbulenceBaseNoise(float2 position, float2 timeSeed, float cycle,
                          constant float4 &screenParams) {
  float2 grid = (float2(720.0f) * screenParams.xy) /
                float2(fast::min(screenParams.x, screenParams.y));
  float2 cell = floor((position * grid) / float2(100.0f));
  float2 local = fract((position * grid) / float2(100.0f));
  float2 blendValue = smoothstep(float2(0.0f), float2(1.0f), local);
  float lower = mix(
      dot(turbulenceGradient(cell + float2(0.0f), timeSeed, cycle),
          local - float2(0.0f)),
      dot(turbulenceGradient(cell + float2(1.0f, 0.0f), timeSeed, cycle),
          local - float2(1.0f, 0.0f)),
      blendValue.x);
  float upper = mix(
      dot(turbulenceGradient(cell + float2(0.0f, 1.0f), timeSeed, cycle),
          local - float2(0.0f, 1.0f)),
      dot(turbulenceGradient(cell + float2(1.0f), timeSeed, cycle),
          local - float2(1.0f)),
      blendValue.x);
  return mix(lower, upper, blendValue.y) * 0.5f + 0.5f;
}

static inline __attribute__((always_inline))
float turbulenceFractalNoise(
    thread float2 &position, float complexity, float evolution,
    float impact, float octaveScale, float octaveRotate, float seed,
    float2 octaveOffset, float cycle, constant float4 &screenParams) {
  float2 grid = (float2(720.0f) * screenParams.xy) /
                float2(fast::min(screenParams.x, screenParams.y));
  float whole = floor(complexity);
  float fractional = fract(complexity);
  float2 basePosition = position;
  float2 timeSeed = float2(evolution, 1.0f + seed);
  float weightSum = 1.0f;
  float amplitude = impact;
  float result =
      turbulenceBaseNoise(basePosition, timeSeed, cycle, screenParams) * 1.0f;
  for (float octave = 2.0f; octave <= 10.0f; octave += 1.0f) {
    if (octave > whole)
      break;
    position -= octaveOffset / grid;
    float2 rotated = position;
    float radiansValue =
        octaveRotate * 3.141592502593994140625f / 180.0f;
    position = turbulenceRotate(rotated, radiansValue, screenParams);
    position *= octaveScale;
    float octaveAmplitude = amplitude;
    weightSum += octaveAmplitude;
    amplitude = octaveAmplitude * impact;
    result += turbulenceBaseNoise(position, timeSeed, cycle, screenParams) *
              octaveAmplitude;
  }
  position -= octaveOffset / grid;
  float2 rotated = position;
  float radiansValue =
      octaveRotate * 3.141592502593994140625f / 180.0f;
  position = turbulenceRotate(rotated, radiansValue, screenParams);
  position *= octaveScale;
  float finalWeightSum = weightSum + amplitude * fractional;
  result = (result +
            turbulenceBaseNoise(position, timeSeed, cycle, screenParams) *
                amplitude * fractional) /
           finalWeightSum;
  return fast::clamp(result, 0.0f, 1.0f);
}

static inline __attribute__((always_inline))
float4 turbulencePackNoise(float2 value) {
  return float4(floor(value.x * 255.0f) / 255.0f,
                fract(value.x * 255.0f),
                floor(value.y * 255.0f) / 255.0f,
                fract(value.y * 255.0f));
}

fragment float4 qtTextTurbulenceNoiseFragment(
    TurbulenceVaryings input [[stage_in]],
    constant TurbulenceNoiseUniforms &uniforms [[buffer(0)]]) {
  float2 position = input.uv - uniforms.offset;
  float2 rotateParameter = position;
  float rotateRadians =
      uniforms.rotate * 3.141592502593994140625f / 180.0f;
  position = turbulenceRotate(rotateParameter, rotateRadians,
                              uniforms.screenParams);
  position = position * (float2(1.0f) / uniforms.scale) - float2(10.0f);
  float first = 0.5f;
  float second = 0.5f;
  float boundedComplexity =
      fast::clamp(uniforms.complexity, 1.0f, 10.0f);
  float octaveScale = 100.0f / uniforms.subScale;
  if (uniforms.type < 0.5f) {
    float2 firstPosition = position;
    first = turbulenceFractalNoise(
        firstPosition, boundedComplexity, uniforms.evolution,
        uniforms.subImpact, octaveScale, uniforms.subRotate,
        2.1099998950958251953125f, uniforms.subOffset, uniforms.cycle,
        uniforms.screenParams);
    float2 secondPosition = position;
    second = turbulenceFractalNoise(
        secondPosition, boundedComplexity, uniforms.evolution,
        uniforms.subImpact, octaveScale, uniforms.subRotate,
        9.71000003814697265625f, uniforms.subOffset, uniforms.cycle,
        uniforms.screenParams);
  } else {
    float2 firstPosition = float2(0.5f, position.y);
    first = turbulenceFractalNoise(
        firstPosition, boundedComplexity, uniforms.evolution,
        uniforms.subImpact, octaveScale, uniforms.subRotate,
        2.1099998950958251953125f, uniforms.subOffset, uniforms.cycle,
        uniforms.screenParams);
    float2 secondPosition = float2(position.x, 0.5f);
    second = turbulenceFractalNoise(
        secondPosition, boundedComplexity, uniforms.evolution,
        uniforms.subImpact, octaveScale, uniforms.subRotate,
        9.71000003814697265625f, uniforms.subOffset, uniforms.cycle,
        uniforms.screenParams);
  }
  return turbulencePackNoise(float2(first, second));
}

static inline __attribute__((always_inline))
float2 turbulenceUnpackNoise(float4 value) {
  return float2(value.x + value.y / 255.0f,
                value.z + value.w / 255.0f);
}

static inline __attribute__((always_inline))
float turbulenceContrast(thread float &value, float brightness,
                         float contrast) {
  value += brightness;
  value = ((value - 0.5f) * contrast) * 10.0f + 0.5f;
  return value;
}

static inline __attribute__((always_inline))
float turbulenceValid(float2 uv) {
  return ((step(0.0f, uv.x) * step(uv.x, 1.0f)) * step(0.0f, uv.y)) *
         step(uv.y, 1.0f);
}

static inline __attribute__((always_inline))
float2 turbulenceMirror(float2 uv) {
  return abs(qtTextMod(uv - float2(1.0f), float2(2.0f)) - float2(1.0f));
}

fragment float4 qtTextTurbulenceDisplacementFragment(
    TurbulenceVaryings input [[stage_in]],
    constant TurbulenceDisplacementUniforms &uniforms [[buffer(0)]],
    texture2d<float> noiseTexture [[texture(0)]],
    texture2d<float> inputTexture [[texture(1)]],
    sampler noiseSampler [[sampler(0)]],
    sampler inputSampler [[sampler(1)]]) {
  float4 packedNoise = noiseTexture.sample(noiseSampler, input.uv);
  float2 unpacked = turbulenceUnpackNoise(packedNoise);
  float first = unpacked.x;
  float second = unpacked.y;
  float firstParameter = unpacked.x;
  first = turbulenceContrast(firstParameter, uniforms.brightness,
                             uniforms.contrast);
  float secondParameter = second;
  second = turbulenceContrast(secondParameter, uniforms.brightness,
                              uniforms.contrast);
  float minimumSize =
      fast::min(uniforms.screenParams.x, uniforms.screenParams.y);
  first = ((first - 0.5f) * minimumSize) / uniforms.screenParams.x + 0.5f;
  second = ((second - 0.5f) * minimumSize) / uniforms.screenParams.y + 0.5f;
  float strength =
      fast::clamp(uniforms.scale.x,
                  0.00999999977648258209228515625f, 1.0f) *
      uniforms.range;
  float2 displaced = input.uv;
  if (uniforms.type < 0.5f) {
    displaced = float2(input.uv.x + (first - 0.5f) * strength,
                       input.uv.y + (second - 0.5f) * strength);
  } else if (uniforms.type < 1.5f) {
    displaced = float2(input.uv.x + (first - 0.5f) * strength, input.uv.y);
  } else if (uniforms.type < 2.5f) {
    displaced = float2(input.uv.x,
                       input.uv.y + (second - 0.5f) * strength);
  } else {
    displaced = float2(input.uv.x + (first - 0.5f) * strength,
                       input.uv.y + (second - 0.5f) * strength);
  }
  float valid = 1.0f;
  if (uniforms.motionTileType < 0.5f) {
    valid = turbulenceValid(displaced);
  } else if (uniforms.motionTileType < 1.5f) {
    displaced = fract(displaced);
  } else {
    displaced = turbulenceMirror(displaced);
  }
  return inputTexture.sample(inputSampler, displaced) * valid;
}
