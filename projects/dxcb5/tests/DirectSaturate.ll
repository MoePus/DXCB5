; The first three producers have one same-block saturate use. The fourth
; channel uses an unclamped multi-use add and a cross-block clamp.
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i64:64"
target triple = "dxil-ms-dx"

define void @shade() {
entry:
  %x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %ix = fptoui float %x to i32
  %parity = and i32 %ix, 1
  %odd = icmp ne i32 %parity, 0
  %exception = select i1 %odd, float 0x8000000000000000, float 0x7FF8000000000000
  %zero = select i1 %odd, float 0x8000000000000000, float 0.000000e+00
  %p0 = fmul float %exception, 2.000000e+00
  %p1 = fdiv float %zero, 5.000000e-01
  %p2 = fsub float %x, 6.000000e+00
  %cross = fadd float %x, -6.250000e+00
  %multi = fadd float %x, -7.250000e+00
  ; A discard predicate is independent of the arithmetic. Target pixels survive.
  %discard = fcmp olt float %x, 4.000000e+00
  call void @dx.op.discard(i32 82, i1 %discard)
  %sat0 = call float @dx.op.unary.f32(i32 7, float %p0)
  %sat1 = call float @dx.op.unary.f32(i32 7, float %p1)
  %sat2 = call float @dx.op.unary.f32(i32 7, float %p2)
  %sat.multi = call float @dx.op.unary.f32(i32 7, float %multi)
  %both = fadd float %multi, %sat.multi
  br i1 %odd, label %other, label %join

other:
  %sat.cross = call float @dx.op.unary.f32(i32 7, float %cross)
  br label %join

join:
  %clamped.cross = phi float [ 0.000000e+00, %entry ], [ %sat.cross, %other ]
  %out3 = fadd float %both, %clamped.cross
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %sat0)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %sat1)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %sat2)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %out3)
  ret void
}

declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) nounwind readnone
declare float @dx.op.unary.f32(i32, float) nounwind readnone
declare void @dx.op.discard(i32, i1) nounwind
declare void @dx.op.storeOutput.f32(i32, i32, i32, i8, float) nounwind

!dx.version = !{!0}
!dx.shaderModel = !{!1}
!dx.entryPoints = !{!2}

!0 = !{i32 1, i32 0}
!1 = !{!"ps", i32 6, i32 0}
!2 = !{void ()* @shade, !"shade", !3, null, null}
!3 = !{!4, !8, null}
!4 = !{!5}
!5 = !{i32 0, !"SV_Position", i8 9, i8 3, !6, i8 4, i32 1, i8 4, i32 0, i8 0, !7}
!6 = !{i32 0}
!7 = !{i32 3, i32 1}
!8 = !{!9}
!9 = !{i32 0, !"SV_Target", i8 9, i8 16, !6, i8 0, i32 1, i8 4, i32 0, i8 0, null}
