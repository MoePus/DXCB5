// Distributed under the University of Illinois Open Source License.
#include "dxcb5/Compiler.h"
#include "dxcb5/ControlFlow.h"
#include "dxcb5/DxbcWriter.h"
#include "dxcb5/InstructionInfo.h"
#include "dxcb5/RegisterAllocation.h"
#include "Emit/Oracle.h"
#include "Frontend/Import.h"
#include "Frontend/LowerToSsaMir.h"
#include "Mir/OptPipeline.h"
#include "Mir/OutOfSsa.h"
#include "Mir/SsaMir.h"
#include "RegAlloc/Liveness.h"
#include "dxc/Support/Global.h"
#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <array>
#include <exception>

namespace {
void PruneUnusedPixelInputs(dxcb5::MirProgram &Program) {
  if (Program.ShaderStage != dxcb5::Stage::Pixel || Program.Inputs.empty())
    return;
  std::array<uint8_t, 32> UsedLanes{};
  for (const auto &Inst : Program.Instructions) {
    const unsigned DestCount = dxcb5::InstructionDestinationCount(Inst.Opcode);
    uint8_t DestMask = 0;
    for (unsigned D = 0; D < DestCount && D < Inst.Operands.size(); ++D)
      if (Inst.Operands[D].Kind != dxcb5::OperandKind::Null)
        DestMask |= Inst.Operands[D].Mask;
    if (!DestMask)
      DestMask = 1u;
    for (size_t I = DestCount; I < Inst.Operands.size(); ++I) {
      const auto &Op = Inst.Operands[I];
      if (Op.Kind != dxcb5::OperandKind::Input)
        continue;
      if (Op.RelativeRegister != UINT32_MAX || Op.Index >= UsedLanes.size())
        return;
      const uint8_t SlotMask = dxcb5::InstructionSourceSlotMaskSm5(
          Inst.Opcode, DestMask, I - DestCount);
      UsedLanes[Op.Index] |= dxcb5::SwizzledReadLanes(Op.Swizzle, SlotMask);
    }
  }
  Program.Inputs.erase(
      std::remove_if(Program.Inputs.begin(), Program.Inputs.end(),
                     [&](const dxcb5::SignatureElement &Elem) {
                       return Elem.SystemValue == 0 &&
                              Elem.Register < UsedLanes.size() &&
                              (UsedLanes[Elem.Register] & Elem.Mask) == 0;
                     }),
      Program.Inputs.end());
  for (auto &Elem : Program.Inputs) {
    if (Elem.SystemValue == 0 && Elem.Register < UsedLanes.size())
      Elem.UsedMask = static_cast<uint8_t>(UsedLanes[Elem.Register] & Elem.Mask);
  }
}

void CollectCompileStats(const dxcb5::MirProgram &Program,
                         dxcb5::CompileStats &Out) {
  Out = {};
  Out.Instructions = static_cast<uint32_t>(Program.Instructions.size());
  Out.Temps = Program.TempCount;
  Out.IndexableTemps = static_cast<uint32_t>(Program.IndexableTemps.size());
  for (const auto &CB : Program.ConstantBuffers)
    if (CB.Name == "$Globals")
      Out.GlobalsBytes = CB.Size;
  for (const auto &Inst : Program.Instructions) {
    switch (Inst.Opcode) {
    case D3D10_SB_OPCODE_EXP:
    case D3D10_SB_OPCODE_LOG:
    case D3D10_SB_OPCODE_RSQ:
    case D3D10_SB_OPCODE_SQRT:
    case D3D11_SB_OPCODE_RCP:
    case D3D10_SB_OPCODE_SINCOS:
    case D3D10_SB_OPCODE_DIV:
      ++Out.SfuOps;
      break;
    case D3D10_SB_OPCODE_SAMPLE:
    case D3D10_SB_OPCODE_SAMPLE_B:
    case D3D10_SB_OPCODE_SAMPLE_C:
    case D3D10_SB_OPCODE_SAMPLE_C_LZ:
    case D3D10_SB_OPCODE_SAMPLE_D:
    case D3D10_SB_OPCODE_SAMPLE_L:
    case D3D10_SB_OPCODE_LD:
    case D3D10_SB_OPCODE_LD_MS:
    case D3D10_1_SB_OPCODE_GATHER4:
    case D3D11_SB_OPCODE_GATHER4_C:
    case D3D11_SB_OPCODE_GATHER4_PO:
    case D3D11_SB_OPCODE_GATHER4_PO_C:
    case D3D10_1_SB_OPCODE_LOD:
      ++Out.TexOps;
      break;
    case D3D10_SB_OPCODE_IF:
    case D3D10_SB_OPCODE_SWITCH:
    case D3D10_SB_OPCODE_LOOP:
    case D3D10_SB_OPCODE_BREAKC:
    case D3D10_SB_OPCODE_CONTINUEC:
    case D3D10_SB_OPCODE_RETC:
      ++Out.BranchOps;
      break;
    default:
      break;
    }
  }
}
} // namespace

HRESULT dxcb5::Compile(llvm::Module &Module,
                       llvm::ArrayRef<SignatureElement> VertexOutput,
                       llvm::raw_ostream &Output, llvm::raw_ostream &Diagnostics,
                       llvm::ArrayRef<VectorPhiGroup> PhiGroups,
                       bool NanSafe, CompileStats *Stats) {
  try {
    auto &Dxil = Module.GetOrCreateDxilModule();
    MirProgram Program;
    Interface Layout;
    std::string Error;
    std::vector<uint8_t> Bytecode;
    SsaProgram Ssa(Program);
    Ssa.NanSafe = NanSafe;
    std::vector<FlowBlock> Blocks;
    OracleSession Oracle;
    if (!ImportInterface(Dxil, VertexOutput, Program, Layout, Error) ||
        !LowerToSsaMir(Dxil, Layout, Ssa, Error, PhiGroups)) {
      Diagnostics << "error: " << Error << '\n';
      return E_FAIL;
    }
    Oracle.RecordSsaStage("00_lower_ssa", Ssa);
    OptimizeSsaMir(Ssa);
    if (!Ssa.Verify(Error, /*RequireNormalizedCfg=*/true)) {
      Diagnostics << "error: SSA MIR verification failed: " << Error << '\n';
      return E_FAIL;
    }
    const bool NeedMetrics = Stats != nullptr || Oracle.Active();
    const uint64_t DynCost = NeedMetrics ? ComputeDynamicCost(Ssa) : 0;
    Oracle.RecordSsaStage("02_opt_ssa", Ssa);
    LowerOutOfSsa(Ssa, Blocks);
    const uint32_t PeakLanes =
        NeedMetrics ? ComputeExactPeakLanes(Blocks, Program.TempCount) : 0;
    Oracle.RecordFlowStage("03_oossa", Blocks);
    if (!AllocateRegisters(Blocks, Program, Error)) {
      Diagnostics << "error: " << Error << '\n';
      return E_FAIL;
    }
    Oracle.RecordFlowStage("04_ra", Blocks);
    if (!LowerControlFlow(Blocks, Program, Error)) {
      Diagnostics << "error: " << Error << '\n';
      return E_FAIL;
    }
    PruneUnusedPixelInputs(Program);
    Oracle.RecordMirStage("05_cfg", Program);
    if (!WriteDxbc(Program, Bytecode, Error)) {
      Diagnostics << "error: " << Error << '\n';
      return E_FAIL;
    }
    if (NeedMetrics) {
      CompileStats LocalStats;
      CollectCompileStats(Program, LocalStats);
      LocalStats.DynamicCost = DynCost;
      LocalStats.PeakLanes = PeakLanes;
      if (Stats)
        *Stats = LocalStats;
      Oracle.Finish(LocalStats, Bytecode);
    }
    Output.write(reinterpret_cast<const char *>(Bytecode.data()), Bytecode.size());
    return S_OK;
  } catch (const hlsl::Exception &E) {
    Diagnostics << "error: " << (E.msg.empty() ? "DXIL exception" : E.msg) << '\n';
    return E_FAIL;
  } catch (const std::exception &E) {
    Diagnostics << "error: " << (E.what() ? E.what() : "C++ exception") << '\n';
    return E_FAIL;
  } catch (...) {
    Diagnostics << "error: unknown C++ exception in dxcb5::Compile\n";
    return E_FAIL;
  }
}
