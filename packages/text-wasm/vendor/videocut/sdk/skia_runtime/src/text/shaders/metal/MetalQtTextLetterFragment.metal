#define AE_FLIP_PATCH 1

#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;
float2 Flip_v(float flip, float2 uv) { if(flip > 0.5) uv.y = 1.0 - uv.y; return uv; }
float3 Flip_v(float flip, float3 uv) { if(flip > 0.5) uv.y = 1.0 - uv.y; return uv; }
float4 Flip_v(float flip, float4 uv) { if(flip > 0.5) uv.y = 1.0 - uv.y; return uv; }


struct main0_out
{
    float4 FragColor [[color(0)]];
};

struct main0_in
{
    float4 vInstanceColor;
    float4 vSDFTexcoord;
    float4 vSDFTexcoordMinMax;
    float4 vBlurSDFTexcoordMinMax;
    float2 vSmoothBold;
    float2 vGradientTexcoord;
    float2 vTextureTexcoord;
};

static inline __attribute__((always_inline))
float unpack16(thread const float2& color)
{
    return dot(color, float2(0.00390625, 1.0));
}

static inline __attribute__((always_inline))
float linearstep(thread const float& edge0, thread const float& edge1, thread const float& x)
{
    return fast::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
}

#ifdef _VOLUME
static inline __attribute__((always_inline))
float2x2 spvInverse2x2(float2x2 m)
{
    float2x2 adj;

    adj[0][0] =  m[1][1];
    adj[0][1] = -m[0][1];

    adj[1][0] = -m[1][0];
    adj[1][1] =  m[0][0];

    float det = (adj[0][0] * m[0][0]) + (adj[0][1] * m[1][0]);

    return (det != 0.0f) ? (adj * (1.0f / det)) : m;
}

static inline __attribute__((always_inline))
float2 get_sdf_grad(thread const float2& sample_uv, thread const float& SDF0, constant float2& u_mainTexSize, float _MainTex_fLiP, texture2d<float> _MainTex, sampler _MainTexSmplr)
{
    float2 duv = float2(1.0 / u_mainTexSize.x, 1.0 / u_mainTexSize.y);
    float2 param = _MainTex.sample(_MainTexSmplr, Flip_v(_MainTex_fLiP, (sample_uv + float2(duv.x, 0.0)))).xy;
    float SDFX = unpack16(param);
    float2 param_1 = _MainTex.sample(_MainTexSmplr, Flip_v(_MainTex_fLiP, (sample_uv + float2(0.0, duv.y)))).xy;
    float SDFY = unpack16(param_1);
    float _dFdx = SDFX - SDF0;
    float _dFdy = SDFY - SDF0;
    float2 dVec = float2(_dFdx, _dFdy);
    return dVec;
}

static inline __attribute__((always_inline))
float2 get_thickness_proj(thread const float& _distance, thread const float& boundary, constant float2& u_mainTexSize, float _MainTex_fLiP, texture2d<float> _MainTex, sampler _MainTexSmplr,thread const float& thicknessAngle, thread float4& vSDFTexcoord,thread const float& thicknessLength, thread float4& vSDFTexcoordMinMax)
{
    float2 sampleDir = float2(cos(thicknessAngle), sin(thicknessAngle));
    int max_step = 15;
    float2 targetUV = float2(-1.0);
    float3 targetPar = float3(0.0);
    float2 curUV0 = vSDFTexcoord.xy;
    float2 curU = vSDFTexcoord.xy;
    float2 deltaUV = float2(0.0);
    float curDistance = 0.0;
    float2 param = _MainTex.sample(_MainTexSmplr, Flip_v(_MainTex_fLiP, curUV0)).xy;
    float curDistance0 = unpack16(param);
    float curDistance1 = 0.0;
    float targetDistance = 0.0;
    int findPoint = 0;
    float2 dFduv = float2(0.0);
    float2 dFdUV = float2(0.0);
    float dFdL = 0.0;
    float dF0 = 0.0;
    float dF1 = 0.0;
    float dF = 0.0;
    float dL = 0.0;
    float2 duv = float2(0.0);
    float2 dUV = float2(0.0);
    float deltaL = 0.0;
    float2x2 JuvUV = float2x2(float2(u_mainTexSize.x, 0.0), float2(0.0, u_mainTexSize.y));
    float2x2 JUVuv = spvInverse2x2(JuvUV);
    float2 delta_step_UV = (sampleDir * thicknessLength) / float2(float(max_step - 1));
    float2 delta_step_uv = JUVuv * delta_step_UV;
    float judgment0 = 0.0;
    float judgment1 = 0.0;
    float judgment2 = 0.0;
    float judgment3 = 0.0;
    float judgment4 = 0.0;
    float2 ret = float2(-1.0, 1000.0);
    for (int i = 0; i < (max_step - 1); i++)
    {
        curU = vSDFTexcoord.xy + (delta_step_uv * float(i + 1));
        curU.x = fast::clamp(curU.x, vSDFTexcoordMinMax.x, vSDFTexcoordMinMax.z);
        curU.y = fast::clamp(curU.y, vSDFTexcoordMinMax.y, vSDFTexcoordMinMax.w);
        float2 param_1 = _MainTex.sample(_MainTexSmplr, Flip_v(_MainTex_fLiP, curU)).xy;
        curDistance1 = unpack16(param_1);
        dF0 = curDistance0 - boundary;
        dF1 = curDistance1 - boundary;
        float2 param_2 = curUV0;
        float param_3 = curDistance0;
        dFdUV = get_sdf_grad(param_2, param_3, u_mainTexSize, _MainTex_fLiP, _MainTex, _MainTexSmplr);
        dFdL = dot(fast::normalize(delta_step_UV), dFdUV);
        dF = boundary - curDistance0;
        dL = dF / dFdL;
        if (abs(dL) < length(delta_step_UV))
        {
            dUV = fast::normalize(delta_step_UV) * dL;
            duv = JUVuv * dUV;
            targetUV = curUV0 + duv;
            float2 param_4 = _MainTex.sample(_MainTexSmplr, Flip_v(_MainTex_fLiP, targetUV)).xy;
            targetDistance = unpack16(param_4);
            curDistance = targetDistance;
            duv = targetUV - vSDFTexcoord.xy;
            dUV = JuvUV * duv;
            deltaL = length(dUV);
            judgment0 = 0.001 - abs(curDistance - boundary);
            judgment1 = dot(dUV, sampleDir);
            judgment2 = thicknessLength - deltaL;
            judgment3 = ret.y - judgment2;

            judgment4 = fast::min(judgment0, judgment1);
            judgment4 = fast::min(judgment4, judgment2);
            judgment4 = fast::min(judgment4, judgment3);
            if (judgment4 > 0.0)
            {
                ret.x = curDistance;
                ret.y = thicknessLength - deltaL;
            }
        }
        if ((dF0 * dF1) < 0.0)
        {
            duv = (curUV0 - (delta_step_uv * (dF0 / (dF1 - dF0)))) - vSDFTexcoord.xy;
            targetUV = curUV0 + duv;
            float2 param_5 = _MainTex.sample(_MainTexSmplr, Flip_v(_MainTex_fLiP, targetUV)).xy;
            curDistance = unpack16(param_5);
            dUV = JuvUV * duv;
            deltaL = length(dUV);
            ret.x = curDistance;
            ret.y = thicknessLength - deltaL;
            break;
        }
        curUV0 = curU;
        curDistance0 = curDistance1;
    }
    return ret;
}
#endif

fragment main0_out main0(main0_in in [[stage_in]], constant int& u_textureBlend [[buffer(7)]], constant float& u_extraSmooth [[buffer(0)]], constant float2& u_strokeStartEnd [[buffer(1)]], constant float& u_minSDF [[buffer(2)]], constant float& u_extraWidth [[buffer(3)]], constant float4& u_color [[buffer(5)]], constant float& u_textureAlpha [[buffer(6)]], constant float2& u_mainTexSize [[buffer(8)]], constant float& u_smoothIntensity [[buffer(9)]], constant float2& u_innerShadowOffset [[buffer(10)]], constant float2& u_blurTexSize [[buffer(11)]],constant float2& u_thicknessInfo [[buffer(12)]], texture2d<float> _MainTex [[texture(0)]], texture2d<float> u_gradientTexture [[texture(1)]], texture2d<float> u_texture [[texture(2)]], texture2d<float> _BlurTex [[texture(3)]], sampler _MainTexSmplr [[sampler(0)]], sampler u_gradientTextureSmplr [[sampler(1)]], sampler u_textureSmplr [[sampler(2)]], sampler _BlurTexSmplr [[sampler(3)]], constant float4& u_is_texture_0_flip_)
{
    main0_out out = {};
    float4 sdfColor = float4(0.0,0.0,0.0,0.0);
    sdfColor.rg = _MainTex.sample(_MainTexSmplr, Flip_v(u_is_texture_0_flip_.x, in.vSDFTexcoord.xy)).rg;
    float2 param = sdfColor.xy;
    float distanceOrg = unpack16(param);
    float _distance = distanceOrg;
#ifdef _INNER_SHADOW
    float distanceOrigin = _distance;
    sdfColor = _MainTex.sample(_MainTexSmplr, Flip_v(u_is_texture_0_flip_.x, (in.vSDFTexcoord.xy - u_innerShadowOffset)));
    float2 param_1 = sdfColor.xy;
    _distance = unpack16(param_1);
#endif
#ifdef _SHADOW
    sdfColor.ba = _BlurTex.sample(_BlurTexSmplr, Flip_v(u_is_texture_0_flip_.w, in.vSDFTexcoord.zw)).rg;
    float4 sampleRadius = float4(float2(25.0 , 25.0) / u_mainTexSize, float2(25.0 , 25.0) / u_blurTexSize);
    float4 vSDFTexcoordU = in.vSDFTexcoord + float4(float2(0.0,sampleRadius.y), float2(0.0,sampleRadius.w));
    float4 vSDFTexcoordB = in.vSDFTexcoord - float4(float2(0.0,sampleRadius.y), float2(0.0,sampleRadius.w));
    float4 vSDFTexcoordL = in.vSDFTexcoord - float4(float2(sampleRadius.x,0.0), float2(sampleRadius.z,0.0));
    float4 vSDFTexcoordR = in.vSDFTexcoord + float4(float2(sampleRadius.x,0.0), float2(sampleRadius.z,0.0));
    float4 sdfColorU = float4(0.0,0.0,0.0,0.0);
    float4 sdfColorB = float4(0.0,0.0,0.0,0.0);
    float4 sdfColorL = float4(0.0,0.0,0.0,0.0);
    float4 sdfColorR = float4(0.0,0.0,0.0,0.0);
    if(vSDFTexcoordU.y < in.vSDFTexcoordMinMax.w)
        sdfColorU.rg = _MainTex.sample(_MainTexSmplr, Flip_v(u_is_texture_0_flip_.x, vSDFTexcoordU.xy)).rg;
    if(vSDFTexcoordB.y > in.vSDFTexcoordMinMax.y)
        sdfColorB.rg = _MainTex.sample(_MainTexSmplr, Flip_v(u_is_texture_0_flip_.x, vSDFTexcoordB.xy)).rg;
    if(vSDFTexcoordL.x > in.vSDFTexcoordMinMax.x)
        sdfColorL.rg = _MainTex.sample(_MainTexSmplr, Flip_v(u_is_texture_0_flip_.x, vSDFTexcoordL.xy)).rg;
    if(vSDFTexcoordR.x < in.vSDFTexcoordMinMax.z)
        sdfColorR.rg = _MainTex.sample(_MainTexSmplr, Flip_v(u_is_texture_0_flip_.x, vSDFTexcoordR.xy)).rg;

    if(vSDFTexcoordU.w < in.vBlurSDFTexcoordMinMax.w)
        sdfColorU.ba = _BlurTex.sample(_BlurTexSmplr, Flip_v(u_is_texture_0_flip_.w, vSDFTexcoordU.zw)).rg;
    if(vSDFTexcoordB.w > in.vBlurSDFTexcoordMinMax.y)
        sdfColorB.ba = _BlurTex.sample(_BlurTexSmplr, Flip_v(u_is_texture_0_flip_.w, vSDFTexcoordB.zw)).rg;
    if(vSDFTexcoordL.z > in.vBlurSDFTexcoordMinMax.x)
        sdfColorL.ba = _BlurTex.sample(_BlurTexSmplr, Flip_v(u_is_texture_0_flip_.w, vSDFTexcoordL.zw)).rg;
    if(vSDFTexcoordR.z < in.vBlurSDFTexcoordMinMax.z)
        sdfColorR.ba = _BlurTex.sample(_BlurTexSmplr, Flip_v(u_is_texture_0_flip_.w, vSDFTexcoordR.zw)).rg;

    float distanceU = unpack16(sdfColorU.xy);
    float distanceB = unpack16(sdfColorB.xy);
    float distanceL = unpack16(sdfColorL.xy);
    float distanceR = unpack16(sdfColorR.xy);

    float2 param_2 = sdfColor.zw;
    float distanceSmoothU = unpack16(sdfColorU.ba);
    float distanceSmoothB = unpack16(sdfColorB.ba);
    float distanceSmoothL = unpack16(sdfColorL.ba);
    float distanceSmoothR = unpack16(sdfColorR.ba);

    float distanceSmooth = unpack16(param_2);
    float param_3 = 0.0;
    float param_4 = 0.100000001490116119384765625;
    float param_5 = u_extraSmooth;
    _distance = mix(_distance, distanceSmooth, linearstep(param_3, param_4, param_5));
    distanceU = mix(distanceU, distanceSmoothU, linearstep(param_3, param_4, param_5));
    distanceB = mix(distanceB, distanceSmoothB, linearstep(param_3, param_4, param_5));
    distanceL = mix(distanceL, distanceSmoothL, linearstep(param_3, param_4, param_5));
    distanceR = mix(distanceR, distanceSmoothR, linearstep(param_3, param_4, param_5));
    float distanceRound = (distanceU + distanceB + distanceL+ distanceR) / 4.0;
#endif
    float smoothDefault = fwidth(_distance) * 0.60000002384185791015625;
    float smoothing = smoothDefault + u_extraSmooth;
    float boundary = 0.5 - in.vSmoothBold.y;


#if !defined(_OUTLINE)  && !defined(_SHADOW)
        float boundary1_1 = fast::clamp(boundary - u_extraWidth, u_minSDF, 1.0);
        float e1_1 = fast::clamp(boundary1_1 - smoothing, u_minSDF, 1.0);
        float e2_1 = fast::clamp(boundary1_1 + smoothing, u_minSDF, 1.0);
        float param_12 = e1_1;
        float param_13 = e2_1;
        float param_14 = _distance;
        float maskOrg = linearstep(param_12, param_13, param_14);
        float mask = maskOrg;
#endif

#ifdef _OUTLINE
        float boundary1 = fast::clamp(boundary - u_strokeStartEnd.x, u_minSDF, 1.0);
        float boundary2 = fast::clamp(boundary - u_strokeStartEnd.y, u_minSDF, 1.0);
        float thicknessBoundary = boundary2;
#ifdef _VOLUME
        float e1 = fast::clamp(boundary1 - 2.0 * smoothing, u_minSDF, 1.0);
        float e2 = fast::clamp(boundary1 + 0.0 * smoothing, u_minSDF, 1.0);
        float e3 = fast::clamp(boundary2 - 2.0 * smoothing, u_minSDF, 1.0);
        float e4 = fast::clamp(boundary2 + 0.0 * smoothing, u_minSDF, 1.0);
#else
        float e1 = fast::clamp(boundary1 - smoothing, u_minSDF, 1.0);
        float e2 = fast::clamp(boundary1 + smoothing, u_minSDF, 1.0);
        float e3 = fast::clamp(boundary2 - smoothing, u_minSDF, 1.0);
        float e4 = fast::clamp(boundary2 + smoothing, u_minSDF, 1.0);
#endif
        float maskOrg = linearstep(e3, e4, _distance) * (1.0 - linearstep(e1, e2, _distance));
        float mask = maskOrg;
#endif

#ifdef _SHADOW
        float boundary1_1 = fast::clamp(boundary - u_extraWidth, u_minSDF, 1.0);
#ifdef _VOLUME
        float e1_1 = fast::clamp(boundary1_1 - (2.0 * smoothing), 0.0, 1.0);
        float e2_1 = fast::clamp(boundary1_1 + (0.0 * smoothing), 0.0, 1.0);
#else
        float e1_1 = fast::clamp(boundary1_1 - smoothing, 0.0, 1.0);
        float e2_1 = fast::clamp(boundary1_1 + smoothing, 0.0, 1.0);
#endif
        float param_12 = e1_1;
        float param_13 = e2_1;
        float param_14 = _distance;
        float maskOrg = linearstep(param_12, param_13, param_14);
        float mask = maskOrg;
        float maskRound = linearstep(param_12, param_13, distanceRound);
        float ratio = 1.0 - fast::clamp(u_smoothIntensity * 1.25, 0.0, 1.0);
        mask = ratio * mask + (1.0 - ratio) * maskRound;
        float thicknessBoundary = boundary1_1;

        float xVar = 1.0 - mask;
        float xEdgeTo0 = 0.99;
        float xEdgeTo1 = 0.35;
        float xRadius = xEdgeTo0 -  xEdgeTo1;
        float invSigma = 2.5;
        float ratioX = fast::clamp(xVar - xEdgeTo1 ,0 ,xRadius) * invSigma;
        float x0Value = exp(-xRadius * xRadius * invSigma * invSigma);
        mask = exp(-ratioX * ratioX);
        mask = linearstep(x0Value,1.0,mask);
#endif

#ifdef _VOLUME
    if (maskOrg < 0.99)
    {
        float2 dUVdx = dfdx(in.vSDFTexcoord.xy);
        float2 dUVdy = dfdy(in.vSDFTexcoord.xy);
        float2x2 JXYuv = float2x2(float2(dUVdx.x, dUVdx.y), float2(dUVdy.x, dUVdy.y));
        float2x2 JuvXY = spvInverse2x2(JXYuv);
        float2x2 JuvUV = float2x2(float2(u_mainTexSize.x, 0.0), float2(0.0, u_mainTexSize.y));
        float2x2 JUVuv = spvInverse2x2(JuvUV);
        float2x2 JXYUV = JuvUV * JXYuv;
        float2x2 JUVXY = spvInverse2x2(JXYUV);
        float2 sampleDir = float2(cos(u_thicknessInfo.x), sin(u_thicknessInfo.x));
        float2 dXY = JUVXY * sampleDir;
        float UVXYRatio = 1.0 / length(dXY);
        float param_18 = distanceOrg;
        float param_19 = thicknessBoundary;
        float param_19_0 = u_thicknessInfo.x;
        float param_19_1 = u_thicknessInfo.y;
        float2 thickInfo = get_thickness_proj(param_18, param_19, u_mainTexSize, u_is_texture_0_flip_.x, _MainTex, _MainTexSmplr, param_19_0, in.vSDFTexcoord, param_19_1, in.vSDFTexcoordMinMax);
        float param_20 = 0.0;
        float param_21 = 2.0 * UVXYRatio;
        float param_22 = thickInfo.y;
        float tSmooth = linearstep(param_20, param_21, param_22);
        float param_23 = u_thicknessInfo.y;
        float param_24 = u_thicknessInfo.y;
        float param_25 = thickInfo.y;
        float tEdge = linearstep(param_23, param_24, param_25);
        float tMask = tSmooth * (1.0 - tEdge);
        mask = fast::clamp(maskOrg + tMask, 0.0, 1.0);
    }
#endif

#ifdef _INNER_SHADOW
    float param_15 = boundary - smoothDefault;
    float param_16 = boundary + smoothDefault;
    float param_17 = distanceOrigin;
    float maskInner = linearstep(param_15, param_16, param_17);
    mask *= maskInner;
#endif
    mask *= in.vInstanceColor.w;
    float4 outColor = u_color;
#ifdef _GRADIENT_TEXTURE
    outColor = u_gradientTexture.sample(u_gradientTextureSmplr, Flip_v(u_is_texture_0_flip_.y, in.vGradientTexcoord));
#endif
    float3 _233 = outColor.xyz * in.vInstanceColor.xyz;
    outColor = float4(_233.x, _233.y, _233.z, outColor.w);
#ifdef _TEXTURE
    float4 tColor = u_texture.sample(u_textureSmplr, Flip_v(u_is_texture_0_flip_.z, in.vTextureTexcoord));
    tColor *= u_textureAlpha;
    outColor = (outColor * (float(u_textureBlend) * (1.0 - tColor.w))) + tColor;
#endif
    out.FragColor = outColor * mask;
    return out;
}
