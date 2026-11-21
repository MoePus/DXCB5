; The hit path retries before reaching the outer latch. Each retry must add
; its contribution to the same outer-header state. Commit only the last hit.
; At x=4.5, 5.5, 6.5, 7.5, RGB is [0,0,0], [4,8,12], [18,6,12], [24,8,16].
; The alpha output is x.
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

define void @shade() {
entry:
  %x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %pixel = fptoui float %x to i32
  %has.hit = icmp ne i32 %pixel, 4
  %rotation = and i32 %pixel, 2
  %has.rotation = icmp ne i32 %rotation, 0
  br label %header

header:
  %ax = phi float [ 0.000000e+00, %entry ], [ %lx, %latch ]
  %ay = phi float [ 0.000000e+00, %entry ], [ %ly, %latch ]
  %az = phi float [ 0.000000e+00, %entry ], [ %lz, %latch ]
  %outer = phi i32 [ 0, %entry ], [ %next.outer, %latch ]
  br i1 %has.hit, label %bypass, label %latch

bypass:
  %try = phi i32 [ 0, %header ], [ %next.try, %hit ]
  %distance = and i32 %pixel, 3
  %distance.f = uitofp i32 %distance to float
  %bx = fadd float %distance.f, 1.000000e+00
  %by = fmul float %bx, 2.000000e+00
  %bz = fmul float %bx, 3.000000e+00
  br i1 %has.rotation, label %rotate, label %hit

rotate:
  %rx = fadd float %bz, 0.000000e+00
  %ry = fadd float %bx, 0.000000e+00
  %rz = fadd float %by, 0.000000e+00
  br label %hit

hit:
  %cx = phi float [ %bx, %bypass ], [ %rx, %rotate ]
  %cy = phi float [ %by, %bypass ], [ %ry, %rotate ]
  %cz = phi float [ %bz, %bypass ], [ %rz, %rotate ]
  %ux = fadd fast float %cx, %ax
  %uy = fadd fast float %cy, %ay
  %uz = fadd fast float %cz, %az
  %next.try = add nuw i32 %try, 1
  %retry = icmp ult i32 %next.try, 2
  br i1 %retry, label %bypass, label %latch

latch:
  %lx = phi float [ %ax, %header ], [ %ux, %hit ]
  %ly = phi float [ %ay, %header ], [ %uy, %hit ]
  %lz = phi float [ %az, %header ], [ %uz, %hit ]
  %next.outer = add nuw i32 %outer, 1
  %continue = icmp ult i32 %next.outer, 2
  br i1 %continue, label %header, label %exit

exit:
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %lx)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %ly)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %lz)
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
