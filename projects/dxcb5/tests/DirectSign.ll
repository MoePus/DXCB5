; Ordered signs include NaN and signed zero. The fourth channel compares
; different values, so it must retain the original arithmetic.
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i64:64"
target triple = "dxil-ms-dx"

define void @shade() {
entry:
  %x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %ix = fptoui float %x to i32
  %parity = and i32 %ix, 1
  %odd = icmp ne i32 %parity, 0
  %special = select i1 %odd, float 0x8000000000000000, float 0x7FF8000000000000
  %zero = fsub float %x, %x
  %nonzero = select i1 %odd, float -2.000000e+00, float 2.000000e+00

  %edge.pos = fcmp ogt float %special, 0.000000e+00
  %edge.neg = fcmp olt float %special, 0.000000e+00
  %edge.pos.i = zext i1 %edge.pos to i32
  %edge.neg.i = zext i1 %edge.neg to i32
  %edge.i = sub nsw i32 %edge.pos.i, %edge.neg.i
  %edge = sitofp i32 %edge.i to float

  %zero.pos = fcmp olt float 0.000000e+00, %zero
  %zero.neg = fcmp ogt float 0.000000e+00, %zero
  %zero.pos.i = zext i1 %zero.pos to i32
  %zero.neg.i = zext i1 %zero.neg to i32
  %zero.i = sub nsw i32 %zero.pos.i, %zero.neg.i
  %zero.result = sitofp i32 %zero.i to float

  %number.pos = fcmp ogt float %nonzero, 0.000000e+00
  %number.neg = fcmp olt float %nonzero, 0.000000e+00
  %number.pos.i = zext i1 %number.pos to i32
  %number.neg.i = zext i1 %number.neg to i32
  %number.i = sub nsw i32 %number.pos.i, %number.neg.i
  %number = sitofp i32 %number.i to float

  %mismatch.pos.i = zext i1 %number.pos to i32
  %mismatch.neg.i = zext i1 %edge.neg to i32
  %mismatch.i = sub nsw i32 %mismatch.pos.i, %mismatch.neg.i
  %mismatch = sitofp i32 %mismatch.i to float
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %edge)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %zero.result)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %number)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %mismatch)
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
