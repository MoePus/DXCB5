; The entry rows can share an exact ordered three-lane source. The strict
; row shares those inputs but retains its precision. The left branch can
; pack three rows across lanes without gathering the live-in source.
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

define void @shade() {
  %x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %y = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
  %z = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 2, i32 undef)
  %w = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 3, i32 undef)
  %u = fadd float %x, %y
  %v = fsub float %y, %z
  %t = fmul float %z, %w

  %a0 = fmul fast float %u, 1.000000e+00
  %a1 = call float @dx.op.tertiary.f32(i32 46, float %v, float 2.000000e+00, float %a0)
  %a = call float @dx.op.tertiary.f32(i32 46, float %t, float 3.000000e+00, float %a1)
  %b0 = fmul fast float %u, 4.000000e+00
  %b1 = call float @dx.op.tertiary.f32(i32 46, float %v, float 5.000000e+00, float %b0)
  %b = call float @dx.op.tertiary.f32(i32 46, float %t, float 6.000000e+00, float %b1)
  %c0 = fmul fast float %u, 7.000000e+00
  %c1 = call float @dx.op.tertiary.f32(i32 46, float %v, float 8.000000e+00, float %c0)
  %c = call float @dx.op.tertiary.f32(i32 46, float %t, float 9.000000e+00, float %c1)

  %strict0 = fmul fast float %u, 1.000000e+00
  %strict1 = call float @dx.op.tertiary.f32(i32 46, float %v, float 4.000000e+00, float %strict0), !dx.precise !9
  %strict = call float @dx.op.tertiary.f32(i32 46, float %t, float 5.000000e+00, float %strict1)
  %single0 = fmul fast float %t, 6.000000e+00
  %single1 = call float @dx.op.tertiary.f32(i32 46, float %u, float 4.000000e+00, float %single0)
  %single = call float @dx.op.tertiary.f32(i32 46, float %w, float 5.000000e+00, float %single1)
  %hidden = fadd float %strict, %single
  %first = fadd float %a, %hidden
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %first)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %b)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %c)
  %condition = fcmp olt float %x, %y
  br i1 %condition, label %left, label %right

left:
  %left0 = fmul fast float %v, 2.000000e+00
  %left1 = call float @dx.op.tertiary.f32(i32 46, float %u, float 3.000000e+00, float %left0)
  %left2 = call float @dx.op.tertiary.f32(i32 46, float %t, float 4.000000e+00, float %left1)
  %leftB0 = fmul fast float %v, 3.100000e+01
  %leftB1 = call float @dx.op.tertiary.f32(i32 46, float %u, float 3.200000e+01, float %leftB0)
  %leftB = call float @dx.op.tertiary.f32(i32 46, float %t, float 3.300000e+01, float %leftB1)
  %leftC0 = fmul fast float %v, 4.100000e+01
  %leftC1 = call float @dx.op.tertiary.f32(i32 46, float %u, float 4.200000e+01, float %leftC0)
  %leftC = call float @dx.op.tertiary.f32(i32 46, float %t, float 4.300000e+01, float %leftC1)
  %leftPair = fadd float %left2, %leftB
  %leftMatrix = fadd float %leftPair, %leftC
  br label %join

right:
  %right0 = fmul fast float %v, 5.000000e+00
  %right1 = call float @dx.op.tertiary.f32(i32 46, float %u, float 6.000000e+00, float %right0)
  %right2 = call float @dx.op.tertiary.f32(i32 46, float %t, float 7.000000e+00, float %right1)
  br label %join

join:
  %selected = phi float [ %leftMatrix, %left ], [ %right2, %right ]
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %selected)
  ret void
}

declare float @dx.op.tertiary.f32(i32, float, float, float) nounwind readnone
declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) nounwind readnone
declare void @dx.op.storeOutput.f32(i32, i32, i32, i8, float) nounwind

!dx.version = !{!0}
!dx.shaderModel = !{!1}
!dx.entryPoints = !{!2}

!0 = !{i32 1, i32 0}
!1 = !{!"vs", i32 6, i32 0}
!2 = !{void ()* @shade, !"shade", !3, null, null}
!3 = !{!4, !8, null}
!4 = !{!5}
!5 = !{i32 0, !"POSITION", i8 9, i8 0, !6, i8 0, i32 1, i8 4, i32 0, i8 0, !7}
!6 = !{i32 0}
!7 = !{i32 3, i32 15}
!8 = !{!10}
!9 = !{i32 1}
!10 = !{i32 0, !"SV_Position", i8 9, i8 3, !6, i8 4, i32 1, i8 4, i32 0, i8 0, !7}
