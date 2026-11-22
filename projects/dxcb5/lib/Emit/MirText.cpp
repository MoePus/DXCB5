// Distributed under the University of Illinois Open Source License.
#include "dxcb5/MirText.h"
#include "llvm/ADT/StringExtras.h"

#include <iomanip>
#include <locale>
#include <sstream>

namespace dxcb5 {
namespace {

class Writer {
public:
  template <class... T> void Record(const char *Tag, const T &...Fields) {
    Stream << Tag;
    int Printed[] = {0, (Field(Fields), 0)...};
    (void)Printed;
    Stream << '\n';
  }

  std::ostringstream Stream;

private:
  template <class T> void Field(T Number) { Stream << ' ' << +Number; }
  void Field(const std::string &Name) { Stream << ' ' << std::quoted(Name); }
  void Field(OperandKind Kind) { Field(static_cast<unsigned>(Kind)); }
  void Field(const std::vector<uint8_t> &Bytes) {
    Stream << " \"";
    for (auto Byte : Bytes)
      Stream << llvm::hexdigit(Byte >> 4) << llvm::hexdigit(Byte & 15);
    Stream << '"';
  }
};

void PrintSignature(Writer &Out, const char *Tag,
                    const std::vector<SignatureElement> &Elements) {
  for (const SignatureElement &Sig : Elements)
    Out.Record(Tag, Sig.Semantic, Sig.SemanticIndex, Sig.Register,
               Sig.SystemValue, Sig.ComponentType, Sig.Interpolation,
               Sig.Mask, Sig.UsedMask);
}

void PrintVariable(Writer &Out, const ConstantVariable &Var) {
  Out.Record("variable", Var.Name, Var.Offset, Var.Size, Var.Class, Var.Type,
             Var.Rows, Var.Columns, Var.Elements, Var.Used, Var.DefaultValue,
             Var.TypeName, Var.Members.size());
  for (const auto &Member : Var.Members)
    PrintVariable(Out, Member);
}

void PrintConstantBuffers(Writer &Out,
                          const std::vector<ConstantBuffer> &Buffers) {
  for (const ConstantBuffer &Buffer : Buffers) {
    Out.Record("cbuffer", Buffer.Name, Buffer.Slot, Buffer.Size,
               Buffer.DynamicIndex, Buffer.Variables.size());
    for (const ConstantVariable &Var : Buffer.Variables)
      PrintVariable(Out, Var);
  }
}

} // namespace

std::string PrintMir(const MirProgram &Program) {
  Writer Out;
  Out.Stream.imbue(std::locale::classic());
  Out.Record("dxcb5-mir", 1, Program.ShaderStage == Stage::Pixel,
             Program.TempCount);
  PrintSignature(Out, "input", Program.Inputs);
  PrintSignature(Out, "output", Program.Outputs);
  PrintConstantBuffers(Out, Program.ConstantBuffers);
  for (const BoundResource &Resource : Program.Resources)
    Out.Record("resource", Resource.Name, Resource.Slot, Resource.Type,
               Resource.Dimension, Resource.ReturnType, Resource.Count,
               Resource.Comparison, Resource.Components, Resource.SampleCount,
               Resource.Stride, Resource.UavFlags);
  const auto &Constants = Program.ImmediateConstants;
  for (size_t I = 0; I < Constants.size(); I += 4)
    Out.Record("immediate-constant", Constants[I], Constants[I + 1],
               Constants[I + 2], Constants[I + 3]);
  for (const auto &Bits : Program.LiteralVectors)
    Out.Record("literal-vector", Bits[0], Bits[1], Bits[2], Bits[3]);
  for (const auto &Decl : Program.IndexableTemps)
    Out.Record("indexable-temp", Decl.Index, Decl.Elements, Decl.Components);
  for (const Instruction &Inst : Program.Instructions) {
    Out.Record("instruction", Inst.Opcode, Inst.TestNonZero, Inst.Saturate,
               Inst.TexelOffset[0], Inst.TexelOffset[1], Inst.TexelOffset[2],
               Inst.Operands.size());
    for (const Operand &Op : Inst.Operands)
      Out.Record("operand", Op.Kind, Op.Index, Op.Element, Op.Mask, Op.Swizzle,
                 Op.Modifier, Op.RelativeRegister, Op.RelativeLane);
  }
  Out.Record("end");
  return Out.Stream.str();
}

} // namespace dxcb5
