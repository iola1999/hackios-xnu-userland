#include <metal_stdlib>
using namespace metal;
[[stitchable]] bool stop(uint3 gid, uint count) { return gid.x >= count; }
[[stitchable]] float read_value(constant float *src) { return *src; }
[[stitchable]] void store(texture2d<float, access::write> dst, float value, uint3 gid) {
   dst.write(float4(value + float(gid.x)), gid.xy);
}
[[stitchable]] float twice(float x) { return x * 2.0f; }
[[visible]] float linked_value(float x);
[[kernel]] void call_value(device float *out [[buffer(0)]], uint gid [[thread_position_in_grid]]) {
   out[gid] = linked_value(3.25f);
}
[[visible]] void linked_void(texture2d<float, access::write> dst, float value, uint3 gid);
[[kernel]] void call_void(texture2d<float, access::write> dst [[texture(3)]], uint3 gid [[thread_position_in_grid]]) {
   linked_void(dst, 1.5f, uint3(0));
}

[[stitchable]] float read_mixed(texture2d<float, access::sample> tex, sampler api) {
   constexpr sampler local(coord::normalized, address::repeat, filter::nearest);
   return tex.sample(local, float2(1.125f, 0.5f)).x + 10.0f * tex.sample(api, float2(1.125f, 0.5f)).x;
}
