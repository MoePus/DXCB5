// Distributed under the University of Illinois Open Source License.
#include "dxcb5/Compiler.h"
#include "dxcb5/ControlFlow.h"
#include "dxcb5/DxbcWriter.h"
#include "dxcb5/RegisterAllocation.h"
#include "Frontend/Import.h"
#include "Frontend/LowerToSsaMir.h"
#include "Mir/OptPipeline.h"
#include "Mir/OutOfSsa.h"
#include "Mir/SsaMir.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

HRESULT dxcb5::Compile(llvm::Module &Module,
                       llvm::ArrayRef<SignatureElement> VertexOutput,
                       llvm::raw_ostream &Output, llvm::raw_ostream &Diagnostics,
                       llvm::ArrayRef<VectorPhiGroup> PhiGroups,
                       bool NanSafe) {
  auto &Dxil = Module.GetOrCreateDxilModule();
  MirProgram Program;
  Interface Layout;
  std::string Error;
  std::vector<uint8_t> Bytecode;
  SsaProgram Ssa(Program);
  Ssa.NanSafe = NanSafe;
  std::vector<FlowBlock> Blocks;
  if (!ImportInterface(Dxil, VertexOutput, Program, Layout, Error) ||
      !LowerToSsaMir(Dxil, Layout, Ssa, Error, PhiGroups)) {
    Diagnostics << "error: " << Error << '\n';
    return E_FAIL;
  }
  OptimizeSsaMir(Ssa);
  LowerOutOfSsa(Ssa, Blocks);
  if (!AllocateRegisters(Blocks, Program, Error) ||
      !LowerControlFlow(Blocks, Program, Error) ||
      !WriteDxbc(Program, Bytecode, Error)) {
    Diagnostics << "error: " << Error << '\n';
    return E_FAIL;
  }
  Output.write(reinterpret_cast<const char *>(Bytecode.data()), Bytecode.size());
  return S_OK;
}
