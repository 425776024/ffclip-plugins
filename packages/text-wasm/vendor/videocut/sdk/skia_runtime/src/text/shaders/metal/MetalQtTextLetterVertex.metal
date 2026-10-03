#define AE_FLIP_PATCH 1

#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float4 vInstanceColor;
    float4 vSDFTexcoord;
    float4 vSDFTexcoordMinMax;
    float4 vBlurSDFTexcoordMinMax;
    float2 vSmoothBold;
    float2 vGradientTexcoord;
    float2 vTextureTexcoord;
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 aPosition [[attribute(0)]];
    float4 aInstanceColor [[attribute(1)]];
    float4 aLineRect[[attribute(2)]];
    float4 aSDFTexcoord [[attribute(3)]];
    float4 aSDFTexcoordMinMax [[attribute(4)]];
    float4 aBlurSDFTexcoordMinMax [[attribute(5)]];
    float4 aSmoothBoldIndex [[attribute(6)]];
};

static inline __attribute__((always_inline))
float2 getOffset(thread const float2& offSetInfo)
{
    float2 offset = float2(offSetInfo.x * cos(offSetInfo.y), offSetInfo.x * sin(offSetInfo.y));
    return offset;
}


#ifdef _BLOOM
vertex main0_out main0(main0_in in [[stage_in]], constant int& u_multiInstanceColor [[buffer(0)]], constant int& u_gradientMode [[buffer(1)]], constant float2& u_offsetInfo [[buffer(2)]], constant float2& u_BloomPixelRatio [[buffer(4)]], constant float& u_gradientScale [[buffer(7)]], constant float& u_gradientPixelRatio [[buffer(8)]], constant float4& u_gradientTextRect [[buffer(9)]], constant float& u_gradientAngle [[buffer(10)]], constant float2& u_texcoordScale [[buffer(11)]], constant float2& u_textureGrid [[buffer(12)]], constant float2& u_textureScale [[buffer(13)]], constant float& u_textureRotate [[buffer(14)]],constant float& u_alignDistance [[buffer(15)]])
#else
vertex main0_out main0(main0_in in [[stage_in]], constant int& u_multiInstanceColor [[buffer(0)]], constant int& u_gradientMode [[buffer(1)]], constant float2& u_offsetInfo [[buffer(2)]], constant float4x4& u_MVP [[buffer(5)]], constant float& u_gradientScale [[buffer(7)]], constant float& u_gradientPixelRatio [[buffer(8)]], constant float4& u_gradientTextRect [[buffer(9)]], constant float& u_gradientAngle [[buffer(10)]], constant float2& u_texcoordScale [[buffer(11)]], constant float2& u_textureGrid [[buffer(12)]], constant float2& u_textureScale [[buffer(13)]], constant float& u_textureRotate [[buffer(14)]],constant float& u_alignDistance [[buffer(15)]])
#endif
{
    main0_out out = {};
    float3 position = in.aPosition;
    float2 param = float2(u_offsetInfo.x, u_offsetInfo.y + in.aSmoothBoldIndex.w);
    float2 offset = getOffset(param);
    float2 _62 = position.xy + offset;
    position = float3(_62.x, _62.y, position.z);
#ifdef _BLOOM
    out.gl_Position = float4(position.xy * u_BloomPixelRatio, position.z, 1.0);
#else
    out.gl_Position = u_MVP * float4(position, 1.0);
#endif
    out.vInstanceColor = in.aInstanceColor;
    if (u_multiInstanceColor == 0)
    {
        out.vInstanceColor = float4(float3(1.0).x, float3(1.0).y, float3(1.0).z, out.vInstanceColor.w);
    }
    out.vSDFTexcoord = in.aSDFTexcoord;
    out.vSDFTexcoordMinMax = in.aSDFTexcoordMinMax;
    out.vBlurSDFTexcoordMinMax = in.aBlurSDFTexcoordMinMax;
    out.vSmoothBold = in.aSmoothBoldIndex.xy;
#ifdef _GRADIENT_TEXTURE
    if (u_gradientMode == 0)
    {
        out.vGradientTexcoord = (in.aSDFTexcoord.xy - in.aSDFTexcoordMinMax.xy) / (in.aSDFTexcoordMinMax.zw - in.aSDFTexcoordMinMax.xy);
        out.vGradientTexcoord = ((out.vGradientTexcoord - float2(0.5)) / float2(u_gradientScale)) + float2(0.5);
    }
    else if (u_gradientMode == 1)
    {
        float2 pos = in.aPosition.xy * u_gradientPixelRatio - float2(0.0, u_alignDistance);
        float isHorizontal = step(u_gradientTextRect.x, 0.5);
        pos += mix(float2(-in.aLineRect.x, u_gradientTextRect.w * 0.5),
                   float2(u_gradientTextRect.z * 0.5, -in.aLineRect.y),
                   isHorizontal);
        float2 lineGradientSize = mix(float2(in.aLineRect.z, u_gradientTextRect.w),
                                      float2(u_gradientTextRect.z, in.aLineRect.w),
                                      isHorizontal);
        out.vGradientTexcoord = pos / lineGradientSize;
    }
    else
    {
        float2 pos = in.aPosition.xy * u_gradientPixelRatio - float2(0.0, u_alignDistance);
        out.vGradientTexcoord = ( pos + (u_gradientTextRect.zw * 0.5)) / u_gradientTextRect.zw;
    }
    out.vGradientTexcoord -= float2(0.5);
    out.vGradientTexcoord = float2((out.vGradientTexcoord.x * cos(u_gradientAngle)) + (out.vGradientTexcoord.y * sin(u_gradientAngle)), (out.vGradientTexcoord.x * (-sin(u_gradientAngle))) + (out.vGradientTexcoord.y * cos(u_gradientAngle)));
    out.vGradientTexcoord += float2(0.5);
#endif
#ifdef _TEXTURE
    out.vTextureTexcoord = (in.aSDFTexcoord.xy - ((in.aSDFTexcoordMinMax.xy + in.aSDFTexcoordMinMax.zw) * 0.5)) * u_texcoordScale;
    float centerX = ((-0.5) + (0.5 / u_textureGrid.x)) + fract(in.aSmoothBoldIndex.z / u_textureGrid.x);
    out.vTextureTexcoord *= (float2(1.0) / u_textureGrid);
    out.vTextureTexcoord *= (float2(1.0) / u_textureScale);
    out.vTextureTexcoord = float2((out.vTextureTexcoord.x * cos(u_textureRotate)) + (out.vTextureTexcoord.y * sin(u_textureRotate)), (out.vTextureTexcoord.x * (-sin(u_textureRotate))) + (out.vTextureTexcoord.y * cos(u_textureRotate)));
    out.vTextureTexcoord.x += centerX;
    out.vTextureTexcoord += float2(0.5);
    out.vTextureTexcoord.y = 1.0 - out.vTextureTexcoord.y;
#endif
    out.gl_Position.z = (out.gl_Position.z + out.gl_Position.w) * 0.5;
    return out;
}
