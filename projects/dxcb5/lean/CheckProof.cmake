# Distributed under the University of Illinois Open Source License.
file(MAKE_DIRECTORY "${BUILD_DIR}")
set(PROOF_MODULES
  ParallelCopy.lean OpcodeSpec.lean LaneDomain.lean
  RewriteRules.lean RegAllocValidator.lean Placement.lean)
foreach(INPUT lean-toolchain lakefile.toml lake-manifest.json
              ${PROOF_MODULES} Codegen.lean)
  configure_file("${PROOF_DIR}/${INPUT}" "${BUILD_DIR}/${INPUT}" COPYONLY)
endforeach()

file(READ "${BUILD_DIR}/lean-toolchain" toolchain)
string(STRIP "${toolchain}" toolchain)
if(NOT toolchain MATCHES "^leanprover/lean4:v([0-9]+)[.]([0-9]+)[.]([0-9]+)$")
  message(FATAL_ERROR "Expected a pinned Lean release, got '${toolchain}'")
endif()
set(expected "${CMAKE_MATCH_1}[.]${CMAKE_MATCH_2}[.]${CMAKE_MATCH_3}")

execute_process(COMMAND "${LAKE}" env lean --version WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE version ERROR_VARIABLE error)
if(NOT status STREQUAL "0" OR
   NOT version MATCHES "^Lean [(]version ${expected}, x86_64-[^,]*windows[^,]*,")
  message(FATAL_ERROR "DXCB5 proof requires native Windows x64 ${toolchain}\n${version}${error}")
endif()

execute_process(COMMAND "${LAKE}" build WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE build_out ERROR_VARIABLE build_err)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "DXCB5 Lean proof build failed (${status})\n${build_out}\n${build_err}")
endif()
if(build_out MATCHES "declaration uses 'sorry'" OR
   build_err MATCHES "declaration uses 'sorry'")
  message(FATAL_ERROR "DXCB5 Lean proof must not use 'sorry'\n${build_out}\n${build_err}")
endif()

foreach(PROOF_FILE ${PROOF_MODULES})
  file(STRINGS "${BUILD_DIR}/${PROOF_FILE}" proof_lines)
  foreach(line IN LISTS proof_lines)
    if(line MATCHES "^[ \t]*--")
      continue()
    endif()
    if(line MATCHES "(^|[^A-Za-z0-9_])(axiom|native_decide|sorry)([^A-Za-z0-9_]|$)")
      message(FATAL_ERROR
        "DXCB5 Lean proof ${PROOF_FILE} contains forbidden escape hatch: ${line}")
    endif()
  endforeach()
endforeach()

function(check_generated_inc mode gen_name ref_rel)
  set(gen_inc "${BUILD_DIR}/${gen_name}")
  set(ref_inc "${PROOF_DIR}/${ref_rel}")
  execute_process(COMMAND "${LAKE}" exe dxcb5-codegen "${mode}" "${gen_inc}"
    WORKING_DIRECTORY "${BUILD_DIR}"
    RESULT_VARIABLE status OUTPUT_VARIABLE gen_out ERROR_VARIABLE gen_err)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "DXCB5 dxcb5-codegen ${mode} failed (${status})\n${gen_out}\n${gen_err}")
  endif()
  if(NOT EXISTS "${ref_inc}")
    message(FATAL_ERROR "Missing checked-in generated file: ${ref_inc}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files --ignore-eol "${gen_inc}" "${ref_inc}"
    RESULT_VARIABLE diff_status)
  if(NOT diff_status STREQUAL "0")
    message(FATAL_ERROR
      "${gen_name} is out of date with Lean spec.\n"
      "Re-run: lake exe dxcb5-codegen ${mode} ${ref_rel}")
  endif()
endfunction()

check_generated_inc("--opcodes" "GeneratedOpcodeDesc.inc" "../include/dxcb5/GeneratedOpcodeDesc.inc")
check_generated_inc("--rules" "GeneratedRules.inc" "../lib/Mir/Combine/GeneratedRules.inc")
