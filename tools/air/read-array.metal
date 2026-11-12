#include <metal_stdlib>
using namespace metal;
#ifdef WRONG_LAYER
#define READ_LAYER(x) 0u
#else
#define READ_LAYER(x) (x)
#endif
struct Input {
    uint x, y, layer, level;
};
#define READ_KERNEL(NAME, TYPE, ACCESS, COORD)                                                                         \
    kernel void NAME(texture2d_array<TYPE, access::ACCESS> t [[texture(0)]], const device Input *in [[buffer(0)]],     \
                     device float4 *out [[buffer(1)]], uint n [[thread_position_in_grid]]) {                           \
        out[n] = float4(t.read(COORD(in[n].x, in[n].y), READ_LAYER(in[n].layer), in[n].level));                        \
    }
READ_KERNEL(array_float_read32, float, read, uint2)
READ_KERNEL(array_float_sample32, float, sample, uint2)
READ_KERNEL(array_float_read16, float, read, ushort2)
READ_KERNEL(array_half_read32, half, read, uint2)
kernel void plain_float_read(texture2d<float, access::read> t [[texture(0)]], const device Input *in [[buffer(0)]],
                             device float4 *out [[buffer(1)]], uint n [[thread_position_in_grid]]) {
    out[n] = t.read(uint2(in[n].x, in[n].y), in[n].level);
}
