; A guarded loop carries three accumulators. Alternate iterations miss, and the
; last hit rotates the vector before updating it. Position.x stays live to exit.
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

define void @shade() {
entry:
  %x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %pixel = fptoui float %x to i32
  %limit = and i32 %pixel, 3
  %enter = icmp ne i32 %limit, 0
  br i1 %enter, label %preheader, label %done

preheader:
  br label %header

header:
  %step = phi i32 [ 0, %preheader ], [ %next, %latch ]
  %a = phi float [ 1.000000e+00, %preheader ], [ %latched.a, %latch ]
  %b = phi float [ 2.000000e+00, %preheader ], [ %latched.b, %latch ]
  %c = phi float [ 3.000000e+00, %preheader ], [ %latched.c, %latch ]
  %parity = and i32 %step, 1
  %hit = icmp eq i32 %parity, 0
  br i1 %hit, label %contribution, label %latch

contribution:
  %step.f = uitofp i32 %step to float
  %base.a = fadd float %step.f, 1.000000e+00
  %base.b = fmul float %base.a, 2.000000e+00
  %base.c = fmul float %base.a, 3.000000e+00
  %rotation = and i32 %step, 2
  %rotate = icmp ne i32 %rotation, 0
  br i1 %rotate, label %rotated, label %update

rotated:
  %rot.a = fadd float %base.c, 0.000000e+00
  %rot.b = fadd float %base.a, 0.000000e+00
  %rot.c = fadd float %base.b, 0.000000e+00
  br label %update

update:
  %v.a = phi float [ %base.a, %contribution ], [ %rot.a, %rotated ]
  %v.b = phi float [ %base.b, %contribution ], [ %rot.b, %rotated ]
  %v.c = phi float [ %base.c, %contribution ], [ %rot.c, %rotated ]
  %new.a = fadd fast float %v.a, %a
  %new.b = fadd fast float %v.b, %b
  %new.c = fadd fast float %v.c, %c
  br label %latch

latch:
  %latched.a = phi float [ %a, %header ], [ %new.a, %update ]
  %latched.b = phi float [ %b, %header ], [ %new.b, %update ]
  %latched.c = phi float [ %c, %header ], [ %new.c, %update ]
  %next = add i32 %step, 1
  %repeat = icmp ult i32 %next, %limit
  br i1 %repeat, label %header, label %exit

exit:
  br label %done

done:
  %result.a = phi float [ 1.000000e+00, %entry ], [ %latched.a, %exit ]
  %result.b = phi float [ 2.000000e+00, %entry ], [ %latched.b, %exit ]
  %result.c = phi float [ 3.000000e+00, %entry ], [ %latched.c, %exit ]
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %result.a)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %result.b)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %result.c)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %x)
  ret void
}

declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) nounwind readnone
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
