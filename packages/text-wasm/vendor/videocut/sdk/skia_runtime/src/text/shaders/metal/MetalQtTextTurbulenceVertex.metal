#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct buffer_t
{
    float picture_scale;
};

struct main0_out
{
    float2 uv0 [[user(locn0)]];
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 a_position [[attribute(0)]];
    float2 a_texcoord0 [[attribute(1)]];
};

vertex main0_out main0(main0_in in [[stage_in]], constant buffer_t& buffer)
{
    main0_out out = {};
    out.gl_Position = float4(in.a_position, 1.0);
    out.uv0 = in.a_texcoord0;
    out.uv0 -= float2(0.5);
    out.uv0 /= float2(buffer.picture_scale);
    out.uv0 += float2(0.5);
    out.gl_Position.z = (out.gl_Position.z + out.gl_Position.w) * 0.5;
    return out;
}

