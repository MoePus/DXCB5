; The false edge goes straight to the join and must defer the static reads.
; The dynamic cbuffer read has a writable index and remains materialized.
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.CBufRet.f32 = type { float, float, float, float }
%Constants = type { [4 x <4 x float>] }

define void @shade() {
entry:
  %x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %w = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 3, i32 undef)
  %condition = fcmp fast ogt float %x, 0.000000e+00
  %handle = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 1, i1 false)
  %row1 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %handle, i32 1)
  %row2 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %handle, i32 2)
  %row1.y = extractvalue %dx.types.CBufRet.f32 %row1, 1
  %row2.z = extractvalue %dx.types.CBufRet.f32 %row2, 2
  %index = fptoui float %x to i32
  %dynamic = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %handle, i32 %index)
  %dynamic.y = extractvalue %dx.types.CBufRet.f32 %dynamic, 1
  br i1 %condition, label %other, label %join

other:
  br label %join

join:
  %p0 = phi float [ %row1.y, %entry ], [ 1.000000e+00, %other ]
  %p1 = phi float [ %row2.z, %entry ], [ 2.000000e+00, %other ]
  %p2 = phi float [ %w, %entry ], [ 3.000000e+00, %other ]
  %p3 = phi float [ %dynamic.y, %entry ], [ 4.000000e+00, %other ]
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %p0)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %p1)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %p2)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %p3)
  ret void
}

declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) nounwind readnone
declare %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %dx.types.Handle, i32) nounwind readonly
declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) nounwind readnone
declare void @dx.op.storeOutput.f32(i32, i32, i32, i8, float) nounwind

!dx.version = !{!0}
!dx.shaderModel = !{!1}
!dx.entryPoints = !{!2}
!dx.resources = !{!10}

!0 = !{i32 1, i32 0}
!1 = !{!"vs", i32 6, i32 0}
!2 = !{void ()* @shade, !"shade", !3, !10, null}
!3 = !{!4, !8, null}
!4 = !{!5}
!5 = !{i32 0, !"POSITION", i8 9, i8 0, !6, i8 0, i32 1, i8 4, i32 0, i8 0, !7}
!6 = !{i32 0}
!7 = !{i32 3, i32 15}
!8 = !{!9}
!9 = !{i32 0, !"SV_Position", i8 9, i8 3, !6, i8 4, i32 1, i8 4, i32 0, i8 0, !7}
!10 = !{null, null, !11, null}
!11 = !{!12}
!12 = !{i32 0, %Constants* undef, !"Constants", i32 0, i32 1, i32 1, i32 64, null}
