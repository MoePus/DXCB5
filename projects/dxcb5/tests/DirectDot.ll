; Cover fast LLVM and DXIL multiply-add chains, literal operands, mixed registers and precise FMad.
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

define void @shade() {
  %x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %y = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
  %z = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 2, i32 undef)
  %w = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 3, i32 undef)
  %other = call float @dx.op.loadInput.f32(i32 4, i32 1, i32 0, i8 0, i32 undef)

  %native.p = fmul fast float %x, %x
  %native.m = call float @dx.op.tertiary.f32(i32 46, float %y, float %y, float %native.p)
  %native = call float @dx.op.tertiary.f32(i32 46, float %z, float %z, float %native.m)

  %scalar.p0 = fmul fast float %x, %w
  %scalar.p1 = fmul fast float %y, %z
  %scalar.m = fadd fast float %scalar.p0, %scalar.p1
  %scalar.p2 = fmul fast float %w, %z
  %scalar = fadd fast float %scalar.m, %scalar.p2

  %mixed.p = fmul fast float %x, %x
  %mixed.m = call float @dx.op.tertiary.f32(i32 46, float %other, float %y, float %mixed.p)
  %mixed = call float @dx.op.tertiary.f32(i32 46, float %z, float %z, float %mixed.m)

  %precise.p = fmul fast float %x, %x
  %precise.m = call float @dx.op.tertiary.f32(i32 46, float %y, float %y, float %precise.p), !dx.precise !12
  %precise = call float @dx.op.tertiary.f32(i32 46, float %z, float %z, float %precise.m)

  %literal.p = fmul fast float %x, 5.000000e+00
  %literal.m = call float @dx.op.tertiary.f32(i32 46, float 6.000000e+00, float %z, float %literal.p)
  %literal = call float @dx.op.tertiary.f32(i32 46, float %y, float 7.000000e+00, float %literal.m)

  %dot2 = call float @dx.op.dot2.f32(i32 54, float %x, float %y, float -0.000000e+00, float 2.000000e+00)
  %dot3 = call float @dx.op.dot3.f32(i32 55, float %x, float %z, float %y, float 3.000000e+00, float 2.000000e+00, float 1.000000e+00)
  %dot4 = call float @dx.op.dot4.f32(i32 56, float %x, float %z, float %w, float %y, float 1.000000e+00, float 4.000000e+00, float 2.000000e+00, float 3.000000e+00)
  %dot23 = fadd float %dot2, %dot3
  %dot4.literal = fadd float %dot4, %literal
  %dots = fadd float %dot23, %dot4.literal
  %result = fadd float %precise, %dots

  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %native)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %scalar)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %mixed)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %result)
  ret void
}

declare float @dx.op.tertiary.f32(i32, float, float, float) nounwind readnone
declare float @dx.op.dot2.f32(i32, float, float, float, float) nounwind readnone
declare float @dx.op.dot3.f32(i32, float, float, float, float, float, float) nounwind readnone
declare float @dx.op.dot4.f32(i32, float, float, float, float, float, float, float, float) nounwind readnone
declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) nounwind readnone
declare void @dx.op.storeOutput.f32(i32, i32, i32, i8, float) nounwind

!dx.version = !{!0}
!dx.shaderModel = !{!1}
!dx.entryPoints = !{!2}

!0 = !{i32 1, i32 0}
!1 = !{!"vs", i32 6, i32 0}
!2 = !{void ()* @shade, !"shade", !3, null, null}
!3 = !{!4, !8, null}
!4 = !{!5, !10}
!5 = !{i32 0, !"POSITION", i8 9, i8 0, !6, i8 0, i32 1, i8 4, i32 0, i8 0, !7}
!6 = !{i32 0}
!7 = !{i32 3, i32 15}
!8 = !{!9}
!9 = !{i32 0, !"SV_Position", i8 9, i8 3, !6, i8 4, i32 1, i8 4, i32 0, i8 0, !7}
!10 = !{i32 1, !"POSITION", i8 9, i8 0, !11, i8 0, i32 1, i8 1, i32 1, i8 0, null}
!11 = !{i32 1}
!12 = !{i32 1}
