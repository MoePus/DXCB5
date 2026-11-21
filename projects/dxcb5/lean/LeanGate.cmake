# Distributed under the University of Illinois Open Source License.
set(DXCB5_LAKE "" CACHE STRING "Absolute path to native Windows x64 Lake for the DXCB5 Lean gate")
if(DXCB5_LAKE)
  if(NOT IS_ABSOLUTE "${DXCB5_LAKE}" OR NOT EXISTS "${DXCB5_LAKE}")
    message(FATAL_ERROR "DXCB5_LAKE must be an absolute path to an existing lake.exe")
  endif()
  add_test(NAME DXCB5LeanParallelCopy
    COMMAND "${CMAKE_COMMAND}" "-DLAKE=${DXCB5_LAKE}"
      "-DPROOF_DIR=${CMAKE_CURRENT_LIST_DIR}"
      "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/lean-gate"
      -P "${CMAKE_CURRENT_LIST_DIR}/CheckProof.cmake")
endif()
