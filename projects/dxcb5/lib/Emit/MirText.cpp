// Distributed under the University of Illinois Open Source License.
#include "dxcb5/MirText.h"
#include "llvm/ADT/StringExtras.h"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

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

class Reader {
public:
  explicit Reader(llvm::StringRef Text) : Stream(Text.str()) {}

  bool Tag(const char *Expected) {
    std::string Word;
    return Stream >> Word && Word == Expected;
  }

  bool Fields() { return true; }

  template <class Head, class... Tail> bool Fields(Head &Value, Tail &...Rest) {
    return Field(Value) && Fields(Rest...);
  }

  std::istringstream Stream;

private:
  template <class UInt> bool Field(UInt &Value) {
    std::string Text;
    if (!(Stream >> Text))
      return false;
    uint64_t Number = 0;
    if (!ParseDigits(Text, std::numeric_limits<UInt>::max(), Number))
      return false;
    Value = static_cast<UInt>(Number);
    return true;
  }

  bool Field(int8_t &Value) {
    std::string Text;
    if (!(Stream >> Text))
      return false;
    llvm::StringRef Digits(Text);
    const bool Negative = Digits.front() == '-';
    if (Negative)
      Digits = Digits.drop_front();
    constexpr uint64_t Max = std::numeric_limits<int8_t>::max();
    uint64_t Number = 0;
    if (!ParseDigits(Digits, Negative ? Max + 1 : Max, Number))
      return false;
    const int Signed = static_cast<int>(Number);
    Value = static_cast<int8_t>(Negative ? -Signed : Signed);
    return true;
  }

  static bool ParseDigits(llvm::StringRef Digits, uint64_t Limit,
                          uint64_t &Number) {
    if (Digits.empty())
      return false;
    for (char Ch : Digits) {
      if (Ch < '0' || Ch > '9')
        return false;
      const unsigned Digit = Ch - '0';
      if (Number > Limit / 10 || (Number == Limit / 10 && Digit > Limit % 10))
        return false;
      Number = Number * 10 + Digit;
    }
    return true;
  }

  bool Field(std::string &Name) {
    return (Stream >> std::ws).peek() == '"' &&
           static_cast<bool>(Stream >> std::quoted(Name));
  }

  bool Field(std::vector<uint8_t> &Bytes) {
    std::string Hex;
    if (!Field(Hex) || Hex.size() % 2)
      return false;
    for (size_t I = 0; I < Hex.size(); I += 2) {
      const auto High = llvm::hexDigitValue(Hex[I]);
      const auto Low = llvm::hexDigitValue(Hex[I + 1]);
      if (High > 15 || Low > 15)
        return false;
      Bytes.push_back(static_cast<uint8_t>(High * 16 + Low));
    }
    return true;
  }

  bool Field(OperandKind &Kind) {
    uint8_t Number;
    if (!Field(Number) ||
        Number > static_cast<uint8_t>(OperandKind::OutputMask))
      return false;
    Kind = static_cast<OperandKind>(Number);
    return true;
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

bool ParseSignature(Reader &In, std::vector<SignatureElement> &Out) {
  SignatureElement Sig;
  if (!In.Fields(Sig.Semantic, Sig.SemanticIndex, Sig.Register, Sig.SystemValue,
                 Sig.ComponentType, Sig.Interpolation, Sig.Mask, Sig.UsedMask))
    return false;
  Out.push_back(std::move(Sig));
  return true;
}

bool ParseVariable(Reader &In, std::vector<ConstantVariable> &Out) {
  ConstantVariable Var;
  size_t Members;
  if (!In.Tag("variable") ||
      !In.Fields(Var.Name, Var.Offset, Var.Size, Var.Class, Var.Type, Var.Rows,
                 Var.Columns, Var.Elements, Var.Used, Var.DefaultValue,
                 Var.TypeName, Members) ||
      (!Var.DefaultValue.empty() && Var.DefaultValue.size() != Var.Size))
    return false;
  for (size_t I = 0; I < Members; ++I)
    if (!ParseVariable(In, Var.Members))
      return false;
  Out.push_back(std::move(Var));
  return true;
}

bool ParseConstantBuffer(Reader &In, std::vector<ConstantBuffer> &Out) {
  ConstantBuffer Buffer;
  size_t Variables;
  if (!In.Fields(Buffer.Name, Buffer.Slot, Buffer.Size, Buffer.DynamicIndex,
                 Variables))
    return false;
  for (size_t I = 0; I < Variables; ++I)
    if (!ParseVariable(In, Buffer.Variables))
      return false;
  Out.push_back(std::move(Buffer));
  return true;
}

bool ParseResource(Reader &In, std::vector<BoundResource> &Out) {
  BoundResource Resource;
  if (!In.Fields(Resource.Name, Resource.Slot, Resource.Type,
                 Resource.Dimension, Resource.ReturnType, Resource.Count,
                 Resource.Comparison, Resource.Components,
                 Resource.SampleCount))
    return false;
  Out.push_back(std::move(Resource));
  return true;
}

bool ParseInstruction(Reader &In, std::vector<Instruction> &Out) {
  Instruction Inst;
  size_t Operands;
  if (!In.Fields(Inst.Opcode, Inst.TestNonZero, Inst.Saturate,
                 Inst.TexelOffset[0], Inst.TexelOffset[1], Inst.TexelOffset[2],
                 Operands))
    return false;
  for (size_t I = 0; I < Operands; ++I) {
    Operand Op{OperandKind::Null};
    if (!In.Tag("operand") ||
        !In.Fields(Op.Kind, Op.Index, Op.Element, Op.Mask, Op.Swizzle,
                   Op.Modifier, Op.RelativeRegister, Op.RelativeLane))
      return false;
    Inst.Operands.push_back(Op);
  }
  Out.push_back(std::move(Inst));
  return true;
}

bool ParseRecord(Reader &In, const std::string &Tag, MirProgram &Program) {
  if (Tag == "input")
    return ParseSignature(In, Program.Inputs);
  if (Tag == "output")
    return ParseSignature(In, Program.Outputs);
  if (Tag == "cbuffer")
    return ParseConstantBuffer(In, Program.ConstantBuffers);
  if (Tag == "resource")
    return ParseResource(In, Program.Resources);
  if (Tag == "literal-vector") {
    std::array<uint32_t, 4> Value;
    if (!In.Fields(Value[0], Value[1], Value[2], Value[3]))
      return false;
    Program.LiteralVectors.push_back(Value);
    return true;
  }
  if (Tag == "indexable-temp") {
    IndexableTempDecl Decl;
    if (!In.Fields(Decl.Index, Decl.Elements, Decl.Components))
      return false;
    Program.IndexableTemps.push_back(Decl);
    return true;
  }
  if (Tag == "immediate-constant") {
    uint32_t X, Y, Z, W;
    if (!In.Fields(X, Y, Z, W))
      return false;
    Program.ImmediateConstants.insert(Program.ImmediateConstants.end(),
                                      {X, Y, Z, W});
    return true;
  }
  if (Tag == "instruction")
    return ParseInstruction(In, Program.Instructions);
  return false;
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
               Resource.Comparison, Resource.Components, Resource.SampleCount);
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

bool ParseMir(llvm::StringRef Text, MirProgram &Program, std::string &Error) {
  Reader In(Text);
  In.Stream.imbue(std::locale::classic());
  MirProgram Result;
  uint32_t Version;
  bool Pixel;
  if (!In.Tag("dxcb5-mir") || !In.Fields(Version, Pixel, Result.TempCount) ||
      Version != 1) {
    Error = "invalid DXCB5 MIR header";
    return false;
  }
  if (Pixel)
    Result.ShaderStage = Stage::Pixel;

  std::string Tag;
  while (In.Stream >> Tag) {
    if (Tag == "end") {
      if ((In.Stream >> std::ws).peek() != std::char_traits<char>::eof()) {
        Error = "unexpected data after DXCB5 MIR end record";
        return false;
      }
      Program = std::move(Result);
      return true;
    }
    if (!ParseRecord(In, Tag, Result)) {
      Error = "invalid DXCB5 MIR record: " + Tag;
      return false;
    }
  }
  Error = "missing DXCB5 MIR end record";
  return false;
}

} // namespace dxcb5
