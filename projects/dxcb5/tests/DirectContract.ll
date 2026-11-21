; Fixed VS 6.0 DXIL derived from DXC output for an input position with its x lane changed.
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

define void @shade() {
  %x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %y = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
  %z = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 2, i32 undef)
  %w = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 3, i32 undef)
  %product = fmul fast float %x, %y
  %out = fadd fast float %product, %z
  %strict_product = fmul float %x, %z
  %strict_sum = fadd float %strict_product, %w
  %shared_product = fmul fast float %x, %w
  %uses_shared = fadd fast float %shared_product, %z
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %out)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %strict_sum)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %uses_shared)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %shared_product)
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
!8 = !{!9}
!9 = !{i32 0, !"SV_Position", i8 9, i8 3, !6, i8 4, i32 1, i8 4, i32 0, i8 0, !7}
