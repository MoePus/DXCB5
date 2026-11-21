; One source edge computes arithmetic and the other computes scalar selects.
; Each producer writes a distinct lane originating from the same float3.
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.CBufRet.f32 = type { float, float, float, float }
%Constants = type { [4 x <4 x float>] }

define void @shade() {
entry:
  %x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %y = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
  %z = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 2, i32 undef)
  %handle = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 1, i1 false)
  %row1 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %handle, i32 1)
  %row2 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %handle, i32 2)
  %row3 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %handle, i32 3)
  %a = extractvalue %dx.types.CBufRet.f32 %row1, 0
  %b = extractvalue %dx.types.CBufRet.f32 %row2, 1
  %c = extractvalue %dx.types.CBufRet.f32 %row3, 2
  %condition = fcmp ogt float %x, 0.000000e+00
  br i1 %condition, label %single_use, label %select_edge

single_use:
  %a0 = fadd float %x, %a
  %a1 = fadd float %b, %y
  %a2 = fadd float %z, %c
  br label %join

select_edge:
  %choose0 = fcmp olt float %x, %b
  %choose1 = fcmp olt float %y, %c
  %choose2 = fcmp olt float %z, %a
  %b0 = select i1 %choose0, float %b, float %x
  %b1 = select i1 %choose1, float %y, float %c
  %b2 = select i1 %choose2, float %a, float %z
  br label %join

join:
  %p0 = phi float [ %a0, %single_use ], [ %b0, %select_edge ]
  %p1 = phi float [ %a1, %single_use ], [ %b1, %select_edge ]
  %p2 = phi float [ %a2, %single_use ], [ %b2, %select_edge ]
  %extraResult = phi float [ 0.000000e+00, %single_use ], [ 1.000000e+00, %select_edge ]
  %summary = call float @dx.op.dot3.f32(i32 55, float %p0, float %p1, float %p2,
    float 1.000000e+00, float 2.000000e+00, float 3.000000e+00)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %summary)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %p1)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %p2)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %extraResult)
  ret void
}

declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) nounwind readnone
declare %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %dx.types.Handle, i32) nounwind readonly
declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) nounwind readnone
declare void @dx.op.storeOutput.f32(i32, i32, i32, i8, float) nounwind
declare float @dx.op.dot3.f32(i32, float, float, float, float, float, float) nounwind readnone

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
