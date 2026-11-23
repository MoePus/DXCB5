# Distributed under the University of Illinois Open Source License.
add_llvm_library(DXCB5Backend
  lib/Emit/Compiler.cpp
  lib/Emit/CompileSession.cpp
  lib/Emit/DxbcParts.cpp
  lib/Emit/DxbcWriter.cpp
  lib/Emit/MirText.cpp
  lib/Emit/Oracle.cpp
  lib/ControlFlow/ControlFlow.cpp
  lib/ControlFlow/ControlFlowDispatch.cpp
  lib/ControlFlow/ControlFlowMovPeephole.cpp
  lib/ControlFlow/ControlFlowNormalize.cpp
  lib/ControlFlow/ControlFlowPeephole.cpp
  lib/ControlFlow/ControlFlowPostRa.cpp
  lib/ControlFlow/ControlFlowRegions.cpp
  lib/ControlFlow/ParallelCopy.cpp
  lib/Frontend/Import.cpp
  lib/Frontend/LoopRegionValues.cpp
  lib/Frontend/LowerDxilOps.cpp
  lib/Frontend/LowerDxilResources.cpp
  lib/Frontend/LowerLocalMemory.cpp
  lib/Frontend/LowerToSsaMir.cpp
  lib/Frontend/LowerToSsaMirInsts.cpp
  lib/Frontend/PackLocalConstants.cpp
  lib/Mir/Analysis/DemandedLanes.cpp
  lib/Mir/Analysis/LaneAnalysis.cpp
  lib/Mir/Analysis/LaneValueFacts.cpp
  lib/Mir/Combine/CombineBitfield.cpp
  lib/Mir/Combine/CombineFloat.cpp
  lib/Mir/Combine/CombineFloatAdd.cpp
  lib/Mir/Combine/CombineGlobal.cpp
  lib/Mir/Combine/CombineInt.cpp
  lib/Mir/Combine/CombineSelect.cpp
  lib/Mir/Combine/CopyProp.cpp
  lib/Mir/Combine/SelectDots.cpp
  lib/Mir/Combine/SelectDotsHoriz.cpp
  lib/Mir/Combine/WorklistCombine.cpp
  lib/Mir/Memory/IndexableCopy.cpp
  lib/Mir/Memory/IndexablePack.cpp
  lib/Mir/Memory/IndexablePlan.cpp
  lib/Mir/Memory/IndexableSsa.cpp
  lib/Mir/Memory/IndexableTemps.cpp
  lib/Mir/OptColdSink.cpp
  lib/Mir/OptFoldOutputs.cpp
  lib/Mir/OptGvnDce.cpp
  lib/Mir/OptIfConvert.cpp
  lib/Mir/OptLicm.cpp
  lib/Mir/OptLoopPeel.cpp
  lib/Mir/OptPipeline.cpp
  lib/Mir/OptPressureShrink.cpp
  lib/Mir/OptSlpTree.cpp
  lib/Mir/OptSlpVectorize.cpp
  lib/Mir/OutOfSsa.cpp
  lib/Mir/SsaAnalysis.cpp
  lib/Mir/SsaMir.cpp
  lib/RegAlloc/LaneAllocator.cpp
  lib/RegAlloc/LaneInterference.cpp
  lib/RegAlloc/Liveness.cpp
  lib/RegAlloc/RegisterAllocation.cpp
  lib/RegAlloc/VerifyAllocation.cpp
  lib/Spec/DxilMathConstEval.cpp
  lib/Spec/SpecCrossStage.cpp
  lib/Spec/SpecDriver.cpp
  lib/Spec/SpecCBufferCompact.cpp
  lib/Spec/SpecCBufferLayout.cpp
  lib/Spec/SpecHash.cpp
  lib/Spec/SpecJson.cpp
  lib/Spec/SpecReflect.cpp
  lib/Spec/SpecTextureFold.cpp
  lib/Spec/SpecTransform.cpp
)

target_include_directories(DXCB5Backend
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib"
)
target_link_libraries(DXCB5Backend PRIVATE
  ShaderBinary LLVMAnalysis LLVMBitReader LLVMBitWriter LLVMDXIL LLVMDxilContainer LLVMDxilHash LLVMDxilRootSignature LLVMInstCombine LLVMScalarOpts LLVMTransformUtils
)

add_llvm_executable(dxcb5 tools/Main.cpp)
target_include_directories(dxcb5 PRIVATE
  "${LLVM_MAIN_SRC_DIR}/tools/clang/tools"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dxcb5 PRIVATE dxclib dxcompiler)
install(TARGETS dxcb5 RUNTIME DESTINATION bin)

add_library(DXCB5MinHook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c
)
target_include_directories(DXCB5MinHook PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include")
target_compile_options(DXCB5MinHook PRIVATE /w)

add_library(DXCB5UnityHook SHARED
  unity-hook/UnityHookMain.cpp
)
target_include_directories(DXCB5UnityHook PRIVATE
  "${LLVM_MAIN_SRC_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/unity-hook")
target_compile_options(DXCB5UnityHook PRIVATE /EHsc)
target_link_libraries(DXCB5UnityHook PRIVATE DXCB5MinHook)
set_target_properties(DXCB5UnityHook PROPERTIES
  OUTPUT_NAME "dxcb5_unity_hook"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/unity-hook"
)

add_library(DXCB5CgBatchPlugin SHARED
  unity-hook/CgBatchPlugin.cpp
  unity-hook/UnityHlslAdapter.cpp
)
target_include_directories(DXCB5CgBatchPlugin PRIVATE
  "${LLVM_MAIN_SRC_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/unity-hook")
target_compile_options(DXCB5CgBatchPlugin PRIVATE /EHsc)
set_target_properties(DXCB5CgBatchPlugin PROPERTIES
  OUTPUT_NAME "dxcb5_cgbatch_plugin"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/unity-hook"
)

add_executable(DXCB5UnityHookTest tests/UnityHookTest.cpp)
target_include_directories(DXCB5UnityHookTest PRIVATE
  "${LLVM_MAIN_SRC_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/unity-hook")
target_compile_options(DXCB5UnityHookTest PRIVATE /EHsc)
target_link_libraries(DXCB5UnityHookTest PRIVATE DXCB5MinHook d3d11 d3dcompiler)
add_test(NAME DXCB5UnityHook COMMAND DXCB5UnityHookTest
  "$<TARGET_FILE:DXCB5UnityHook>"
  "$<TARGET_FILE:DXCB5CgBatchPlugin>"
  "$<TARGET_FILE:dxcompiler>"
  "${CMAKE_CURRENT_BINARY_DIR}/unity-hook-test")

add_executable(DXCB5ParallelCopyTest tests/ParallelCopyTest.cpp lib/ControlFlow/ParallelCopy.cpp)
target_include_directories(DXCB5ParallelCopyTest PRIVATE include)
enable_testing()
add_executable(DXCB5CompileSessionTest tests/CompileSessionTest.cpp)
target_compile_options(DXCB5CompileSessionTest PRIVATE /EHsc)
target_link_libraries(DXCB5CompileSessionTest PRIVATE dxcompiler)
add_test(NAME DXCB5CompileSession COMMAND DXCB5CompileSessionTest)
add_executable(DXCB5SpecApiTest tests/SpecApiTest.cpp)
target_include_directories(DXCB5SpecApiTest PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/unity-hook")
target_compile_options(DXCB5SpecApiTest PRIVATE /EHsc)
target_link_libraries(DXCB5SpecApiTest PRIVATE dxcompiler d3dcompiler)
add_test(NAME DXCB5SpecApi COMMAND DXCB5SpecApiTest
  "$<TARGET_FILE:DXCB5CgBatchPlugin>"
  "$<TARGET_FILE:dxcompiler>"
  "${CMAKE_CURRENT_BINARY_DIR}/spec-api-test")
add_executable(DXCB5LoopRegionValuesTest tests/LoopRegionValuesTest.cpp)
target_link_libraries(DXCB5LoopRegionValuesTest PRIVATE DXCB5Backend LLVMAsmParser LLVMDxcSupport)
add_test(NAME DXCB5LoopRegionValues COMMAND DXCB5LoopRegionValuesTest)
add_executable(DXCB5PhiIncomingLayoutTest tests/PhiIncomingLayoutTest.cpp)
target_link_libraries(DXCB5PhiIncomingLayoutTest PRIVATE DXCB5Backend LLVMIRReader d3dcompiler)
add_test(NAME DXCB5PhiIncomingLayout COMMAND DXCB5PhiIncomingLayoutTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/PhiIncomingLayout.ll")
add_test(NAME DXCB5PhiIncomingSelect COMMAND DXCB5PhiIncomingLayoutTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/PhiIncomingSelect.ll" select)
add_executable(DXCB5DirectDxilTest tests/DirectDxilTest.cpp)
target_link_libraries(DXCB5DirectDxilTest PRIVATE DXCB5Backend LLVMIRReader d3dcompiler)
add_test(NAME DXCB5DirectArithmetic COMMAND DXCB5DirectDxilTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/DirectArithmetic.ll" arithmetic)
add_test(NAME DXCB5DirectLoopRecurrenceRgb COMMAND DXCB5DirectDxilTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/DirectLoopRecurrenceRgb.ll" direct-loop-rgb)
add_test(NAME DXCB5DirectLoopRecurrenceGuarded COMMAND DXCB5DirectDxilTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/DirectLoopRecurrenceGuarded.ll" guarded-loop-rgb)
add_test(NAME DXCB5DirectLoopRecurrenceReenter COMMAND DXCB5DirectDxilTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/DirectLoopRecurrenceReenter.ll" reenter-loop-rgb)
add_test(NAME DXCB5DirectContract COMMAND DXCB5DirectDxilTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/DirectContract.ll" contract)
add_test(NAME DXCB5DirectDot COMMAND DXCB5DirectDxilTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/DirectDot.ll" dot)
add_test(NAME DXCB5DirectSharedDotSources COMMAND DXCB5DirectDxilTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/DirectSharedDotSources.ll" shared-dot-sources)
add_test(NAME DXCB5DirectUniformDots COMMAND DXCB5DirectDxilTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/DirectUniformDots.ll" uniform-dots)
add_test(NAME DXCB5DirectPhi COMMAND DXCB5DirectDxilTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/DirectPhi.ll" phi)
add_test(NAME DXCB5DirectWave COMMAND DXCB5DirectDxilTest
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/DirectWave.ll" wave)
foreach(test ControlFlow MirText RegisterAllocation SsaMirOpt)
  add_executable(DXCB5${test}Test tests/${test}Test.cpp)
  target_include_directories(DXCB5${test}Test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib")
  target_link_libraries(DXCB5${test}Test PRIVATE DXCB5Backend)
  add_test(NAME DXCB5${test} COMMAND DXCB5${test}Test)
endforeach()
add_test(NAME DXCB5ParallelCopy COMMAND DXCB5ParallelCopyTest)
add_test(NAME DXCB5Frontend COMMAND "${CMAKE_COMMAND}"
  "-DDXCB5=$<TARGET_FILE:dxcb5>" "-DDXC=$<TARGET_FILE:dxc>"
  "-DWARP_TEST=$<TARGET_FILE:DXCB5WarpABITest>"
  "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/frontend-test"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/FrontendTest.cmake")

add_executable(DXCB5WarpABITest tests/WarpABITest.cpp)
target_compile_options(DXCB5WarpABITest PRIVATE /EHsc)
target_link_libraries(DXCB5WarpABITest PRIVATE d3d11 d3dcompiler)
add_test(NAME DXCB5WarpABI COMMAND "${CMAKE_COMMAND}"
  "-DWARP_TEST=$<TARGET_FILE:DXCB5WarpABITest>" "-DDXCB5=$<TARGET_FILE:dxcb5>"
  "-DDIRECT_TEST=$<TARGET_FILE:DXCB5DirectDxilTest>"
  "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/warp-abi-test"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/WarpABITest.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/lean/LeanGate.cmake")
