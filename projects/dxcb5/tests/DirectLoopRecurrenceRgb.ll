target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

define void @shade() {
entry:
  %x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %y = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
  %z = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 2, i32 undef)
  %w = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 3, i32 undef)
  %u = fadd float %x, %z
  %v = fadd float %y, %w
  %t = fadd float %x, %y
  %enter = fcmp ogt float %z, 0.000000e+00
  br i1 %enter, label %pre, label %exit

pre:
  br label %loop

loop:
  %left = phi float [ 0.000000e+00, %pre ], [ %nextL, %loop ]
  %right = phi float [ 0.000000e+00, %pre ], [ %nextR, %loop ]
  %blue = phi float [ 0.000000e+00, %pre ], [ %nextB, %loop ]
  %i = phi i32 [ 0, %pre ], [ %step, %loop ]
  %iter = uitofp i32 %i to float
  %scale = fadd float %iter, %z
  %fractionL = fadd float %u, %scale
  %fractionR = fadd float %v, %scale
  %fractionB = fadd float %t, %scale
  %productL = fmul fast float %scale, %fractionL
  %productR = fmul fast float %scale, %fractionR
  %productB = fmul fast float %scale, %fractionB
  %nextL = fadd fast float %productL, %left
  %nextR = fadd fast float %productR, %right
  %nextB = fadd fast float %productB, %blue
  %step = add i32 %i, 1
  %more = icmp ult i32 %step, 12
  br i1 %more, label %loop, label %exit

exit:
  %outL = phi float [ 0.000000e+00, %entry ], [ %nextL, %loop ]
  %outR = phi float [ 0.000000e+00, %entry ], [ %nextR, %loop ]
  %outB = phi float [ 0.000000e+00, %entry ], [ %nextB, %loop ]
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %outL)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %outR)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %outB)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float 1.000000e+00)
  ret void
}

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
!10 = !{i32 0, !"SV_Position", i8 9, i8 3, !6, i8 4, i32 1, i8 4, i32 0, i8 0, !7}
