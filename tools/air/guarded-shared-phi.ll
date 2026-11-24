target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-n8:16:32"
target triple = "air64-apple-ios26.0.0"
@cells = internal addrspace(3) global [2 x i32] undef, align 4
define void @guarded(ptr addrspace(1) %out) {
entry:
  %cell0 = getelementptr [2 x i32], ptr addrspace(3) @cells, i32 0, i32 0
  %cell1 = getelementptr [2 x i32], ptr addrspace(3) @cells, i32 0, i32 1
  store i32 83, ptr addrspace(3) %cell0, align 4
  store i32 151, ptr addrspace(3) %cell1, align 4
  %mode = getelementptr i32, ptr addrspace(1) %out, i32 1
  %id = load i32, ptr addrspace(1) %mode, align 4
  %cond = icmp eq i32 %id, 0
  br i1 %cond, label %left, label %right
left:
  %p0 = getelementptr [2 x i32], ptr addrspace(3) @cells, i32 0, i32 0
  br label %join
right:
  %p1 = getelementptr [2 x i32], ptr addrspace(3) @cells, i32 0, i32 1
  br label %join
join:
  %p = phi ptr addrspace(3) [ %p0, %left ], [ undef, %right ]
  %q = phi ptr addrspace(3) [ undef, %left ], [ %p1, %right ]
  br i1 %cond, label %use0, label %other
use0:
  %v0 = load i32, ptr addrspace(3) %p, align 4
  store i32 %v0, ptr addrspace(1) %out
  br label %other
other:
  %inv = icmp ne i32 %id, 0
  br i1 %inv, label %use1, label %done
use1:
  %v1 = load i32, ptr addrspace(3) %q, align 4
  store i32 %v1, ptr addrspace(1) %out
  br label %done
done:
  ret void
}
!air.kernel = !{!0}
!0 = !{ptr @guarded, !1, !2}
!1 = !{}
!2 = !{!3}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.buffer_size", i32 8, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_name", !"out"}
