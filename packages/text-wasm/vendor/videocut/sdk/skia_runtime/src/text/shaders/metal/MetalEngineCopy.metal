
#include <metal_stdlib>
using namespace metal;

struct EngineCopyVertex {
  packed_float2 position;
  packed_float2 uv;
};

struct EngineCopyVaryings {
  float4 position [[position]];
  float2 uv [[user(texturecoord)]];
};

vertex EngineCopyVaryings qtTextEngineCopyVertex(
    const device EngineCopyVertex *vertices [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  EngineCopyVertex input = vertices[vertexId];
  EngineCopyVaryings output;
  output.position = float4(float2(input.position), 0.0f, 1.0f);
  output.uv = float2(input.uv);
  return output;
}

fragment float4 qtTextEngineCopyFragment(
    EngineCopyVaryings input [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]]) {
  return source.sample(sourceSampler, input.uv);
}
