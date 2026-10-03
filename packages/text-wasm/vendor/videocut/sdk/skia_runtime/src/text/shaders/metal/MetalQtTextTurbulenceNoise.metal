#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;


template<typename Tx, typename Ty>
inline Tx mod(Tx x, Ty y)
{
    return x - y * floor(x / y);
}

struct buffer_t
{
    float u_Cycle;
    float4 u_ScreenParams;
    float2 u_Offset;
    float u_Rotate;
    float2 u_Scale;
    float u_type;
    float u_Complexity;
    float u_Evolution;
    float u_SubImpact;
    float u_SubScale;
    float u_SubRotate;
    float2 u_SubOffset;
};

struct main0_out
{
    float4 o_fragColor [[color(0)]];
};

struct main0_in
{
    float2 uv0 [[user(locn0)]];
};

static inline __attribute__((always_inline))
float2 _f3(thread float2& _p0, thread const float& _p1, constant float4& u_ScreenParams)
{
    _p0.y *= (u_ScreenParams.y / u_ScreenParams.x);
    float _361 = sin(_p1);
    float _364 = cos(_p1);
    _p0 -= float2(0.5);
    _p0 = float2x2(float2(_364, _361), float2(-_361, _364)) * _p0;
    _p0 += float2(0.5);
    _p0.y *= (u_ScreenParams.x / u_ScreenParams.y);
    return _p0;
}

static inline __attribute__((always_inline))
float _f0(thread const float2& _p0)
{
    float2 _49 = fract(_p0 * 1324.5179443359375);
    float2 _t0 = _49 + float2(dot(_49, _49.yx + float2(22.5410003662109375)));
    return fract((_t0.x + _t0.y) * _t0.y);
}

static inline __attribute__((always_inline))
float2 _f2(thread const float2& _p0, thread const float2& _p1, constant float& u_Cycle)
{
    float2 param = _p0;
    float _107 = _f0(param);
    float _116 = (_p1.x * 0.25) + _107;
    float _119 = floor(_116);
    float _t8 = _119;
    float _t9 = _119 + 1.0;
    bool _128 = u_Cycle >= 2.0;
    if (_128)
    {
        _t8 = floor(mod(_116, u_Cycle));
        _t9 = floor(mod(_116 + 1.0, u_Cycle));
    }
    float2 _153 = fract(_p0 * ((34.532001495361328125 + (_t8 * 2.0)) + (_p1.y * 0.80309998989105224609375)));
    float2 _163 = _153 + float2(dot(_153, _153.yx + float2(15.4340000152587890625)));
    float2 _167 = _163.yx;
    float2 _176 = float2(_107);
    float2 _t11 = fract((((_163 + _167) + float2(0.5230000019073486328125)) * _167) + _176);
    float2 _190 = fract(_p0 * ((34.532001495361328125 + (_t9 * 2.0)) + (_p1.y * 0.80309998989105224609375)));
    float2 _199 = _190 + float2(dot(_190, _190.yx + float2(15.4340000152587890625)));
    float2 _203 = _199.yx;
    float2 _t13 = fract((((_199 + _203) + float2(0.5230000019073486328125)) * _203) + _176);
    _t13.y += 1.0;
    float _230 = (mix(_t11.y, _t13.y, fract(_116)) * 3.141590118408203125) * 2.0;
    float _236 = (_p1.x * 2.0) + _107;
    float _239 = floor(_236);
    float _t17 = _239;
    float _t18 = _239 + 1.0;
    if (_128)
    {
        _t17 = floor(mod(_236, u_Cycle));
        _t18 = floor(mod(_236 + 1.0, u_Cycle));
    }
    float2 _267 = fract(_p0 * ((34.532001495361328125 + (_t17 * 2.0)) + (_p1.y * 0.80309998989105224609375)));
    float2 _276 = _267 + float2(dot(_267, _267.yx + float2(15.4340000152587890625)));
    float2 _280 = _276.yx;
    float2 _t20 = fract((((_276 + _280) + float2(0.5230000019073486328125)) * _280) + _176);
    float2 _302 = fract(_p0 * ((34.532001495361328125 + (_t18 * 2.0)) + (_p1.y * 0.80309998989105224609375)));
    float2 _311 = _302 + float2(dot(_302, _302.yx + float2(15.4340000152587890625)));
    float2 _315 = _311.yx;
    float2 _t22 = fract((((_311 + _315) + float2(0.5230000019073486328125)) * _315) + _176);
    return float2(cos(_230), sin(_230)) * (mix(0.75, 1.0, mix(_t20.x, _t22.x, fract(_236))) * 1.0);
}

static inline __attribute__((always_inline))
float _f4(thread const float2& _p0, thread const float2& _p1, constant float& u_Cycle, constant float4& u_ScreenParams)
{
    float2 _411 = (float2(720.0) * u_ScreenParams.xy) / float2(fast::min(u_ScreenParams.x, u_ScreenParams.y));
    float2 _419 = floor((_p0 * _411) / float2(100.0));
    float2 _426 = fract((_p0 * _411) / float2(100.0));
    float2 param = _419 + float2(0.0);
    float2 param_1 = _p1;
    float2 param_2 = _419 + float2(1.0, 0.0);
    float2 param_3 = _p1;
    float2 param_4 = _419 + float2(0.0, 1.0);
    float2 param_5 = _p1;
    float2 param_6 = _419 + float2(1.0);
    float2 param_7 = _p1;
    float2 _t36 = smoothstep(float2(0.0), float2(1.0), _426);
    return (mix(mix(dot(_f2(param, param_1, u_Cycle), _426 - float2(0.0)), dot(_f2(param_2, param_3, u_Cycle), _426 - float2(1.0, 0.0)), _t36.x), mix(dot(_f2(param_4, param_5, u_Cycle), _426 - float2(0.0, 1.0)), dot(_f2(param_6, param_7, u_Cycle), _426 - float2(1.0)), _t36.x), _t36.y) * 0.5) + 0.5;
}

static inline __attribute__((always_inline))
float _f5(thread float2& _p0, thread const float& _p1, thread const float& _p2, thread const float& _p3, thread const float& _p4, thread const float& _p5, thread const float& _p6, thread const float2& _p7, constant float& u_Cycle, constant float4& u_ScreenParams)
{
    float2 _524 = (float2(720.0) * u_ScreenParams.xy) / float2(fast::min(u_ScreenParams.x, u_ScreenParams.y));
    float _527 = floor(_p1);
    float _530 = fract(_p1);
    float2 param = _p0;
    float2 param_1 = float2(_p2, 1.0 + _p6);
    float _t44 = 1.0;
    float _t45 = _p3;
    float _t46 = _f4(param, param_1, u_Cycle, u_ScreenParams) * 1.0;
    for (float _t47 = 2.0; _t47 <= 10.0; _t47 += 1.0)
    {
        if (_t47 > _527)
        {
            break;
        }
        _p0 -= (_p7 / _524);
        float2 param_2 = _p0;
        float param_3 = (_p5 * 3.141592502593994140625) / 180.0;
        float2 _579 = _f3(param_2, param_3, u_ScreenParams);
        _p0 = _579;
        _p0 *= _p4;
        float2 param_4 = _p0;
        float2 param_5 = float2(_p2, 1.0 + _p6);
        float _591 = _t45;
        _t44 += _591;
        _t45 = _591 * _p3;
        _t46 += (_f4(param_4, param_5, u_Cycle, u_ScreenParams) * _591);
    }
    _p0 -= (_p7 / _524);
    float2 param_6 = _p0;
    float param_7 = (_p5 * 3.141592502593994140625) / 180.0;
    float2 _615 = _f3(param_6, param_7, u_ScreenParams);
    _p0 = _615;
    _p0 *= _p4;
    float2 param_8 = _p0;
    float2 param_9 = float2(_p2, 1.0 + _p6);
    float _634 = _t44;
    float _635 = _634 + (_t45 * _530);
    _t44 = _635;
    float _637 = _t46;
    float _641 = (_637 + ((_f4(param_8, param_9, u_Cycle, u_ScreenParams) * _t45) * _530)) / _635;
    _t46 = _641;
    return fast::clamp(_641, 0.0, 1.0);
}

static inline __attribute__((always_inline))
float4 _f1(thread const float2& _p0)
{
    return float4(floor(_p0.x * 255.0) / 255.0, fract(_p0.x * 255.0), floor(_p0.y * 255.0) / 255.0, fract(_p0.y * 255.0));
}

fragment main0_out main0(main0_in in [[stage_in]], constant buffer_t& buffer)
{
    main0_out out = {};
    float2 _654 = in.uv0 - buffer.u_Offset;
    float2 _t48 = _654;
    float2 param = _654;
    float param_1 = (buffer.u_Rotate * 3.141592502593994140625) / 180.0;
    float2 _662 = _f3(param, param_1, buffer.u_ScreenParams);
    _t48 = (_662 * (float2(1.0) / buffer.u_Scale)) - float2(10.0);
    float _t49 = 0.5;
    float _t50 = 0.5;
    if (buffer.u_type < 0.5)
    {
        float _681 = fast::clamp(buffer.u_Complexity, 1.0, 10.0);
        float _686 = 100.0 / buffer.u_SubScale;
        float2 param_2 = _t48;
        float param_3 = _681;
        float param_4 = buffer.u_Evolution;
        float param_5 = buffer.u_SubImpact;
        float param_6 = _686;
        float param_7 = buffer.u_SubRotate;
        float param_8 = 2.1099998950958251953125;
        float2 param_9 = buffer.u_SubOffset;
        float _703 = _f5(param_2, param_3, param_4, param_5, param_6, param_7, param_8, param_9, buffer.u_Cycle, buffer.u_ScreenParams);
        _t49 = _703;
        float2 param_10 = _t48;
        float param_11 = _681;
        float param_12 = buffer.u_Evolution;
        float param_13 = buffer.u_SubImpact;
        float param_14 = _686;
        float param_15 = buffer.u_SubRotate;
        float param_16 = 9.71000003814697265625;
        float2 param_17 = buffer.u_SubOffset;
        float _722 = _f5(param_10, param_11, param_12, param_13, param_14, param_15, param_16, param_17, buffer.u_Cycle, buffer.u_ScreenParams);
        _t50 = _722;
    }
    else
    {
        float _728 = fast::clamp(buffer.u_Complexity, 1.0, 10.0);
        float _730 = 100.0 / buffer.u_SubScale;
        float2 param_18 = float2(0.5, _t48.y);
        float param_19 = _728;
        float param_20 = buffer.u_Evolution;
        float param_21 = buffer.u_SubImpact;
        float param_22 = _730;
        float param_23 = buffer.u_SubRotate;
        float param_24 = 2.1099998950958251953125;
        float2 param_25 = buffer.u_SubOffset;
        float _743 = _f5(param_18, param_19, param_20, param_21, param_22, param_23, param_24, param_25, buffer.u_Cycle, buffer.u_ScreenParams);
        _t49 = _743;
        float2 param_26 = float2(_t48.x, 0.5);
        float param_27 = _728;
        float param_28 = buffer.u_Evolution;
        float param_29 = buffer.u_SubImpact;
        float param_30 = _730;
        float param_31 = buffer.u_SubRotate;
        float param_32 = 9.71000003814697265625;
        float2 param_33 = buffer.u_SubOffset;
        float _763 = _f5(param_26, param_27, param_28, param_29, param_30, param_31, param_32, param_33, buffer.u_Cycle, buffer.u_ScreenParams);
        _t50 = _763;
    }
    float2 param_34 = float2(_t49, _t50);
    out.o_fragColor = _f1(param_34);
    return out;
}

