// Distributed under the University of Illinois Open Source License.
#include "dxcb5/MirText.h"

#include <iostream>

using namespace dxcb5;

namespace {

constexpr const char *Complete = R"(
dxcb5-mir 1 1 17
input "MY SEMANTIC" 2 3 4 1 5 7 3
output "SV_Target" 1 2 64 2 6 8 0
cbuffer "$buffer" 3 64 1 1
variable "name \"quoted\"" 12 4 5 0 1 1 0 1 "" "Tint" 1
variable "scale" 0 4 0 3 1 1 0 1 "0000803F" "" 0
resource "source\\texture" 4 3 8 7 2 1 3 0
immediate-constant 1065353216 0 3221225472 0
literal-vector 3 5 0 1
indexable-temp 0 8 1
instruction 54 0 1 -8 7 0 3
operand 9 0 0 1 0 0 7 3
operand 4 3 2 12 228 2 7 3
operand 3 4294967295 1 1 0 0 4294967295 0
instruction 54 0 0 0 0 0 2
operand 0 0 0 15 0 0 4294967295 0
operand 10 0 0 1 0 0 4294967295 0
instruction 62 1 0 0 0 0 0
end
)";

bool CheckRoundTrip() {
  MirProgram Program;
  std::string Error;
  if (!ParseMir(Complete, Program, Error)) {
    std::cerr << Error << '\n';
    return false;
  }
  if (Program.ShaderStage != Stage::Pixel || Program.TempCount != 17 ||
      Program.Inputs.size() != 1 || Program.Outputs.size() != 1 ||
      Program.ConstantBuffers.size() != 1 || Program.Resources.size() != 1 ||
      Program.ImmediateConstants.size() != 4 || Program.LiteralVectors.size() != 1 ||
      Program.IndexableTemps.size() != 1 || Program.Instructions.size() != 3)
    return false;
  constexpr auto HeaderOffset = 1;
  return PrintMir(Program) == Complete + HeaderOffset;
}

bool CheckBadRecords() {
  MirProgram Program;
  Program.TempCount = 7;
  std::string Error;
  const char *Bad[] = {
      "dxcb5-mir 2 0 0\nend\n",
      "dxcb5-mir 1 0 -1\nend\n",
      "dxcb5-mir 1 0 4294967296\nend\n",
      "dxcb5-mir 1 0 0\ninstruction 1 2 0 0 0 0 0\nend\n",
      "dxcb5-mir 1 0 0\ninstruction 1 0 0 0 0 0 1\noperand 16 0 0 0 0 0 0 0\nend\n",
      "dxcb5-mir 1 0 0\ninstruction 1 0 0 0 0 0 1\nend\n",
      "dxcb5-mir 1 0 0\ninstruction 1 0 0 -129 0 0 0\nend\n",
      "dxcb5-mir 1 0 0\nend\nunknown\n"};
  for (const char *Text : Bad) {
    if (ParseMir(Text, Program, Error) || Error.empty() || Program.TempCount != 7)
      return false;
  }
  return true;
}

} // namespace

int main() {
  if (!CheckRoundTrip() || !CheckBadRecords()) {
    std::cerr << "MIR text did not preserve the intermediate program\n";
    return 1;
  }
}
