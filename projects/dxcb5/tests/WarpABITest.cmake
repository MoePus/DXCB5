# Distributed under the University of Illinois Open Source License.
file(MAKE_DIRECTORY "${TEST_DIR}")
set(source "${CMAKE_CURRENT_LIST_DIR}/WarpABI.hlsl")
set(baseline_vs "${TEST_DIR}/baseline.vs")
set(baseline_ps "${TEST_DIR}/baseline.ps")
set(native_vs "${TEST_DIR}/dxcb5.vs")
set(native_ps "${TEST_DIR}/dxcb5.ps")
set(mixed_ps "${TEST_DIR}/mixed.ps")

function(run_checked)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "${ARGV} failed (${result})\n${output}${error}")
  endif()
endfunction()

run_checked("${WARP_TEST}" compile "${source}" "${baseline_vs}" "${baseline_ps}")
run_checked("${DXCB5}" "${source}" -E VS -T vs_5_0 -Fo "${native_vs}")
run_checked("${DXCB5}" "${source}" -E PS -T ps_5_0 -Fo "${native_ps}"
  --vs-dxbc "${native_vs}")
run_checked("${DXCB5}" "${source}" -E PS -T ps_5_0 -Fo "${mixed_ps}"
  --vs-dxbc "${baseline_vs}")
run_checked("${WARP_TEST}" draw "${baseline_vs}" "${baseline_ps}")
run_checked("${WARP_TEST}" draw "${native_vs}" "${native_ps}")
run_checked("${WARP_TEST}" draw "${baseline_vs}" "${mixed_ps}")

set(discard_ps "${TEST_DIR}/discard.ps")
run_checked("${DXCB5}" "${source}" -E PS -T ps_5_0 -Fo "${discard_ps}"
  -D DXCB5_TEST_DISCARD=1 --vs-dxbc "${native_vs}")
run_checked("${WARP_TEST}" draw-discard "${native_vs}" "${discard_ps}")

set(resources_ps "${TEST_DIR}/resource-arrays.ps")
set(resources_baseline_ps "${TEST_DIR}/resource-arrays-baseline.ps")
run_checked("${WARP_TEST}" compile-resource-arrays "${source}" "${resources_baseline_ps}")
run_checked("${WARP_TEST}" draw-resource-arrays "${baseline_vs}" "${resources_baseline_ps}")
run_checked("${DXCB5}" "${source}" -E PSResources -T ps_5_0
  -Fo "${resources_ps}" --vs-dxbc "${native_vs}")
run_checked("${WARP_TEST}" draw-resource-arrays "${native_vs}" "${resources_ps}")

set(quad_loop_baseline "${TEST_DIR}/quad-loop-fxc.ps")
set(quad_loop_native "${TEST_DIR}/quad-loop-dxcb5.ps")
run_checked("${WARP_TEST}" compile-quad-loop "${source}" "${quad_loop_baseline}")
run_checked("${DXCB5}" "${source}" -E PSQuadLoop -T ps_5_0
  -Fo "${quad_loop_native}" --vs-dxbc "${native_vs}")
run_checked("${WARP_TEST}" draw-quad-loop "${baseline_vs}" "${quad_loop_baseline}")
run_checked("${WARP_TEST}" draw-quad-loop "${native_vs}" "${quad_loop_native}")

set(loop_lanes "${TEST_DIR}/loop-lanes.dxbc")
run_checked("${DIRECT_TEST}" "${CMAKE_CURRENT_LIST_DIR}/DirectLoopLanes.ll"
  loop "${loop_lanes}" "${native_vs}")
run_checked("${WARP_TEST}" draw-loop-lanes "${native_vs}" "${loop_lanes}")
set(retry_loop "${TEST_DIR}/retry-loop.dxbc")
run_checked("${DIRECT_TEST}" "${CMAKE_CURRENT_LIST_DIR}/DirectRetryLoop.ll"
  loop-retry "${retry_loop}" "${native_vs}")
run_checked("${WARP_TEST}" draw-retry-loop "${native_vs}" "${retry_loop}")

set(sign_shader "${TEST_DIR}/sign.dxbc")
run_checked("${DIRECT_TEST}" "${CMAKE_CURRENT_LIST_DIR}/DirectSign.ll"
  sign "${sign_shader}" "${native_vs}")
run_checked("${WARP_TEST}" draw-sign "${native_vs}" "${sign_shader}")

set(saturate_shader "${TEST_DIR}/saturate.dxbc")
run_checked("${DIRECT_TEST}" "${CMAKE_CURRENT_LIST_DIR}/DirectSaturate.ll"
  saturate "${saturate_shader}" "${native_vs}")
run_checked("${WARP_TEST}" draw-saturate "${native_vs}" "${saturate_shader}")

set(local_constant_tuples "${TEST_DIR}/local-constant-tuples.dxbc")
run_checked("${DIRECT_TEST}" "${CMAKE_CURRENT_LIST_DIR}/DirectLocalConstantTuples.ll"
  local-constant-tuples "${local_constant_tuples}" "${native_vs}")
run_checked("${WARP_TEST}" draw-local-constant-tuples "${native_vs}" "${local_constant_tuples}")

set(local_arrays_baseline "${TEST_DIR}/local-arrays-fxc.ps")
set(local_arrays_native "${TEST_DIR}/local-arrays-dxcb5.ps")
run_checked("${WARP_TEST}" compile-local-arrays "${source}" "${local_arrays_baseline}")
run_checked("${DXCB5}" "${source}" -E PSLocalArrays -T ps_5_0 -O3
  -Fo "${local_arrays_native}" --vs-dxbc "${native_vs}")
run_checked("${WARP_TEST}" draw-local-arrays "${baseline_vs}" "${local_arrays_baseline}")
run_checked("${WARP_TEST}" draw-local-arrays "${native_vs}" "${local_arrays_native}")

set(gather_baseline "${TEST_DIR}/gather-fxc.ps")
set(gather_native "${TEST_DIR}/gather-dxcb5.ps")
run_checked("${WARP_TEST}" compile-gather "${source}" "${gather_baseline}")
run_checked("${DXCB5}" "${source}" -E PSGather -T ps_5_0
  -Fo "${gather_native}" --vs-dxbc "${native_vs}")
run_checked("${WARP_TEST}" draw-gather "${baseline_vs}" "${gather_baseline}")
run_checked("${WARP_TEST}" draw-gather "${native_vs}" "${gather_native}")

set(correctness_baseline "${TEST_DIR}/correctness-fxc.ps")
set(correctness_o3 "${TEST_DIR}/correctness-dxcb5-o3.ps")
set(correctness_od "${TEST_DIR}/correctness-dxcb5-od.ps")
run_checked("${WARP_TEST}" compile-correctness "${source}" "${correctness_baseline}")
run_checked("${DXCB5}" "${source}" -E PSCorrectness -T ps_5_0 -O3 --nan-safe
  -Fo "${correctness_o3}" --vs-dxbc "${native_vs}")
run_checked("${DXCB5}" "${source}" -E PSCorrectness -T ps_5_0 -Od
  -Fo "${correctness_od}" --vs-dxbc "${native_vs}")
run_checked("${WARP_TEST}" draw-correctness "${baseline_vs}" "${correctness_baseline}")
run_checked("${WARP_TEST}" draw-correctness "${native_vs}" "${correctness_o3}")
run_checked("${WARP_TEST}" draw-correctness "${native_vs}" "${correctness_od}")

set(dimensions_baseline "${TEST_DIR}/dimensions-fxc.ps")
set(dimensions_native "${TEST_DIR}/dimensions-dxcb5.ps")
run_checked("${WARP_TEST}" compile-dimensions "${source}" "${dimensions_baseline}")
run_checked("${DXCB5}" "${source}" -E PSDimensions -T ps_5_0
  -Fo "${dimensions_native}" --vs-dxbc "${native_vs}")
run_checked("${WARP_TEST}" draw-dimensions "${baseline_vs}" "${dimensions_baseline}")
run_checked("${WARP_TEST}" draw-dimensions "${native_vs}" "${dimensions_native}")

set(compat_baseline "${TEST_DIR}/compat-fxc.ps")
set(compat_native "${TEST_DIR}/compat-dxcb5.ps")
run_checked("${WARP_TEST}" compile-compat "${source}" "${compat_baseline}")
run_checked("${DXCB5}" "${source}" -E PSCompat -T ps_5_0 -O3
  -Fo "${compat_native}" --vs-dxbc "${native_vs}")
run_checked("${WARP_TEST}" draw-compat "${baseline_vs}" "${compat_baseline}")
run_checked("${WARP_TEST}" draw-compat "${native_vs}" "${compat_native}")

set(compat2_baseline "${TEST_DIR}/compat2-fxc.ps")
set(compat2_native "${TEST_DIR}/compat2-dxcb5.ps")
run_checked("${WARP_TEST}" compile-compat2 "${source}" "${compat2_baseline}")
run_checked("${DXCB5}" "${source}" -E PSCompat2 -T ps_5_0 -O3
  -Fo "${compat2_native}" --vs-dxbc "${native_vs}")
run_checked("${WARP_TEST}" draw-compat2 "${baseline_vs}" "${compat2_baseline}")
run_checked("${WARP_TEST}" draw-compat2 "${native_vs}" "${compat2_native}")

set(compat3_baseline "${TEST_DIR}/compat3-fxc.ps")
set(compat3_native "${TEST_DIR}/compat3-dxcb5.ps")
run_checked("${WARP_TEST}" compile-compat3 "${source}" "${compat3_baseline}")
run_checked("${DXCB5}" "${source}" -E PSCompat3 -T ps_5_0 -O3
  -Fo "${compat3_native}" --vs-dxbc "${native_vs}")
run_checked("${WARP_TEST}" draw-compat3 "${baseline_vs}" "${compat3_baseline}")
run_checked("${WARP_TEST}" draw-compat3 "${native_vs}" "${compat3_native}")

set(compat4_baseline "${TEST_DIR}/compat4-fxc.ps")
set(compat4_native "${TEST_DIR}/compat4-dxcb5.ps")
run_checked("${WARP_TEST}" compile-compat4 "${source}" "${compat4_baseline}")
run_checked("${DXCB5}" "${source}" -E PSCompat4 -T ps_5_0 -O3
  -Fo "${compat4_native}" --vs-dxbc "${native_vs}")
run_checked("${WARP_TEST}" draw-compat4 "${baseline_vs}" "${compat4_baseline}")
run_checked("${WARP_TEST}" draw-compat4 "${native_vs}" "${compat4_native}")

function(expect_array_error define diagnostic)
  set(output "${TEST_DIR}/${define}.ps")
  file(REMOVE "${output}")
  execute_process(COMMAND "${DXCB5}" "${source}" -E PSResources
    -T ps_5_0 -D "${define}=1" -Fo "${output}" --vs-dxbc "${native_vs}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(result STREQUAL "0" OR NOT stderr MATCHES "${diagnostic}" OR EXISTS "${output}")
    message(FATAL_ERROR "Expected resource array error '${diagnostic}'\n${stdout}${stderr}")
  endif()
endfunction()
expect_array_error(DXCB5_TEST_DYNAMIC_SRV "constant binding index")
expect_array_error(DXCB5_TEST_DYNAMIC_SAMPLER "constant binding index")
expect_array_error(DXCB5_TEST_OVERFLOW_RANGE "finite binding range")
