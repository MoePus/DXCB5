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

function(run_ps_parity slug entry)
  set(base_ps "${TEST_DIR}/${slug}-fxc.ps")
  set(nat_ps "${TEST_DIR}/${slug}-dxcb5.ps")
  run_checked("${WARP_TEST}" "compile-${slug}" "${source}" "${base_ps}")
  run_checked("${DXCB5}" "${source}" -E "${entry}" -T ps_5_0 ${ARGN}
    -Fo "${nat_ps}" --vs-dxbc "${native_vs}")
  run_checked("${WARP_TEST}" "draw-${slug}" "${baseline_vs}" "${base_ps}")
  run_checked("${WARP_TEST}" "draw-${slug}" "${native_vs}" "${nat_ps}")
endfunction()

function(run_direct_draw slug ll_file direct_mode draw_mode)
  set(out_dxbc "${TEST_DIR}/${slug}.dxbc")
  run_checked("${DIRECT_TEST}" "${CMAKE_CURRENT_LIST_DIR}/${ll_file}"
    "${direct_mode}" "${out_dxbc}" "${native_vs}")
  run_checked("${WARP_TEST}" "${draw_mode}" "${native_vs}" "${out_dxbc}")
endfunction()

run_ps_parity(resource-arrays PSResources)
run_ps_parity(quad-loop PSQuadLoop)

run_direct_draw(loop-lanes DirectLoopLanes.ll loop draw-loop-lanes)
run_direct_draw(retry-loop DirectRetryLoop.ll loop-retry draw-retry-loop)
run_direct_draw(sign DirectSign.ll sign draw-sign)
run_direct_draw(saturate DirectSaturate.ll saturate draw-saturate)
run_direct_draw(local-constant-tuples DirectLocalConstantTuples.ll
  local-constant-tuples draw-local-constant-tuples)

run_ps_parity(local-arrays PSLocalArrays -O3)
run_ps_parity(gather PSGather)

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

run_ps_parity(dimensions PSDimensions)
run_ps_parity(compat PSCompat -O3)
run_ps_parity(compat2 PSCompat2 -O3)
run_ps_parity(compat3 PSCompat3 -O3)
run_ps_parity(compat4 PSCompat4 -O3)

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

# Step 4 Pass Ablation Gate: individually disable each major optimization pass
# and verify WARP pixel output remains 100% consistent (no hidden pass coupling).
function(run_ablation entry draw_mode extra_flags pass_name)
  set(abl_ps "${TEST_DIR}/ablate-${entry}-${pass_name}.ps")
  execute_process(
    COMMAND "${DXCB5}" "${source}" -E "${entry}" -T ps_5_0 -O3 ${extra_flags}
      --disable-pass "${pass_name}"
      -Fo "${abl_ps}" --vs-dxbc "${native_vs}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Pass ablation (${entry}, disable=${pass_name}) compile failed (${result})\n${output}${error}")
  endif()
  run_checked("${WARP_TEST}" "${draw_mode}" "${native_vs}" "${abl_ps}")
endfunction()

foreach(pass copies lane-const if-convert gvn masked-dce dots algebraic slp dp2 loop-peel guard-discard licm cold-sink fold-outputs pressure-shrink)
  run_ablation(PSCorrectness draw-correctness "--nan-safe" "${pass}")
endforeach()

foreach(pass indexable-copy indexable-ssa indexable-temps indexable-pack)
  run_ablation(PSLocalArrays draw-local-arrays "" "${pass}")
endforeach()
