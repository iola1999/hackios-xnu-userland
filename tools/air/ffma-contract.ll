source_filename = "ffma-contract"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-v32:32:32-v64:64:64-v128:128:128-n8:16:32"
target triple = "air64_v28-apple-ios26.6.0"

define <{ <4 x float> }> @plane__vertex(i16 %vertex_id) {
  %x = uitofp i16 %vertex_id to float
  %f = tail call fast float @air.fma.f32(float %x, float 2.0, float 1.0)
  %v0 = insertelement <4 x float> undef, float %f, i32 0
  %v1 = insertelement <4 x float> %v0, float 0.0, i32 1
  %v2 = insertelement <4 x float> %v1, float 0.0, i32 2
  %v3 = insertelement <4 x float> %v2, float 1.0, i32 3
  %result = insertvalue <{ <4 x float> }> undef, <4 x float> %v3, 0
  ret <{ <4 x float> }> %result
}

declare float @air.fma.f32(float, float, float)

!air.version = !{!0}
!air.vertex = !{!1}
!0 = !{i32 2, i32 8, i32 0}
!1 = !{ptr @plane__vertex, !2, !4}
!2 = !{!3}
!3 = !{!"air.position"}
!4 = !{!5}
!5 = !{i32 0, !"air.vertex_id"}
