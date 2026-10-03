#define _RENDER_GROUP 1
#define AE_FLIP_PATCH 1

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;
float2 Flip_v(float flip, float2 uv) { if(flip > 0.5) uv.y = 1.0 - uv.y; return uv; }
float3 Flip_v(float flip, float3 uv) { if(flip > 0.5) uv.y = 1.0 - uv.y; return uv; }
float4 Flip_v(float flip, float4 uv) { if(flip > 0.5) uv.y = 1.0 - uv.y; return uv; }


struct buffer_t
{
    float u_alpha;
};

struct main0_out
{
    float4 FragColor [[color(0)]];
};

struct main0_in
{
    float2 vTexcoord;
};

fragment main0_out main0(main0_in in [[stage_in]], constant buffer_t& buffer, texture2d<float> _MainTex [[texture(0)]], sampler _MainTexSmplr [[sampler(0)]], constant float4& u_is_texture_0_flip_)
{
    main0_out out = {};
    float alpha = buffer.u_alpha;
    float4 outColor = _MainTex.sample(_MainTexSmplr, Flip_v(u_is_texture_0_flip_.x, in.vTexcoord));
    out.FragColor = outColor * alpha;
    return out;
}
