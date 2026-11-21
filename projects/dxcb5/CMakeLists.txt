# Distributed under the University of Illinois Open Source License.
add_llvm_library(DXCB5Backend
  lib/Emit/Compiler.cpp
  lib/Emit/CompileSession.cpp
  lib/Emit/DxbcParts.cpp
  lib/Emit/DxbcWriter.cpp
  lib/Emit/MirText.cpp
  lib/ControlFlow/ControlFlow.cpp
  lib/ControlFlow/ControlFlowDispatch.cpp
  lib/ControlFlow/ControlFlowMovPeephole.cpp
  lib/ControlFlow/ControlFlowNormalize.cpp
  lib/ControlFlow/ControlFlowPeephole.cpp
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
  lib/Mir/OptAlgebraic.cpp
  lib/Mir/OptCombineBitfield.cpp
  lib/Mir/OptCombineFloat.cpp
  lib/Mir/OptCombineFloatAdd.cpp
  lib/Mir/OptCombineInt.cpp
  lib/Mir/OptCombineSelect.cpp
  lib/Mir/OptCopyProp.cpp
  lib/Mir/OptDce.cpp
  lib/Mir/OptGlobalCombine.cpp
  lib/Mir/OptGvnDce.cpp
  lib/Mir/OptIfConvert.cpp
  lib/Mir/OptIndexableCopy.cpp
  lib/Mir/OptIndexablePack.cpp
  lib/Mir/OptIndexablePlan.cpp
  lib/Mir/OptIndexableSsa.cpp
  lib/Mir/OptIndexableTemps.cpp
  lib/Mir/OptLocalArrays.cpp
  lib/Mir/OptPipeline.cpp
  lib/Mir/OptSelectDots.cpp
  lib/Mir/OptSelectDotsHoriz.cpp
  lib/Mir/OptSlpTree.cpp
  lib/Mir/OptSlpVectorize.cpp
  lib/Mir/OutOfSsa.cpp
  lib/Mir/SsaMir.cpp
  lib/RegAlloc/LaneAllocator.cpp
  lib/RegAlloc/LaneInterference.cpp
  lib/RegAlloc/Liveness.cpp
  lib/RegAlloc/RegisterAllocation.cpp
)

target_include_directories(DXCB5Backend
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib"
)
target_link_libraries(DXCB5Backend PRIVATE
  ShaderBinary LLVMAnalysis LLVMDXIL LLVMDxilContainer LLVMDxilHash LLVMDxilRootSignature LLVMTransformUtils
)

add_llvm_executable(dxcb5 tools/Main.cpp)
target_include_directories(dxcb5 PRIVATE "${LLVM_MAIN_SRC_DIR}/tools/clang/tools")
target_link_libraries(dxcb5 PRIVATE dxclib dxcompiler)
install(TARGETS dxcb5 RUNTIME DESTINATION bin)

add_library(DXCB5UnityProxy SHARED
  unity-proxy/D3DCompilerProxy.cpp
  unity-proxy/D3DCompilerForward.cpp
  unity-proxy/UnityHlslAdapter.cpp
  unity-proxy/D3DCompiler_47.def
)
target_include_directories(DXCB5UnityProxy PRIVATE "${LLVM_MAIN_SRC_DIR}/include")
target_compile_options(DXCB5UnityProxy PRIVATE /EHsc)
set_target_properties(DXCB5UnityProxy PROPERTIES
  OUTPUT_NAME "D3DCompiler_47"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/unity-proxy"
)
add_executable(DXCB5UnityProxyTest tests/UnityProxyTest.cpp)
target_compile_options(DXCB5UnityProxyTest PRIVATE /EHsc)
target_link_libraries(DXCB5UnityProxyTest PRIVATE d3d11)
add_test(NAME DXCB5UnityProxy COMMAND DXCB5UnityProxyTest
  "$<TARGET_FILE:DXCB5UnityProxy>"
  "$ENV{SystemRoot}/System32/D3DCompiler_47.dll"
  "$<TARGET_FILE:dxcompiler>"
  "${CMAKE_CURRENT_BINARY_DIR}/unity-proxy-test")

add_executable(DXCB5ParallelCopyTest tests/ParallelCopyTest.cpp lib/ControlFlow/ParallelCopy.cpp)
target_include_directories(DXCB5ParallelCopyTest PRIVATE include)
enable_testing()
add_executable(DXCB5CompileSessionTest tests/CompileSessionTest.cpp)
target_compile_options(DXCB5CompileSessionTest PRIVATE /EHsc)
target_link_libraries(DXCB5CompileSessionTest PRIVATE dxcompiler)
add_test(NAME DXCB5CompileSession COMMAND DXCB5CompileSessionTest)
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
add_executable(DXCB5MoeToonWarp EXCLUDE_FROM_ALL tests/MoeToonWarp.cpp)
foreach(test DXCB5WarpABITest DXCB5MoeToonWarp)
  target_compile_options(${test} PRIVATE /EHsc)
  target_link_libraries(${test} PRIVATE d3d11 d3dcompiler)
endforeach()
add_test(NAME DXCB5WarpABI COMMAND "${CMAKE_COMMAND}"
  "-DWARP_TEST=$<TARGET_FILE:DXCB5WarpABITest>" "-DDXCB5=$<TARGET_FILE:dxcb5>"
  "-DDIRECT_TEST=$<TARGET_FILE:DXCB5DirectDxilTest>"
  "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/warp-abi-test"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/WarpABITest.cmake")

set(DXCB5_UNITY_PLUGIN_API "" CACHE PATH "Path to Unity Editor's PluginAPI headers")
if(DXCB5_UNITY_PLUGIN_API)
  add_library(DXCB5MoeToonUnity SHARED EXCLUDE_FROM_ALL
    tests/MoeToonWarp.cpp tests/MoeToonUnityPlugin.cpp)
  target_include_directories(DXCB5MoeToonUnity PRIVATE "${DXCB5_UNITY_PLUGIN_API}")
  target_compile_definitions(DXCB5MoeToonUnity PRIVATE DXCB5_UNITY_RENDER_PLUGIN)
  target_compile_options(DXCB5MoeToonUnity PRIVATE /EHsc /utf-8)
  target_link_libraries(DXCB5MoeToonUnity PRIVATE d3d11 d3dcompiler)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/lean/LeanGate.cmake")
