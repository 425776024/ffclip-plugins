
#include <metal_stdlib>
using namespace metal;

struct DistanceVertex {
  packed_float2 position;
  packed_float2 parabola;
  packed_float2 limits;
  float distanceScale;
  float distanceLimit;
};

struct DistanceVaryings {
  float4 position [[position]];
  float2 parabola [[user(parabola)]];
  float2 limits [[user(limits)]];
  float normalizedScale [[user(normalized_scale)]];
};

vertex DistanceVaryings textSdfDistanceVertex(
    const device DistanceVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  DistanceVertex input = vertices[vertexId];
  DistanceVaryings output;
  output.parabola = float2(input.parabola);
  output.limits = float2(input.limits);
  output.normalizedScale = input.distanceScale / input.distanceLimit;
  float2 clip = float2(2.0f * input.position.x - 1.0f,
                       2.0f * input.position.y - 1.0f);
  output.position = float4(clip, 0.5f, 1.0f);
  return output;
}

vertex DistanceVaryings textSdfMaterialDistanceVertex(
    const device DistanceVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  DistanceVertex input = vertices[vertexId];
  DistanceVaryings output;
  output.parabola = float2(input.parabola);
  output.limits = float2(input.limits);
  output.normalizedScale = input.distanceScale / input.distanceLimit;
  float2 clip = float2(2.0f * input.position.x - 1.0f,
                       1.0f - 2.0f * input.position.y);
  output.position = float4(clip, 0.5f, 1.0f);
  return output;
}

struct DistanceFragmentOutput {
  float4 color [[color(0)]];
  float depth [[depth(any)]];
};

static inline __attribute__((always_inline))
float2 pack16(thread const float& value) {
  float2 encoded = fract(float2(256.0f, 1.0f) * value);
  encoded -= encoded.xx * float2(0.0f, 0.00390625f);
  return encoded;
}

fragment DistanceFragmentOutput textSdfDistanceFragment(
    DistanceVaryings input [[stage_in]]) {
  float p = 0.5f - input.parabola.y;
  float q = (-0.5f) * input.parabola.x;
  float signX = input.parabola.x > 0.0f ? 1.0f : -1.0f;
  float squaredQ = (27.0f * q) * q;
  float cubedP = ((4.0f * p) * p) * p;
  float thirdP = (-p) * 0.3333333432674407958984375f;
  float distance;
  if (squaredQ >= -cubedP) {
    float rootCubeBase = 0.096225000917911529541015625f;
    float middle = signX *
                   pow((sqrt(abs(squaredQ + cubedP)) * rootCubeBase) +
                           (0.5f * abs(q)),
                       0.3333333432674407958984375f);
    float root = thirdP / middle + middle;
    root = fast::clamp(root, input.limits.x, input.limits.y);
    distance = length(float2(root, root * root) - input.parabola);
  } else {
    float ratioSquared = abs(squaredQ / cubedP);
    float ratio = sqrt(ratioSquared);
    float approximation =
        ratioSquared *
            ((0.01875323988497257232666015625f * ratio) -
             0.081791579723358154296875f) +
        ((0.3309875428676605224609375f * ratio) +
         1.73205077648162841796875f);
    float rootScale = sqrt(abs(thirdP));
    float root0 = (signX * rootScale) * approximation;
    float deltaRoot =
        signX * sqrt((((-0.75f) * root0) * root0) - p);
    float root1 = ((-0.5f) * root0) - deltaRoot;
    root0 = fast::clamp(root0, input.limits.x, input.limits.y);
    root1 = fast::clamp(root1, input.limits.x, input.limits.y);
    float distance0 =
        length(float2(root0, root0 * root0) - input.parabola);
    float distance1 =
        length(float2(root1, root1 * root1) - input.parabola);
    distance = fast::min(distance0, distance1);
  }
  float normalizedDistance =
      fast::min(distance * input.normalizedScale, 1.0f);
  float color = 0.5f - (0.5f * normalizedDistance);
  float parameter = color;
  DistanceFragmentOutput output;
  output.color = float4(pack16(parameter), 0.0f, 1.0f);
  output.depth = normalizedDistance;
  return output;
}

struct ShapeVertex {
  packed_float2 position;
  packed_float2 parabola;
};

struct ShapeVaryings {
  float4 position [[position]];
  float2 parabola [[user(parabola)]];
};

vertex ShapeVaryings textSdfShapeVertex(
    const device ShapeVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  ShapeVertex input = vertices[vertexId];
  ShapeVaryings output;
  output.parabola = float2(input.parabola);
  float2 clip = float2(2.0f * input.position.x - 1.0f,
                       2.0f * input.position.y - 1.0f);
  output.position = float4(clip, 0.5f, 1.0f);
  return output;
}

vertex ShapeVaryings textSdfMaterialShapeVertex(
    const device ShapeVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  ShapeVertex input = vertices[vertexId];
  ShapeVaryings output;
  output.parabola = float2(input.parabola);
  float2 clip = float2(2.0f * input.position.x - 1.0f,
                       1.0f - 2.0f * input.position.y);
  output.position = float4(clip, 0.5f, 1.0f);
  return output;
}

fragment float4 textSdfShapeFragment(ShapeVaryings input [[stage_in]]) {
  if (!(input.parabola.x * input.parabola.x < input.parabola.y))
    discard_fragment();
  return float4(1.0f);
}

vertex float4 textSdfInverseVertex(uint vertexId [[vertex_id]]) {
  constexpr float2 positions[6] = {
      float2(-1.0f, -1.0f), float2(1.0f, -1.0f),
      float2(1.0f, 1.0f), float2(-1.0f, -1.0f),
      float2(1.0f, 1.0f), float2(-1.0f, 1.0f)};
  return float4(positions[vertexId], 0.0f, 1.0f);
}

fragment float4 textSdfInverseFragment() { return float4(1.0f); }
