#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;
float2 Flip_v(float flip, float2 uv) { if(flip > 0.5) uv.y = 1.0 - uv.y; return uv; }
float3 Flip_v(float flip, float3 uv) { if(flip > 0.5) uv.y = 1.0 - uv.y; return uv; }
float4 Flip_v(float flip, float4 uv) { if(flip > 0.5) uv.y = 1.0 - uv.y; return uv; }



template<typename Tx, typename Ty>
inline Tx mod(Tx x, Ty y)
{
    return x - y * floor(x / y);
}

struct buffer_t
{
    float u_Brightness;
    float u_Contrast;
    float4 u_ScreenParams;
    float2 u_Scale;
    float u_Range;
    float u_type;
    float u_fix_type;
    float motion_tile_type;
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
float2 _f0(thread const float4& _p0)
{
    return float2(_p0.x + (_p0.y / 255.0), _p0.z + (_p0.w / 255.0));
}

static inline __attribute__((always_inline))
float _f3(thread float& _p0, thread const float& _p1, thread const float& _p2)
{
    _p0 += _p1;
    _p0 = (((_p0 - 0.5) * _p2) * 10.0) + 0.5;
    return _p0;
}

static inline __attribute__((always_inline))
float _f2(thread const float2& _p0)
{
    return ((step(0.0, _p0.x) * step(_p0.x, 1.0)) * step(0.0, _p0.y)) * step(_p0.y, 1.0);
}

static inline __attribute__((always_inline))
float2 _f1(thread const float2& _p0)
{
    return abs(mod(_p0 - float2(1.0), float2(2.0)) - float2(1.0));
}

fragment main0_out main0(main0_in in [[stage_in]], constant buffer_t& buffer, texture2d<float> noiseTexture [[texture(0)]], texture2d<float> inputImageTexture [[texture(1)]], sampler noiseTextureSmplr [[sampler(0)]], sampler inputImageTextureSmplr [[sampler(1)]], constant float4& u_is_texture_0_flip_)
{
    main0_out out = {};
    float4 param = noiseTexture.sample(noiseTextureSmplr, Flip_v(u_is_texture_0_flip_.x, in.uv0));
    float2 _t2 = _f0(param);
    float _t3 = _t2.x;
    float _t4 = _t2.y;
    float param_1 = _t2.x;
    float param_2 = buffer.u_Brightness;
    float param_3 = buffer.u_Contrast;
    float _126 = _f3(param_1, param_2, param_3);
    _t3 = _126;
    float param_4 = _t4;
    float param_5 = buffer.u_Brightness;
    float param_6 = buffer.u_Contrast;
    float _133 = _f3(param_4, param_5, param_6);
    float _142 = fast::min(buffer.u_ScreenParams.x, buffer.u_ScreenParams.y);
    _t3 = (((_t3 - 0.5) * _142) / buffer.u_ScreenParams.x) + 0.5;
    _t4 = (((_133 - 0.5) * _142) / buffer.u_ScreenParams.y) + 0.5;
    float _169 = fast::clamp(buffer.u_Scale.x, 0.00999999977648258209228515625, 1.0) * buffer.u_Range;
    float2 _t6 = in.uv0;
    if (buffer.u_type < 0.5)
    {
        _t6 = float2(in.uv0.x + ((_t3 - 0.5) * _169), in.uv0.y + ((_t4 - 0.5) * _169));
    }
    else
    {
        if (buffer.u_type < 1.5)
        {
            _t6 = float2(in.uv0.x + ((_t3 - 0.5) * _169), in.uv0.y);
        }
        else
        {
            if (buffer.u_type < 2.5)
            {
                _t6 = float2(in.uv0.x, in.uv0.y + ((_t4 - 0.5) * _169));
            }
            else
            {
                _t6 = float2(in.uv0.x + ((_t3 - 0.5) * _169), in.uv0.y + ((_t4 - 0.5) * _169));
            }
        }
    }
    if (buffer.u_fix_type < 0.5)
    {
    }
    else
    {
        if (buffer.u_fix_type < 1.5)
        {
            if (in.uv0.x < 0.100000001490116119384765625)
            {
                _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.x));
            }
            if (in.uv0.x > 0.89999997615814208984375)
            {
                _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.x));
            }
            if (in.uv0.y < 0.100000001490116119384765625)
            {
                _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.y));
            }
            if (in.uv0.y > 0.89999997615814208984375)
            {
                _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.y));
            }
        }
        else
        {
            if (buffer.u_fix_type < 2.5)
            {
                if (fast::clamp(_t6.x, 0.0, 1.0) == _t6.x)
                {
                    if (in.uv0.y < 0.100000001490116119384765625)
                    {
                        _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.y));
                    }
                    if (in.uv0.y > 0.89999997615814208984375)
                    {
                        _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.y));
                    }
                }
            }
            else
            {
                if (buffer.u_fix_type < 3.5)
                {
                    if (fast::clamp(_t6.y, 0.0, 1.0) == _t6.y)
                    {
                        if (in.uv0.x < 0.100000001490116119384765625)
                        {
                            _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.x));
                        }
                        if (in.uv0.x > 0.89999997615814208984375)
                        {
                            _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.x));
                        }
                    }
                }
                else
                {
                    if (buffer.u_fix_type < 4.5)
                    {
                        bool _423 = _t6.x <= 1.0;
                        bool _432;
                        if (_423)
                        {
                            _432 = fast::clamp(_t6.y, 0.0, 1.0) == _t6.y;
                        }
                        else
                        {
                            _432 = _423;
                        }
                        if (_432)
                        {
                            if (in.uv0.x < 0.100000001490116119384765625)
                            {
                                _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.x));
                            }
                        }
                    }
                    else
                    {
                        if (buffer.u_fix_type < 5.5)
                        {
                            bool _459 = _t6.x >= 0.0;
                            bool _468;
                            if (_459)
                            {
                                _468 = fast::clamp(_t6.y, 0.0, 1.0) == _t6.y;
                            }
                            else
                            {
                                _468 = _459;
                            }
                            if (_468)
                            {
                                if (in.uv0.x > 0.89999997615814208984375)
                                {
                                    _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.x));
                                }
                            }
                        }
                        else
                        {
                            if (buffer.u_fix_type < 6.5)
                            {
                                bool _496 = _t6.y >= 0.0;
                                bool _505;
                                if (_496)
                                {
                                    _505 = fast::clamp(_t6.x, 0.0, 1.0) == _t6.x;
                                }
                                else
                                {
                                    _505 = _496;
                                }
                                if (_505)
                                {
                                    if (in.uv0.y > 0.89999997615814208984375)
                                    {
                                        _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.y));
                                    }
                                }
                            }
                            else
                            {
                                if (buffer.u_fix_type < 7.5)
                                {
                                    bool _533 = _t6.y <= 1.0;
                                    bool _542;
                                    if (_533)
                                    {
                                        _542 = fast::clamp(_t6.x, 0.0, 1.0) == _t6.x;
                                    }
                                    else
                                    {
                                        _542 = _533;
                                    }
                                    if (_542)
                                    {
                                        if (in.uv0.y < 0.100000001490116119384765625)
                                        {
                                            _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.y));
                                        }
                                    }
                                }
                                else
                                {
                                    if (buffer.u_fix_type < 8.5)
                                    {
                                        bool _569 = in.uv0.x < 0.100000001490116119384765625;
                                        if (_569)
                                        {
                                            _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.x));
                                        }
                                        bool _585 = in.uv0.x > 0.89999997615814208984375;
                                        if (_585)
                                        {
                                            _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.x));
                                        }
                                        bool _602 = in.uv0.y < 0.100000001490116119384765625;
                                        if (_602)
                                        {
                                            _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.y));
                                        }
                                        bool _618 = in.uv0.y > 0.89999997615814208984375;
                                        if (_618)
                                        {
                                            _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.y));
                                        }
                                        float _t24 = 1.0;
                                        if (_569)
                                        {
                                            _t24 = fast::min(_t24, in.uv0.x * 10.0);
                                        }
                                        if (_585)
                                        {
                                            _t24 = fast::min(_t24, (1.0 - in.uv0.x) * 10.0);
                                        }
                                        if (_602)
                                        {
                                            _t24 = fast::min(_t24, in.uv0.y * 10.0);
                                        }
                                        if (_618)
                                        {
                                            _t24 = fast::min(_t24, (1.0 - in.uv0.y) * 10.0);
                                        }
                                        _t6 = mix(in.uv0, _t6, float2(_t24));
                                    }
                                    else
                                    {
                                        if (buffer.u_fix_type < 9.5)
                                        {
                                            bool _689 = in.uv0.y < 0.100000001490116119384765625;
                                            if (_689)
                                            {
                                                _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.y));
                                            }
                                            bool _705 = in.uv0.y > 0.89999997615814208984375;
                                            if (_705)
                                            {
                                                _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.y));
                                            }
                                            float _t27 = 1.0;
                                            if (_689)
                                            {
                                                _t27 = fast::min(_t27, in.uv0.y * 10.0);
                                            }
                                            if (_705)
                                            {
                                                _t27 = fast::min(_t27, (1.0 - in.uv0.y) * 10.0);
                                            }
                                            _t6 = mix(in.uv0, _t6, float2(_t27));
                                        }
                                        else
                                        {
                                            if (buffer.u_fix_type < 10.5)
                                            {
                                                bool _755 = in.uv0.x < 0.100000001490116119384765625;
                                                if (_755)
                                                {
                                                    _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.x));
                                                }
                                                bool _771 = in.uv0.x > 0.89999997615814208984375;
                                                if (_771)
                                                {
                                                    _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.x));
                                                }
                                                float _t30 = 1.0;
                                                if (_755)
                                                {
                                                    _t30 = fast::min(_t30, in.uv0.x * 10.0);
                                                }
                                                if (_771)
                                                {
                                                    _t30 = fast::min(_t30, (1.0 - in.uv0.x) * 10.0);
                                                }
                                                _t6 = mix(in.uv0, _t6, float2(_t30));
                                            }
                                            else
                                            {
                                                if (buffer.u_fix_type < 11.5)
                                                {
                                                    bool _821 = in.uv0.x < 0.100000001490116119384765625;
                                                    if (_821)
                                                    {
                                                        _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.x));
                                                    }
                                                    float _t32 = 1.0;
                                                    if (_821)
                                                    {
                                                        _t32 = fast::min(_t32, in.uv0.x * 10.0);
                                                    }
                                                    _t6 = mix(in.uv0, _t6, float2(_t32));
                                                }
                                                else
                                                {
                                                    if (buffer.u_fix_type < 12.5)
                                                    {
                                                        bool _859 = in.uv0.x > 0.89999997615814208984375;
                                                        if (_859)
                                                        {
                                                            _t6.x = mix(in.uv0.x, _t6.x, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.x));
                                                        }
                                                        float _t34 = 1.0;
                                                        if (_859)
                                                        {
                                                            _t34 = fast::min(_t34, (1.0 - in.uv0.x) * 10.0);
                                                        }
                                                        _t6 = mix(in.uv0, _t6, float2(_t34));
                                                    }
                                                    else
                                                    {
                                                        if (buffer.u_fix_type < 13.5)
                                                        {
                                                            bool _899 = in.uv0.y > 0.89999997615814208984375;
                                                            if (_899)
                                                            {
                                                                _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, 1.0 - in.uv0.y));
                                                            }
                                                            float _t36 = 1.0;
                                                            if (_899)
                                                            {
                                                                _t36 = fast::min(_t36, (1.0 - in.uv0.y) * 10.0);
                                                            }
                                                            _t6 = mix(in.uv0, _t6, float2(_t36));
                                                        }
                                                        else
                                                        {
                                                            if (buffer.u_fix_type < 14.5)
                                                            {
                                                                bool _939 = in.uv0.y < 0.100000001490116119384765625;
                                                                if (_939)
                                                                {
                                                                    _t6.y = mix(in.uv0.y, _t6.y, smoothstep(0.0, 0.100000001490116119384765625, in.uv0.y));
                                                                }
                                                                float _t38 = 1.0;
                                                                if (_939)
                                                                {
                                                                    _t38 = fast::min(_t38, in.uv0.y * 10.0);
                                                                }
                                                                _t6 = mix(in.uv0, _t6, float2(_t38));
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    float _t39 = 1.0;
    if (buffer.motion_tile_type < 0.5)
    {
        float2 param_7 = _t6;
        _t39 = _f2(param_7);
    }
    else
    {
        if (buffer.motion_tile_type < 1.5)
        {
            _t6 = fract(_t6);
        }
        else
        {
            float2 param_8 = _t6;
            _t6 = _f1(param_8);
        }
    }
    out.o_fragColor = inputImageTexture.sample(inputImageTextureSmplr, Flip_v(u_is_texture_0_flip_.y, _t6)) * _t39;
    return out;
}

