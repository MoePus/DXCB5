; The whole tuple uses a bounded index; the other arrays exercise scalar fallback.
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

@packed = internal constant [12 x float] [
  float 1.0, float 2.0, float 3.0, float 4.0, float 5.0, float 6.0,
  float 7.0, float 8.0, float 9.0, float 10.0, float 11.0, float 12.0]
@wrapped = internal constant [12 x float] [
  float 0.0, float 1.0, float 2.0, float 3.0, float 4.0, float 5.0,
  float 6.0, float 7.0, float 8.0, float 9.0, float 10.0, float 11.0]
@mixed = internal constant [4 x float] [float 2.0, float 3.0, float 5.0, float 7.0]
@partial = internal constant [6 x float] [
  float 1.0, float 10.0, float 100.0, float 2.0, float 20.0, float 200.0]

define void @shade() {
  %x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %pixel = fptoui float %x to i32
  %index = and i32 %pixel, 3
  %base = mul i32 %index, 3
  %lane2 = add i32 2, %base
  %p2 = getelementptr [12 x float], [12 x float]* @packed, i32 0, i32 %lane2
  %z = load float, float* %p2, align 4
  %p0 = getelementptr [12 x float], [12 x float]* @packed, i32 0, i32 %base
  %r = load float, float* %p0, align 4
  %lane1 = add i32 %base, 1
  %p1 = getelementptr [12 x float], [12 x float]* @packed, i32 0, i32 %lane1
  %g = load float, float* %p1, align 4
  %required = uitofp i32 %base to float

  %inner = and i32 %index, 1
  %wrap = add i32 %inner, -1431655765
  %wrappedBase = mul i32 %wrap, 3
  %p = getelementptr [12 x float], [12 x float]* @wrapped, i32 0, i32 %wrappedBase
  %w0 = load float, float* %p, align 4
  %wrappedLane1 = add i32 %wrappedBase, 1
  %w1p = getelementptr [12 x float], [12 x float]* @wrapped, i32 0, i32 %wrappedLane1
  %w1 = load float, float* %w1p, align 4
  %wrappedLane2 = add i32 %wrappedBase, 2
  %w2p = getelementptr [12 x float], [12 x float]* @wrapped, i32 0, i32 %wrappedLane2
  %w2 = load float, float* %w2p, align 4

  %pair = mul i32 %inner, 2
  %m0p = getelementptr [4 x float], [4 x float]* @mixed, i32 0, i32 %pair
  %m0 = load float, float* %m0p, align 4
  %pairOne = add i32 %pair, 1
  %m1p = getelementptr [4 x float], [4 x float]* @mixed, i32 0, i32 %pairOne
  %m1 = load float, float* %m1p, align 4
  %extraIndex = xor i32 %inner, 2
  %extraPointer = getelementptr [4 x float], [4 x float]* @mixed, i32 0, i32 %extraIndex
  %mixedExtra = load float, float* %extraPointer, align 4

  %partialBase = mul i32 %inner, 3
  %partialPtr = getelementptr [6 x float], [6 x float]* @partial, i32 0, i32 %partialBase
  %incomplete = load float, float* %partialPtr, align 4

  %outR = fadd float %r, %w0
  %sumG = fadd float %g, %w1
  %outG = fadd float %sumG, %m0
  %sumB = fadd float %z, %w2
  %outB = fadd float %sumB, %m1
  %alpha = fadd float %required, %mixedExtra
  %outA = fadd float %alpha, %incomplete
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %outR)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %outG)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %outB)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %outA)
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
