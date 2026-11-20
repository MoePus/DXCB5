// Distributed under the University of Illinois Open Source License.
#include "dxclib/dxc.h"
#include <vector>

int __cdecl wmain(int Argc, const wchar_t **Argv) {
  std::vector<const wchar_t *> Arguments;
  Arguments.reserve(Argc + 1);
  Arguments.push_back(Argv[0]);
  Arguments.push_back(L"-fdxcb5");
  Arguments.insert(Arguments.end(), Argv + 1, Argv + Argc);
  return dxc::main(static_cast<int>(Arguments.size()), Arguments.data());
}
