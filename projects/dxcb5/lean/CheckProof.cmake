# Distributed under the University of Illinois Open Source License.
file(MAKE_DIRECTORY "${BUILD_DIR}")
foreach(INPUT lean-toolchain lakefile.toml lake-manifest.json ParallelCopy.lean)
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
  RESULT_VARIABLE status)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "DXCB5 parallel-copy Lean proof failed (${status})")
endif()
