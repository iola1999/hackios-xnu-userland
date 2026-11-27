#include <metal_stdlib>
using namespace metal;

struct QuadOut {
    float4 position [[position]];
    float2 uv;
};

vertex QuadOut mr_vertex(uint vid [[vertex_id]])
{
    const float2 pos[3] = {
        float2(-1.0f, -1.0f),
        float2( 3.0f, -1.0f),
        float2(-1.0f,  3.0f),
    };
    const float2 uv[3] = {
        float2(0.0f, 1.0f),
        float2(2.0f, 1.0f),
        float2(0.0f, -1.0f),
    };
    QuadOut out;
    out.position = float4(pos[vid], 0.0f, 1.0f);
    out.uv = uv[vid];
    return out;
}

/* 1. Liquid Glass / multi-attachment framebuffer fetch (Color0 BGRA8, Color1 RGBA16F). */
struct GlassOut {
    float4 color0 [[color(0)]];
    half4 color1 [[color(1)]];
};

fragment GlassOut mr_glass_fbfetch(QuadOut in [[stage_in]],
                                   float4 dst0 [[color(0)]],
                                   half4 dst1 [[color(1)]],
                                   texture2d<float> src [[texture(0)]],
                                   sampler samp [[sampler(0)]])
{
    float2 refract_uv = clamp(in.uv + float2(dst1.r, dst1.g) * 0.0625f, 0.0f, 1.0f);
    float4 sample_c = src.sample(samp, refract_uv);
    float fresnel = clamp(0.25f + 0.5f * length(in.uv - float2(0.5f)), 0.0f, 1.0f);
    GlassOut out;
    out.color0 = float4(mix(dst0.rgb, sample_c.rgb, 0.45f) + float3(fresnel * 0.15f), 0.75f);
    out.color1 = half4(half3(dst1.rgb) * half(0.6h) + half3(sample_c.rgb) * half(0.4h), half(1.0h));
    return out;
}

/* 2. Half-precision 5-tap Gaussian blur reading RGBA16F with linear clamp sampler. */
fragment half4 mr_blur_fp16(QuadOut in [[stage_in]],
                            texture2d<half> src [[texture(0)]],
                            sampler samp [[sampler(0)]])
{
    const float2 texel = float2(1.0f / 16.0f, 1.0f / 16.0f);
    half4 c = src.sample(samp, in.uv) * half(0.40h);
    c += src.sample(samp, in.uv + float2( texel.x, 0.0f)) * half(0.15h);
    c += src.sample(samp, in.uv + float2(-texel.x, 0.0f)) * half(0.15h);
    c += src.sample(samp, in.uv + float2(0.0f,  texel.y)) * half(0.15h);
    c += src.sample(samp, in.uv + float2(0.0f, -texel.y)) * half(0.15h);
    return c;
}

/* 3. Compositing filter & blend matrix (single source + dual source). */
fragment float4 mr_blend_src(QuadOut in [[stage_in]],
                             texture2d<float> src [[texture(0)]],
                             sampler samp [[sampler(0)]])
{
    return src.sample(samp, in.uv);
}

struct DualOut {
    float4 primary [[color(0), index(0)]];
    float4 secondary [[color(0), index(1)]];
};

fragment DualOut mr_blend_dual(QuadOut in [[stage_in]],
                               texture2d<float> src [[texture(0)]],
                               sampler samp [[sampler(0)]])
{
    float4 s = src.sample(samp, in.uv);
    DualOut out;
    out.primary = float4(s.rgb * 0.8f, s.a);
    out.secondary = float4(0.35f + 0.5f * in.uv.x, 0.25f + 0.5f * in.uv.y, 0.6f, 0.5f);
    return out;
}

/* 4. SDF text / single-channel mask AA (A8Unorm & R8Unorm). */
fragment float4 mr_sdf_a8(QuadOut in [[stage_in]],
                          texture2d<float> glyph [[texture(0)]],
                          sampler samp [[sampler(0)]])
{
    float d = glyph.sample(samp, in.uv).a;
    float cov = smoothstep(0.35f, 0.65f, d);
    float3 ink = float3(0.92f, 0.22f, 0.18f);
    return float4(ink * cov, cov);
}

fragment float4 mr_sdf_r8(QuadOut in [[stage_in]],
                          texture2d<float> glyph [[texture(0)]],
                          sampler samp [[sampler(0)]])
{
    float d = glyph.sample(samp, in.uv).r;
    float cov = smoothstep(0.35f, 0.65f, d);
    float3 ink = float3(0.15f, 0.65f, 0.90f);
    return float4(ink * cov, cov);
}

/* 5. High-precision & sRGB format passthrough with fine gradient. */
fragment float4 mr_format_grad(QuadOut in [[stage_in]])
{
    float r = clamp(in.uv.x * 0.95f + 0.025f, 0.0f, 1.0f);
    float g = clamp(in.uv.y * 0.95f + 0.025f, 0.0f, 1.0f);
    float b = clamp(0.5f * (in.uv.x + in.uv.y), 0.0f, 1.0f);
    float a = clamp(0.25f + 0.75f * in.uv.x, 0.0f, 1.0f);
    return float4(r, g, b, a);
}

/* 6. Multi-pass DontCare / writeMask=0 seed, touch, and readback. */
fragment GlassOut mr_mp_seed(QuadOut in [[stage_in]])
{
    GlassOut out;
    out.color0 = float4(in.uv.x, in.uv.y, 0.25f, 1.0f);
    out.color1 = half4(half(0.2h + 0.7h * in.uv.x),
                       half(0.1h + 0.8h * in.uv.y),
                       half(0.5h), half(1.0h));
    return out;
}

fragment GlassOut mr_mp_touch(QuadOut in [[stage_in]])
{
    GlassOut out;
    out.color0 = float4(1.0f - in.uv.x, 0.25f, in.uv.y, 1.0f);
    out.color1 = half4(half(0.9h), half(0.3h), half(0.1h + 0.6h * in.uv.x), half(1.0h));
    return out;
}

fragment float4 mr_mp_readback(QuadOut in [[stage_in]],
                               texture2d<half> c1 [[texture(0)]],
                               sampler samp [[sampler(0)]])
{
    return float4(c1.sample(samp, in.uv));
}

/* 7. G3 (ir3 0018): fsat over integer bitcast (32-bit & 16-bit) and relative constant MOV (alias.rt). */
struct G3FsatInput {
    uint4 xor_bits;
    uint4 xor_mask;
    uint4 add_base;
    uint4 add_delta;
    ushort4 half_bits;
    ushort4 half_mask;
};

fragment float4 mr_g3_fsat(QuadOut in [[stage_in]],
                           constant G3FsatInput &u [[buffer(0)]])
{
    uint lane = (uint(in.position.x) + uint(in.position.y)) & 3u;
    float fx = as_type<float>(u.xor_bits[lane] ^ u.xor_mask[lane]);
    float fa = as_type<float>(u.add_base[lane] + u.add_delta[lane]);
    half hx = as_type<half>(ushort(u.half_bits[lane] ^ u.half_mask[lane]));
    return float4(saturate(fx), saturate(fa), float(saturate(hx)), 1.0f);
}

struct G3ConstTable {
    float4 entries[8];
};

fragment float4 mr_g3_alias_rel(QuadOut in [[stage_in]],
                                constant G3ConstTable &tbl [[buffer(0)]])
{
    uint idx = (uint(in.position.x) + uint(in.position.y) * 3u) & 7u;
    return tbl.entries[idx];
}
