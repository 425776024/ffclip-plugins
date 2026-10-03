
#include <metal_stdlib>
using namespace metal;

struct VatVertex {
  packed_float4 clipPosition;
  packed_float2 textureCoordinate;
};

struct VatVaryings {
  float4 position [[position]];
  float2 textureCoordinate [[user(texture_coordinate)]];
};

vertex VatVaryings textVatMeshVertex(
    const device VatVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  VatVaryings output;
  output.position = float4(vertices[vertexId].clipPosition);
  output.textureCoordinate = float2(vertices[vertexId].textureCoordinate);
  return output;
}

fragment float4 textVatMeshFragment(
    VatVaryings input [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]],
    constant float &opacity [[buffer(0)]],
    constant uint &flipSourceV [[buffer(1)]]) {
  const float2 logicalUv = input.textureCoordinate;
  const float cut = step(0.0f, logicalUv.x) * step(logicalUv.x, 1.0f) *
                    step(0.0f, logicalUv.y) * step(logicalUv.y, 1.0f);
  const float sampledV = flipSourceV != 0u ? 1.0f - logicalUv.y
                                           : logicalUv.y;
  return source.sample(sourceSampler, float2(logicalUv.x, sampledV)) *
         (cut * opacity);
}
