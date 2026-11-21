; Exercise dominance, same-block order, and exact source identity.
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.CBufRet.f32 = type { float, float, float, float }
%Constants = type { [4 x <4 x float>] }

define void @shade() {
entry:
  %input = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %condition = fcmp ogt float %input, 0.000000e+00
  %handle = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 1, i1 false)
  %row = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %handle, i32 1)
  %x = extractvalue %dx.types.CBufRet.f32 %row, 0
  %y = extractvalue %dx.types.CBufRet.f32 %row, 1
  %z = extractvalue %dx.types.CBufRet.f32 %row, 2
  %index = fptoui float %input to i32
  %relative = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %handle, i32 %index)
  %dynamic.x = extractvalue %dx.types.CBufRet.f32 %relative, 0
  ; Same-block DP2 and a separately loaded DP3 in a dominated block are reusable.
  %dot2 = call float @dx.op.dot2.f32(i32 54, float %x, float %y, float -0.000000e+00, float 2.000000e+00)
  %same = call float @dx.op.dot2.f32(i32 54, float %x, float %y, float -0.000000e+00, float 2.000000e+00)
  %dot3 = call float @dx.op.dot3.f32(i32 55, float %x, float %y, float %z, float 1.000000e+00, float 2.000000e+00, float 3.000000e+00)
  ; Signed zero, swizzle and precision use different keys.
  %positive = call float @dx.op.dot2.f32(i32 54, float %x, float %y, float 0.000000e+00, float 2.000000e+00)
  %swizzle = call float @dx.op.dot2.f32(i32 54, float %y, float %x, float -0.000000e+00, float 2.000000e+00)
  %precise = call float @dx.op.dot2.f32(i32 54, float %x, float %y, float -0.000000e+00, float 2.000000e+00), !dx.precise !13
  ; Relative indexing and varying input remain local computations.
  %relative0 = call float @dx.op.dot2.f32(i32 54, float %dynamic.x, float %y, float 3.000000e+00, float 4.000000e+00)
  %relative1 = call float @dx.op.dot2.f32(i32 54, float %dynamic.x, float %y, float 3.000000e+00, float 4.000000e+00)
  %varying0 = call float @dx.op.dot2.f32(i32 54, float %input, float %y, float 5.000000e+00, float 6.000000e+00)
  %varying1 = call float @dx.op.dot2.f32(i32 54, float %input, float %y, float 5.000000e+00, float 6.000000e+00)
  %ab = fadd float %dot2, %same
  %cd = fadd float %positive, %swizzle
  %sum = fadd float %ab, %cd
  %rel = fsub float %relative0, %relative1
  %varying = fsub float %varying0, %varying1
  %precisum = fadd float %precise, %rel
  %combined = fadd float %precisum, %varying
  br i1 %condition, label %left, label %right

left:
  %reload = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %handle, i32 1)
  %xx = extractvalue %dx.types.CBufRet.f32 %reload, 0
  %yy = extractvalue %dx.types.CBufRet.f32 %reload, 1
  %zz = extractvalue %dx.types.CBufRet.f32 %reload, 2
  %dominated = call float @dx.op.dot3.f32(i32 55, float %xx, float %yy, float %zz, float 1.000000e+00, float 2.000000e+00, float 3.000000e+00)
  %left.scale = fadd float %dominated, %dot3
  ; Sibling paths retain their own definitions.
  %left.dot4 = call float @dx.op.dot4.f32(i32 56, float %x, float %y, float %z, float 1.000000e+00, float 1.000000e+00, float 2.000000e+00, float 3.000000e+00, float 4.000000e+00)
  %left.value = fmul float %left.dot4, %left.scale
  br label %join

right:
  %right.dot4 = call float @dx.op.dot4.f32(i32 56, float %x, float %y, float %z, float 1.000000e+00, float 1.000000e+00, float 2.000000e+00, float 3.000000e+00, float 4.000000e+00)
  %right.value = fadd float %right.dot4, %dot3
  br label %join

join:
  %result = phi float [ %left.value, %left ], [ %right.value, %right ]
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %result)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %sum)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %combined)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float 1.000000e+00)
  ret void
}

declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) nounwind readnone
declare %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %dx.types.Handle, i32) nounwind readonly
declare float @dx.op.dot2.f32(i32, float, float, float, float) nounwind readnone
declare float @dx.op.dot3.f32(i32, float, float, float, float, float, float) nounwind readnone
declare float @dx.op.dot4.f32(i32, float, float, float, float, float, float, float, float) nounwind readnone
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
!13 = !{i32 1}
