# Distributed under the University of Illinois Open Source License.

file(MAKE_DIRECTORY "${TEST_DIR}")
set(source "${CMAKE_CURRENT_LIST_DIR}/Frontend.hlsl")
set(vertex "${TEST_DIR}/linked vertex.dxbc")
set(pixel "${TEST_DIR}/pixel.dxbc")
set(dxil "${TEST_DIR}/pixel.dxil")
set(unused "${TEST_DIR}/unexpected.dxbc")
file(REMOVE "${vertex}" "${pixel}" "${dxil}" "${unused}")

function(expect_dxcb5_error message)
  execute_process(COMMAND "${DXCB5}" "${source}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  string(FIND "${error}" "${message}" match)
  if(result STREQUAL "0" OR match EQUAL -1 OR EXISTS "${unused}")
    message(FATAL_ERROR "Expected DXCB5 failure '${message}': ${output}${error}")
  endif()
endfunction()

function(compile_shader tool target output expected_chunk)
  execute_process(COMMAND "${tool}" "${source}" -E shade -T "${target}"
    -Fo "${output}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT result STREQUAL "0" OR NOT EXISTS "${output}")
    message(FATAL_ERROR "${target} compile failed: ${stdout}${stderr}")
  endif()
  file(READ "${output}" bytes HEX)
  if(NOT bytes MATCHES "^44584243" OR NOT bytes MATCHES "${expected_chunk}")
    message(FATAL_ERROR "${target} wrote the wrong shader bytecode to ${output}")
  endif()
endfunction()

# DXCB5 uses its own backend even when a DXIL validator path is set.
set(ENV{DXC_DXIL_DLL_PATH} "invalid-relative-validator.dll")
expect_dxcb5_error("vs_5_0 or ps_5_0" -T cs_5_0 -Fo "${unused}")
expect_dxcb5_error("requires -Fo" -T vs_5_0)
expect_dxcb5_error("only accepts --vs-dxbc"
  -T vs_5_0 -Fo "${unused}" --vs-dxbc "${vertex}")
expect_dxcb5_error("does not support -Fc"
  -T vs_5_0 -Fo "${unused}" -Fc "${TEST_DIR}/unused.asm")
expect_dxcb5_error("does not support -Zi" -T vs_5_0 -Fo "${unused}" -Zi)
expect_dxcb5_error("does not support -link" -link -Fo "${unused}")
expect_dxcb5_error("could not read --vs-dxbc"
  -T ps_5_0 -Fo "${unused}" --vs-dxbc "${vertex}")

function(verify_dxbc vs ps)
  execute_process(COMMAND "${WARP_TEST}" verify-dxbc "${vs}" "${ps}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "WARP/disasm verify failed for ${ps}:\n${stdout}${stderr}")
  endif()
endfunction()

compile_shader("${DXCB5}" vs_5_0 "${vertex}" "(53484452|53484558)"
  -D EXPECTED_MAJOR=5 -D EXPECT_VERTEX=1)
compile_shader("${DXCB5}" ps_5_0 "${TEST_DIR}/pixel_standalone.dxbc"
  "(53484452|53484558)" -D EXPECTED_MAJOR=5)
verify_dxbc("${vertex}" "${TEST_DIR}/pixel_standalone.dxbc" "ps_5_0")
compile_shader("${DXCB5}" ps_5_0 "${pixel}" "(53484452|53484558)"
  -D EXPECTED_MAJOR=5 --vs-dxbc "${vertex}")
verify_dxbc("${vertex}" "${pixel}" "ps_5_0")
compile_shader("${DXCB5}" ps_5_0 "${TEST_DIR}/pixel_getdim.dxbc" "(53484452|53484558)"
  -D EXPECTED_MAJOR=5 -D TEST_GET_DIMENSIONS=1 --vs-dxbc "${vertex}")
verify_dxbc("${vertex}" "${TEST_DIR}/pixel_getdim.dxbc"
  "dcl_resource_buffer" "dcl_resource_texture2dms(4)"
  "dcl_resource_texture2dms(0)" "dcl_resource_texture2dmsarray(4)"
  "bufinfo" "resinfo" "sampleinfo_uint")
compile_shader("${DXCB5}" ps_5_0 "${TEST_DIR}/pixel_getdim_od.dxbc" "(53484452|53484558)"
  -Od -D EXPECTED_MAJOR=5 -D TEST_GET_DIMENSIONS=1 --vs-dxbc "${vertex}")
verify_dxbc("${vertex}" "${TEST_DIR}/pixel_getdim_od.dxbc"
  "bufinfo" "resinfo" "sampleinfo_uint")
compile_shader("${DXCB5}" ps_5_0 "${TEST_DIR}/pixel_xtemp.dxbc" "(53484452|53484558)"
  -D EXPECTED_MAJOR=5 -D TEST_INDEXABLE_TEMP=1 --vs-dxbc "${vertex}")
verify_dxbc("${vertex}" "${TEST_DIR}/pixel_xtemp.dxbc" "dcl_indexableTemp")
compile_shader("${DXCB5}" ps_5_0 "${TEST_DIR}/pixel_inline_switch_od.dxbc" "(53484452|53484558)"
  -Od -D EXPECTED_MAJOR=5 -D TEST_INLINE_SWITCH_PRUNE=1 --vs-dxbc "${vertex}")
verify_dxbc("${vertex}" "${TEST_DIR}/pixel_inline_switch_od.dxbc" "ps_5_0")
compile_shader("${DXCB5}" ps_5_0 "${TEST_DIR}/pixel_inline_switch_o3.dxbc" "(53484452|53484558)"
  -O3 -D EXPECTED_MAJOR=5 -D TEST_INLINE_SWITCH_PRUNE=1 --vs-dxbc "${vertex}")
verify_dxbc("${vertex}" "${TEST_DIR}/pixel_inline_switch_o3.dxbc" "ps_5_0")
expect_dxcb5_error("incompatible vertex output for pixel input COLOR0"
  -T ps_5_0 -E shade -Fo "${unused}" -D EXPECTED_MAJOR=5
  -D INCOMPATIBLE_COLOR=1 --vs-dxbc "${vertex}")
expect_dxcb5_error("unsupported"
  -T ps_5_0 -E shade -Fo "${unused}" -D EXPECTED_MAJOR=5
  -D TEST_DOUBLE=1 --vs-dxbc "${vertex}")

unset(ENV{DXC_DXIL_DLL_PATH})
compile_shader("${DXC}" ps_6_0 "${dxil}" "4458494c" -D EXPECTED_MAJOR=6)
