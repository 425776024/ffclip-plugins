#define _RENDER_GROUP 1
#define AE_FLIP_PATCH 1

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float2 vTexcoord;
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 aPosition [[attribute(0)]];
    float4 aTexcoord [[attribute(1)]];
};

#if defined(_RENDER_GROUP) && defined(_BLOOM)
vertex main0_out main0(main0_in in [[stage_in]], constant float4x4& u_MVP [[buffer(0)]], constant float4x4& u_CustomMat [[buffer(1)]], constant float2& u_BloomPixelRatio [[buffer(2)]])
#else
vertex main0_out main0(main0_in in [[stage_in]], constant float4x4& u_MVP [[buffer(0)]], constant float4x4& u_CustomMat [[buffer(1)]])
#endif
{
    main0_out out = {};
#ifdef _RENDER_GROUP


    #ifdef _BLOOM
        out.gl_Position = u_CustomMat * float4(in.aPosition, 1.0);
        out.gl_Position.xy *= u_BloomPixelRatio;
    #else
        out.gl_Position = u_MVP * u_CustomMat * float4(in.aPosition, 1.0);
    #endif
    out.gl_Position.z = (out.gl_Position.z + out.gl_Position.w) * 0.5;
#else
    out.gl_Position = float4(in.aPosition, 1.0);
#endif
    out.vTexcoord = in.aTexcoord.xy;
    return out;
}
