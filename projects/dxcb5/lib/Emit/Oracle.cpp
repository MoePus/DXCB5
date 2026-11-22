// Distributed under the University of Illinois Open Source License.
#include "Emit/Oracle.h"
#include "Mir/OptPipeline.h"
#include "Mir/SsaAnalysis.h"
#include "dxcb5/Formats.h"
#include "dxcb5/InstructionInfo.h"
#include "dxcb5/MirText.h"
#include "dxcb5/SpecEngine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace dxcb5 {
namespace {

struct OracleConfig {
  std::string DumpStagesDir;
  std::string DumpMetricsPath;
  bool ListPasses = false;
};

OracleConfig &ConfigStorage() {
  static OracleConfig Cfg;
  return Cfg;
}

thread_local OracleSession *gCurrentSession = nullptr;

void FormatOperand(std::ostringstream &OS, const Operand &Op) {
  OS << static_cast<unsigned>(Op.Kind) << ':' << Op.Index << '.' << Op.Element
     << '/' << static_cast<unsigned>(Op.Mask) << '@'
     << static_cast<unsigned>(Op.Swizzle);
  if (Op.Modifier)
    OS << 'm' << static_cast<unsigned>(Op.Modifier);
  if (Op.RelativeRegister != UINT32_MAX)
    OS << '[' << Op.RelativeRegister << '.'
       << static_cast<unsigned>(Op.RelativeLane) << ']';
}

std::string FormatSsaProgram(const SsaProgram &Ssa) {
  std::ostringstream OS;
  OS << "ssa blocks=" << Ssa.Blocks.size()
     << " x=" << Ssa.Mir.IndexableTemps.size() << '\n';
  for (const auto &IT : Ssa.Mir.IndexableTemps)
    OS << "  x" << IT.Index << '[' << IT.Elements << "]." << IT.Components
       << '\n';
  for (size_t B = 0; B < Ssa.Blocks.size(); ++B) {
    const auto &Blk = Ssa.Blocks[B];
    OS << "bb" << B << " kind=" << static_cast<unsigned>(Blk.Kind);
    if (Blk.StrictBranch)
      OS << " strict";
    if (Blk.UniformBranch)
      OS << " uniform";
    OS << " succs=[";
    for (size_t K = 0; K < Blk.Successors.size(); ++K) {
      if (K) OS << ',';
      OS << Blk.Successors[K];
    }
    OS << ']';
    if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch) {
      OS << " cond=";
      FormatOperand(OS, Blk.Condition);
    }
    OS << '\n';
    for (const auto &Phi : Blk.Phis) {
      if (Phi.Dead)
        continue;
      OS << "  phi v" << Phi.Result << '/' << static_cast<unsigned>(Phi.Mask);
      for (const auto &In : Phi.Inputs) {
        OS << " [" << In.PredBlock << ':';
        FormatOperand(OS, In.Value);
        OS << ']';
      }
      OS << '\n';
    }
    for (const auto &I : Blk.Insts) {
      if (I.Dead)
        continue;
      OS << "  op=" << I.Opcode;
      if (I.Saturate) OS << ".sat";
      if (I.Precise) OS << ".prec";
      if (I.FastMath) OS << ".fm";
      OS << ' ';
      FormatOperand(OS, I.Dest);
      if (I.AuxDest.Kind != OperandKind::Null) {
        OS << ',';
        FormatOperand(OS, I.AuxDest);
      }
      for (const auto &S : I.Sources) {
        OS << ' ';
        FormatOperand(OS, S);
      }
      OS << '\n';
    }
  }
  return OS.str();
}

std::string FormatFlowBlocks(llvm::ArrayRef<FlowBlock> Blocks) {
  std::ostringstream OS;
  OS << "flow blocks=" << Blocks.size() << '\n';
  for (size_t B = 0; B < Blocks.size(); ++B) {
    const auto &Blk = Blocks[B];
    OS << "bb" << B << " kind=" << static_cast<unsigned>(Blk.Kind);
    if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch) {
      OS << " cond=";
      FormatOperand(OS, Blk.Condition);
    }
    OS << '\n';
    for (const auto &I : Blk.Instructions) {
      OS << "  op=" << I.Opcode;
      if (I.Saturate) OS << ".sat";
      for (const auto &Op : I.Operands) {
        OS << ' ';
        FormatOperand(OS, Op);
      }
      OS << '\n';
    }
    for (const auto &E : Blk.Edges) {
      OS << "  edge->" << E.Target << " copies=" << E.Copies.size() << '\n';
      for (const auto &C : E.Copies) {
        OS << "    cp r" << C.Destination << '.'
           << static_cast<unsigned>(C.DestinationLane) << " <- ";
        FormatOperand(OS, C.Source);
        OS << '\n';
      }
    }
  }
  return OS.str();
}

void WriteTextFileOrStderr(const std::string &Path, const std::string &Text) {
  if (Path == "-") {
    std::fwrite(Text.data(), 1, Text.size(), stderr);
    return;
  }
  std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
  if (Out)
    Out.write(Text.data(), static_cast<std::streamsize>(Text.size()));
}

std::string EscapeJson(const std::string &S) {
  std::string Out;
  Out.reserve(S.size() + 2);
  Out.push_back('"');
  for (char C : S) {
    if (C == '"' || C == '\\') Out.push_back('\\');
    Out.push_back(C);
  }
  Out.push_back('"');
  return Out;
}

} // namespace

void ConfigureOracle(const char *DumpStagesDir, const char *DumpMetricsPath,
                     bool ListPasses) {
  auto &Cfg = ConfigStorage();
  Cfg.DumpStagesDir = DumpStagesDir ? DumpStagesDir : "";
  Cfg.DumpMetricsPath = DumpMetricsPath ? DumpMetricsPath : "";
  Cfg.ListPasses = ListPasses;
}

uint64_t ComputeDynamicCost(const SsaProgram &Ssa) {
  if (Ssa.Blocks.empty())
    return 0;
  SsaDomTree Dom;
  Ssa.ComputeDominators(Dom);
  SsaLoopInfo Loops;
  ComputeLoopInfo(Ssa, Dom, Loops);
  uint64_t Total = 0;
  for (uint32_t B : Dom.RpoOrder) {
    const unsigned Depth =
        B < Loops.LoopDepth.size() ? std::min(Loops.LoopDepth[B], 6u) : 0u;
    const uint64_t Weight = uint64_t{1} << (Depth * 3u);
    uint64_t LiveInsts = 0;
    for (const auto &I : Ssa.Blocks[B].Insts)
      if (!I.Dead)
        ++LiveInsts;
    Total += LiveInsts * Weight;
  }
  return Total;
}

OracleSession::OracleSession() : Prev(gCurrentSession) {
  const auto &Cfg = ConfigStorage();
  DumpStages = !Cfg.DumpStagesDir.empty();
  DumpMetrics = !Cfg.DumpMetricsPath.empty();
  ListPasses = Cfg.ListPasses;
  StagesTarget = Cfg.DumpStagesDir;
  MetricsTarget = Cfg.DumpMetricsPath;
  IsActive = DumpStages || DumpMetrics || ListPasses;
  if (IsActive)
    gCurrentSession = this;
}

OracleSession::~OracleSession() {
  if (IsActive)
    gCurrentSession = Prev;
}

OracleSession *OracleSession::Current() { return gCurrentSession; }

void OracleSession::EmitStageText(const char *Name, const std::string &Text) {
  const uint32_t Seq = NextSeq++;
  const std::string Hash = spec::ComputeSha256Hex(Text.data(), Text.size());
  Stages.push_back({Seq, Name, Hash});
  if (!DumpStages)
    return;
  if (StagesTarget == "-") {
    std::fprintf(stderr, "[dxcb5-oracle] stage %03u %-24s sha256=%s\n", Seq,
                 Name, Hash.c_str());
    return;
  }
  llvm::sys::fs::create_directories(StagesTarget);
  char Prefix[32];
  std::snprintf(Prefix, sizeof(Prefix), "%03u_", Seq);
  std::string Path = StagesTarget;
  if (!Path.empty() && Path.back() != '/' && Path.back() != '\\')
    Path.push_back('/');
  Path += Prefix;
  Path += Name;
  Path += ".txt";
  WriteTextFileOrStderr(Path, Text);
}

void OracleSession::RecordSsaStage(const char *Name, const SsaProgram &Ssa) {
  if (!DumpStages)
    return;
  EmitStageText(Name, FormatSsaProgram(Ssa));
}

void OracleSession::RecordPass(const char *Name, bool Changed,
                               const SsaProgram &Ssa) {
  if (!IsActive)
    return;
  auto It = std::find_if(Passes.begin(), Passes.end(),
                         [Name](const OraclePassStat &P) {
                           return P.Name == Name;
                         });
  if (It == Passes.end())
    Passes.push_back({Name, 1u, Changed ? 1u : 0u});
  else {
    ++It->Calls;
    if (Changed)
      ++It->Changes;
  }
  if (DumpStages && Changed)
    EmitStageText(Name, FormatSsaProgram(Ssa));
}

void OracleSession::RecordFlowStage(const char *Name,
                                    llvm::ArrayRef<FlowBlock> Blocks) {
  if (!DumpStages)
    return;
  EmitStageText(Name, FormatFlowBlocks(Blocks));
}

void OracleSession::RecordMirStage(const char *Name,
                                   const MirProgram &Program) {
  if (!DumpStages)
    return;
  EmitStageText(Name, PrintMir(Program));
}

void OracleSession::Finish(const CompileStats &Stats,
                           llvm::ArrayRef<uint8_t> Dxbc) {
  if (!IsActive)
    return;
  const std::string DxbcHash = spec::ComputeSha256Hex(Dxbc.data(), Dxbc.size());
  if (DumpStages) {
    Stages.push_back({NextSeq++, "06_dxbc", DxbcHash});
    if (StagesTarget == "-") {
      std::fprintf(stderr, "[dxcb5-oracle] stage %03u %-24s sha256=%s\n",
                   Stages.back().Seq, "06_dxbc", DxbcHash.c_str());
    } else {
      std::ostringstream Manifest;
      for (const auto &St : Stages)
        Manifest << St.Seq << '\t' << St.Name << '\t' << St.Sha256 << '\n';
      std::string Path = StagesTarget;
      if (!Path.empty() && Path.back() != '/' && Path.back() != '\\')
        Path.push_back('/');
      Path += "stages.sha256.tsv";
      WriteTextFileOrStderr(Path, Manifest.str());
    }
  }
  if (ListPasses) {
    uint32_t TotalCalls = 0, TotalChanges = 0;
    std::fprintf(stderr, "=== DXCB5 Executed Passes ===\n");
    for (const auto &P : Passes) {
      std::fprintf(stderr, "  %-20s calls=%-3u changes=%u\n", P.Name.c_str(),
                   P.Calls, P.Changes);
      TotalCalls += P.Calls;
      TotalChanges += P.Changes;
    }
    std::fprintf(stderr, "  TOTAL                calls=%-3u changes=%u\n",
                 TotalCalls, TotalChanges);
  }
  if (DumpMetrics) {
    std::ostringstream OS;
    OS << "{\"schema_version\":" << DXCB5_ORACLE_SCHEMA_VERSION
       << ",\"disabled_passes\":" << EscapeJson(GetDisabledOptimizationPasses())
       << ",\"instructions\":" << Stats.Instructions
       << ",\"temps\":" << Stats.Temps
       << ",\"indexable_temps\":" << Stats.IndexableTemps
       << ",\"sfu_ops\":" << Stats.SfuOps
       << ",\"tex_ops\":" << Stats.TexOps
       << ",\"branch_ops\":" << Stats.BranchOps
       << ",\"globals_bytes\":" << Stats.GlobalsBytes
       << ",\"dynamic_cost\":" << Stats.DynamicCost
       << ",\"peak_lanes\":" << Stats.PeakLanes
       << ",\"dxbc_bytes\":" << Dxbc.size()
       << ",\"dxbc_sha256\":" << EscapeJson(DxbcHash)
       << ",\"passes\":[";
    for (size_t I = 0; I < Passes.size(); ++I) {
      if (I) OS << ',';
      OS << "{\"name\":" << EscapeJson(Passes[I].Name)
         << ",\"calls\":" << Passes[I].Calls
         << ",\"changes\":" << Passes[I].Changes << '}';
    }
    OS << "],\"stages\":[";
    for (size_t I = 0; I < Stages.size(); ++I) {
      if (I) OS << ',';
      OS << "{\"seq\":" << Stages[I].Seq
         << ",\"name\":" << EscapeJson(Stages[I].Name)
         << ",\"sha256\":" << EscapeJson(Stages[I].Sha256) << '}';
    }
    OS << "]}\n";
    WriteTextFileOrStderr(MetricsTarget, OS.str());
  }
}

} // namespace dxcb5
