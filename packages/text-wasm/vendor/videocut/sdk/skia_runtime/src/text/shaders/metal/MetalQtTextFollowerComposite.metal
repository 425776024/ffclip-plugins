
#include <metal_stdlib>
using namespace metal;

struct FollowerVertex {
  packed_float2 position;
  packed_float2 texcoord;
};

struct FollowerVaryings {
  float4 position [[position]];
  float2 texcoord;
};

vertex FollowerVaryings main0(
    const device FollowerVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  const FollowerVertex input = vertices[vertexId];
  FollowerVaryings output;
  output.position = float4(float2(input.position), 0.5f, 1.0f);
  output.texcoord = float2(input.texcoord);
  return output;
}

fragment float4 followerFragment(
    FollowerVaryings input [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]],
    constant float& opacity [[buffer(0)]]) {
  return source.sample(sourceSampler, input.texcoord) * min(opacity, 1.0f);
}
