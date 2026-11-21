// Distributed under the University of Illinois Open Source License.
#include "RegAlloc/Liveness.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <utility>

using namespace dxcb5;

namespace {

constexpr uint8_t kXLane = 1u;
constexpr unsigned kRegisterLanes = 4;
constexpr unsigned kRegsPerWord = 16;

bool ValidOperand(const Operand &Op, uint32_t TempCount) {
  return (Op.Kind != OperandKind::Temp || Op.Index < TempCount) &&
         (Op.RelativeRegister == UINT32_MAX ||
          (Op.RelativeRegister < TempCount && Op.RelativeLane < kRegisterLanes));
}

bool IsDotOpcode(uint32_t Opcode) {
  return Opcode == D3D10_SB_OPCODE_DP2 || Opcode == D3D10_SB_OPCODE_DP3 ||
         Opcode == D3D10_SB_OPCODE_DP4;
}

struct LocalBlockSummary {
  llvm::SmallVector<std::pair<uint32_t, uint8_t>, 16> Uses;
  llvm::SmallVector<std::pair<uint32_t, uint8_t>, 16> Defs;
};

struct BlockScanContext {
  std::vector<uint8_t> DefMask;
  std::vector<uint32_t> DefStamp;
  std::vector<uint32_t> UsePos;
  std::vector<uint32_t> UseStamp;
  uint32_t Gen = 0;

  explicit BlockScanContext(uint32_t TempCount)
      : DefMask(TempCount, 0), DefStamp(TempCount, 0), UsePos(TempCount, 0),
        UseStamp(TempCount, 0) {}

  void BeginBlock() { ++Gen; }

  void RecordRead(uint32_t Reg, uint8_t Mask, LocalBlockSummary &Summary,
                  std::vector<TempInfo> &Temps, std::vector<uint8_t> &IsGlobal) {
    Mask &= 0xfu;
    if (!Mask)
      return;
    Temps[Reg].ReadMask |= Mask;
    const uint8_t Defined = DefStamp[Reg] == Gen ? DefMask[Reg] : 0u;
    const uint8_t Exposed = static_cast<uint8_t>(Mask & ~Defined);
    if (!Exposed)
      return;
    IsGlobal[Reg] = 1;
    if (UseStamp[Reg] == Gen) {
      Summary.Uses[UsePos[Reg]].second |= Exposed;
    } else {
      UseStamp[Reg] = Gen;
      UsePos[Reg] = static_cast<uint32_t>(Summary.Uses.size());
      Summary.Uses.emplace_back(Reg, Exposed);
    }
  }

  void RecordDef(uint32_t Reg, uint8_t Mask, LocalBlockSummary &Summary) {
    Mask &= 0xfu;
    if (!Mask)
      return;
    if (DefStamp[Reg] == Gen) {
      DefMask[Reg] |= Mask;
    } else {
      DefStamp[Reg] = Gen;
      DefMask[Reg] = Mask;
      Summary.Defs.emplace_back(Reg, Mask);
    }
  }

  void FinalizeDefs(LocalBlockSummary &Summary) const {
    for (auto &KV : Summary.Defs)
      KV.second = DefMask[KV.first];
  }
};

bool ScanInstruction(const Instruction &Inst, BlockScanContext &Ctx,
                     LocalBlockSummary &Summary, std::vector<TempInfo> &Temps,
                     std::vector<uint8_t> &IsGlobal, uint32_t TempCount) {
  const size_t Dests = std::min<size_t>(
      InstructionDestinationCount(Inst.Opcode), Inst.Operands.size());
  const uint8_t DestMask = CombinedInstructionDestMask(Inst);

  for (size_t I = 0; I < Inst.Operands.size(); ++I) {
    const Operand &Op = Inst.Operands[I];
    if (!ValidOperand(Op, TempCount))
      return false;
    if (Op.RelativeRegister != UINT32_MAX) {
      Ctx.RecordRead(Op.RelativeRegister,
                     static_cast<uint8_t>(kXLane << Op.RelativeLane), Summary,
                     Temps, IsGlobal);
    }
    if (I >= Dests && Op.Kind == OperandKind::Temp) {
      const uint8_t Read = InstructionSourceReadLanes(
          Inst.Opcode, DestMask, I - Dests, Op.Swizzle);
      Ctx.RecordRead(Op.Index, Read, Summary, Temps, IsGlobal);
    }
  }

  unsigned TempDestCount = 0;
  for (size_t I = 0; I < Dests; ++I)
    TempDestCount += (Inst.Operands[I].Kind == OperandKind::Temp) ? 1u : 0u;

  const bool MustFixDest =
      (!InstructionIsLaneRemappable(Inst.Opcode) && !IsDotOpcode(Inst.Opcode)) ||
      TempDestCount > 1;
  for (size_t I = 0; I < Dests; ++I) {
    const Operand &Op = Inst.Operands[I];
    if (Op.Kind != OperandKind::Temp)
      continue;
    const uint8_t Mask = static_cast<uint8_t>(Op.Mask & 0xfu);
    Temps[Op.Index].WriteMask |= Mask;
    ++Temps[Op.Index].DefCount;
    if (MustFixDest)
      Temps[Op.Index].FixedLanes = true;
    Ctx.RecordDef(Op.Index, Mask, Summary);
  }
  return true;
}

bool ScanBlock(const FlowBlock &Block, size_t Id, size_t NumBlocks,
               uint32_t TempCount, BlockScanContext &Ctx,
               LocalBlockSummary &Summary, LivenessResult &Result,
               std::vector<uint8_t> &IsGlobal) {
  Ctx.BeginBlock();
  for (const Instruction &Inst : Block.Instructions) {
    if (!ScanInstruction(Inst, Ctx, Summary, Result.Temps, IsGlobal, TempCount))
      return false;
  }
  if (HasCondition(Block)) {
    if (!ValidOperand(Block.Condition, TempCount))
      return false;
    if (Block.Condition.RelativeRegister != UINT32_MAX) {
      Ctx.RecordRead(Block.Condition.RelativeRegister,
                     static_cast<uint8_t>(kXLane << Block.Condition.RelativeLane),
                     Summary, Result.Temps, IsGlobal);
    }
    if (Block.Condition.Kind == OperandKind::Temp) {
      const unsigned Lane = Block.Condition.Swizzle & 3u;
      Ctx.RecordRead(Block.Condition.Index, static_cast<uint8_t>(kXLane << Lane),
                     Summary, Result.Temps, IsGlobal);
    }
  }
  Ctx.FinalizeDefs(Summary);
  for (const FlowEdge &Edge : Block.Edges) {
    if (Edge.Target >= NumBlocks)
      return false;
    Result.Blocks[Edge.Target].Predecessors.push_back(Id);
    for (const ParallelCopy &Copy : Edge.Copies) {
      if (Copy.DestinationLane >= kRegisterLanes ||
          Copy.Destination >= TempCount ||
          (Copy.ReadsTemp() && Copy.Source.Index >= TempCount))
        return false;
      if (Copy.ReadsTemp()) {
        Result.Temps[Copy.Source.Index].ReadMask |=
            static_cast<uint8_t>(kXLane << Copy.SourceLane());
        IsGlobal[Copy.Source.Index] = 1;
      }
      Result.Temps[Copy.Destination].WriteMask |= Copy.DestinationMask();
      ++Result.Temps[Copy.Destination].DefCount;
      IsGlobal[Copy.Destination] = 1;
    }
  }
  return true;
}

void SetPackedMask(std::vector<uint64_t> &Words, uint32_t GlobalId,
                   uint8_t Mask) {
  const uint32_t W = GlobalId / kRegsPerWord;
  const unsigned Shift = (GlobalId % kRegsPerWord) * 4;
  Words[W] |= (uint64_t{Mask & 0xfu} << Shift);
}

void ClearPackedMask(std::vector<uint64_t> &Words, uint32_t GlobalId,
                     uint8_t Mask) {
  const uint32_t W = GlobalId / kRegsPerWord;
  const unsigned Shift = (GlobalId % kRegsPerWord) * 4;
  Words[W] &= ~(uint64_t{Mask & 0xfu} << Shift);
}

void PostOrderDfs(llvm::ArrayRef<FlowBlock> Blocks, size_t Root,
                  std::vector<uint8_t> &Visited, std::vector<size_t> &Order) {
  std::vector<std::pair<size_t, size_t>> Stack;
  Visited[Root] = 1;
  Stack.emplace_back(Root, 0);
  while (!Stack.empty()) {
    auto &Top = Stack.back();
    const auto &Edges = Blocks[Top.first].Edges;
    if (Top.second >= Edges.size()) {
      Order.push_back(Top.first);
      Stack.pop_back();
      continue;
    }
    const size_t Next = Edges[Top.second++].Target;
    if (!Visited[Next]) {
      Visited[Next] = 1;
      Stack.emplace_back(Next, 0);
    }
  }
}

void ComputePostOrder(llvm::ArrayRef<FlowBlock> Blocks,
                      std::vector<size_t> &Order) {
  const size_t N = Blocks.size();
  std::vector<uint8_t> Visited(N, 0);
  Order.reserve(N);
  for (size_t Root = 0; Root < N; ++Root)
    if (!Visited[Root])
      PostOrderDfs(Blocks, Root, Visited, Order);
}

void ComputeBlockLiveOut(const FlowBlock &Block,
                         const std::vector<BlockLiveness> &Facts,
                         const std::vector<uint32_t> &GlobalIdOf,
                         std::vector<uint64_t> &Out,
                         std::vector<uint64_t> &EdgeIn) {
  std::fill(Out.begin(), Out.end(), uint64_t{0});
  for (const FlowEdge &Edge : Block.Edges) {
    const auto &TargetIn = Facts[Edge.Target].In;
    if (Edge.Copies.empty()) {
      for (size_t W = 0; W < Out.size(); ++W)
        Out[W] |= TargetIn[W];
      continue;
    }
    EdgeIn = TargetIn;
    for (const ParallelCopy &Copy : Edge.Copies)
      ClearPackedMask(EdgeIn, GlobalIdOf[Copy.Destination],
                      Copy.DestinationMask());
    for (const ParallelCopy &Copy : Edge.Copies) {
      if (Copy.ReadsTemp()) {
        SetPackedMask(EdgeIn, GlobalIdOf[Copy.Source.Index],
                      static_cast<uint8_t>(kXLane << Copy.SourceLane()));
      }
    }
    for (size_t W = 0; W < Out.size(); ++W)
      Out[W] |= EdgeIn[W];
  }
}

bool UpdateBlockLiveIn(BlockLiveness &Block) {
  bool Changed = false;
  for (size_t W = 0; W < Block.Out.size(); ++W) {
    const uint64_t NewIn = Block.Uses[W] | (Block.Out[W] & ~Block.Defs[W]);
    Changed |= (NewIn != Block.In[W]);
    Block.In[W] = NewIn;
  }
  return Changed;
}

} // namespace

uint8_t dxcb5::CombinedInstructionDestMask(const Instruction &Inst) {
  const size_t Dests = std::min<size_t>(
      InstructionDestinationCount(Inst.Opcode), Inst.Operands.size());
  uint8_t Mask = 0;
  for (size_t I = 0; I < Dests; ++I) {
    if (Inst.Operands[I].Kind != OperandKind::Null)
      Mask |= static_cast<uint8_t>(Inst.Operands[I].Mask & 0xfu);
  }
  return Mask ? Mask : 0x1u;
}

bool dxcb5::ComputeLiveness(llvm::ArrayRef<FlowBlock> Blocks,
                            uint32_t TempCount, LivenessResult &Result) {
  llvm::TimeTraceScope Timer("DXCB5 Register Liveness", llvm::StringRef());
  Result.Blocks.assign(Blocks.size(), BlockLiveness{});
  Result.Temps.assign(TempCount, TempInfo{});
  Result.ActiveTemps.clear();
  Result.GlobalTemps.clear();

  std::vector<LocalBlockSummary> Summaries(Blocks.size());
  std::vector<uint8_t> IsGlobal(TempCount, 0);
  BlockScanContext Ctx(TempCount);
  for (size_t Id = 0; Id < Blocks.size(); ++Id) {
    if (!ScanBlock(Blocks[Id], Id, Blocks.size(), TempCount, Ctx, Summaries[Id],
                   Result, IsGlobal))
      return false;
  }

  std::vector<uint32_t> GlobalIdOf(TempCount, UINT32_MAX);
  for (uint32_t Reg = 0; Reg < TempCount; ++Reg) {
    TempInfo &Info = Result.Temps[Reg];
    Info.ActiveMask =
        static_cast<uint8_t>((Info.WriteMask | Info.ReadMask) & 0xfu);
    Info.Width = static_cast<uint8_t>(llvm::countPopulation(Info.ActiveMask));
    if (Info.Width > 0)
      Result.ActiveTemps.push_back(Reg);
    if (IsGlobal[Reg]) {
      GlobalIdOf[Reg] = static_cast<uint32_t>(Result.GlobalTemps.size());
      Result.GlobalTemps.push_back(Reg);
    }
  }

  const size_t Words = (Result.GlobalTemps.size() + kRegsPerWord - 1) / kRegsPerWord;
  if (Words == 0)
    return true;

  for (size_t Id = 0; Id < Blocks.size(); ++Id) {
    auto &Facts = Result.Blocks[Id];
    Facts.Uses.assign(Words, 0);
    Facts.Defs.assign(Words, 0);
    Facts.In.assign(Words, 0);
    Facts.Out.assign(Words, 0);
    for (const auto &KV : Summaries[Id].Uses)
      SetPackedMask(Facts.Uses, GlobalIdOf[KV.first], KV.second);
    for (const auto &KV : Summaries[Id].Defs) {
      const uint32_t GId = GlobalIdOf[KV.first];
      if (GId != UINT32_MAX)
        SetPackedMask(Facts.Defs, GId, KV.second);
    }
  }

  // Initialize worklist in reverse PostOrder so LIFO pop_back visits PostOrder.
  std::vector<size_t> Worklist;
  ComputePostOrder(Blocks, Worklist);
  std::reverse(Worklist.begin(), Worklist.end());
  std::vector<uint64_t> EdgeIn(Words, 0);
  while (!Worklist.empty()) {
    const size_t Id = Worklist.back();
    Worklist.pop_back();
    BlockLiveness &Facts = Result.Blocks[Id];
    Facts.Pending = false;
    ComputeBlockLiveOut(Blocks[Id], Result.Blocks, GlobalIdOf, Facts.Out, EdgeIn);
    if (!UpdateBlockLiveIn(Facts))
      continue;
    for (const size_t Pred : Facts.Predecessors) {
      if (!Result.Blocks[Pred].Pending) {
        Result.Blocks[Pred].Pending = true;
        Worklist.push_back(Pred);
      }
    }
  }
  return true;
}
