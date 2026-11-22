// Distributed under the University of Illinois Open Source License.
// Single owner for Stage-1 LLVM/DXIL <-> DXCB5 pipeline contract metadata and
// pass creation.
#ifndef DXC_HLSL_DXCB5_CONTRACT_H
#define DXC_HLSL_DXCB5_CONTRACT_H

namespace llvm {

class ModulePass;

namespace dxcb5_contract {

constexpr const char *kPipelineMD = "dxcb5.pipeline";
constexpr const char *kStrictBranchMD = "dxcb5.strict_branch";
constexpr const char *kUniformMD = "dxcb5.uniform";
constexpr const char *kAutoBranchMD = "dxcb5.auto.branch";
constexpr const char *kFastMathMD = "dxcb5.fast_math";
constexpr const char *kSpecMetaMD = "dxcb5.spec.meta";
constexpr const char *kCBufferDefaultsMD = "dxcb5.cbuffer.defaults";
constexpr const char *kSpecBranchGlobalsMD = "dxcb5.spec.branch.globals";
constexpr const char *kUniformGlobalsMD = "dxcb5.uniform.globals";

} // namespace dxcb5_contract

ModulePass *createDxcb5PipelineContractPass();

} // namespace llvm

#endif
