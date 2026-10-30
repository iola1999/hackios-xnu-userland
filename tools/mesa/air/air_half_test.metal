#include <metal_stdlib>
using namespace metal;

struct HalfInput { uint u; int i; float f; uint padding; };

#ifndef HALF_UNSUPPORTED
kernel void half_boundaries(constant HalfInput *input [[buffer(0)]],
                            texture2d<half, access::write> output [[texture(0)]],
                            uint index [[thread_position_in_grid]])
{
    HalfInput value = input[index];
    output.write(half4(half(value.u), half(value.i), half(value.f), half(1)), uint2(index, 0));
}
#else
kernel void half_unsupported(constant HalfInput *input [[buffer(0)]],
                             texture2d<half, access::write> output [[texture(0)]],
                             uint index [[thread_position_in_grid]])
{
    half sum = half(input[index].u) + half(input[index].f);
    output.write(half4(sum, sum, sum, 1), uint2(index, 0));
}
#endif
