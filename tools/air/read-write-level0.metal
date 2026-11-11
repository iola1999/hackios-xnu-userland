#include <metal_stdlib>
using namespace metal;
kernel void rw_2d_float_uint(texture2d<float, access::read_write> t [[texture(0)]], device float4 *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
 uint2 xy=uint2(p.xy); float4 v=t.read(xy);out[p.y * 7 + p.x]=float4(v);
#ifndef OMIT_WRITE
 t.write(v+float4(.5,-.25,1,2),xy);
#endif
}
kernel void read_2d_float_uint(texture2d<float, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
 uint2 xy=uint2(p.xy); float4 v=t.read(xy);out[p.y * 7 + p.x]=float4(v);
}
kernel void rw_2d_float_ushort(texture2d<float, access::read_write> t [[texture(0)]], device float4 *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
 ushort2 xy=ushort2(p.xy); float4 v=t.read(xy);out[p.y * 7 + p.x]=float4(v);
#ifndef OMIT_WRITE
 t.write(v+float4(.5,-.25,1,2),xy);
#endif
}
kernel void read_2d_float_ushort(texture2d<float, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
 ushort2 xy=ushort2(p.xy); float4 v=t.read(xy);out[p.y * 7 + p.x]=float4(v);
}
kernel void rw_2d_half_uint(texture2d<half, access::read_write> t [[texture(0)]], device float4 *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
 uint2 xy=uint2(p.xy); half4 v=t.read(xy);out[p.y * 7 + p.x]=float4(v);
#ifndef OMIT_WRITE
 t.write(v+half4(.5,-.25,1,2),xy);
#endif
}
kernel void read_2d_half_uint(texture2d<half, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
 uint2 xy=uint2(p.xy); half4 v=t.read(xy);out[p.y * 7 + p.x]=float4(v);
}
kernel void rw_2d_half_ushort(texture2d<half, access::read_write> t [[texture(0)]], device float4 *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
 ushort2 xy=ushort2(p.xy); half4 v=t.read(xy);out[p.y * 7 + p.x]=float4(v);
#ifndef OMIT_WRITE
 t.write(v+half4(.5,-.25,1,2),xy);
#endif
}
kernel void read_2d_half_ushort(texture2d<half, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
 ushort2 xy=ushort2(p.xy); half4 v=t.read(xy);out[p.y * 7 + p.x]=float4(v);
}
kernel void rw_array_float_uint(texture2d_array<float, access::read_write> t [[texture(0)]], device float4 *out [[buffer(0)]], uint3 p [[thread_position_in_grid]]) {
 uint2 xy=uint2(p.xy); float4 v=t.read(xy, p.z);out[(p.z * 3 + p.y) * 7 + p.x]=float4(v);
#ifndef OMIT_WRITE
 t.write(v+float4(.5,-.25,1,2),xy, p.z);
#endif
}
kernel void read_array_float_uint(texture2d_array<float, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]], uint3 p [[thread_position_in_grid]]) {
 uint2 xy=uint2(p.xy); float4 v=t.read(xy, p.z);out[(p.z * 3 + p.y) * 7 + p.x]=float4(v);
}
kernel void rw_array_float_ushort(texture2d_array<float, access::read_write> t [[texture(0)]], device float4 *out [[buffer(0)]], uint3 p [[thread_position_in_grid]]) {
 ushort2 xy=ushort2(p.xy); float4 v=t.read(xy, p.z);out[(p.z * 3 + p.y) * 7 + p.x]=float4(v);
#ifndef OMIT_WRITE
 t.write(v+float4(.5,-.25,1,2),xy, p.z);
#endif
}
kernel void read_array_float_ushort(texture2d_array<float, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]], uint3 p [[thread_position_in_grid]]) {
 ushort2 xy=ushort2(p.xy); float4 v=t.read(xy, p.z);out[(p.z * 3 + p.y) * 7 + p.x]=float4(v);
}
kernel void rw_array_half_uint(texture2d_array<half, access::read_write> t [[texture(0)]], device float4 *out [[buffer(0)]], uint3 p [[thread_position_in_grid]]) {
 uint2 xy=uint2(p.xy); half4 v=t.read(xy, p.z);out[(p.z * 3 + p.y) * 7 + p.x]=float4(v);
#ifndef OMIT_WRITE
 t.write(v+half4(.5,-.25,1,2),xy, p.z);
#endif
}
kernel void read_array_half_uint(texture2d_array<half, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]], uint3 p [[thread_position_in_grid]]) {
 uint2 xy=uint2(p.xy); half4 v=t.read(xy, p.z);out[(p.z * 3 + p.y) * 7 + p.x]=float4(v);
}
kernel void rw_array_half_ushort(texture2d_array<half, access::read_write> t [[texture(0)]], device float4 *out [[buffer(0)]], uint3 p [[thread_position_in_grid]]) {
 ushort2 xy=ushort2(p.xy); half4 v=t.read(xy, p.z);out[(p.z * 3 + p.y) * 7 + p.x]=float4(v);
#ifndef OMIT_WRITE
 t.write(v+half4(.5,-.25,1,2),xy, p.z);
#endif
}
kernel void read_array_half_ushort(texture2d_array<half, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]], uint3 p [[thread_position_in_grid]]) {
 ushort2 xy=ushort2(p.xy); half4 v=t.read(xy, p.z);out[(p.z * 3 + p.y) * 7 + p.x]=float4(v);
}
