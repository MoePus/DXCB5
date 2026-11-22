// Distributed under the University of Illinois Open Source License.
#include "dxcb5/dxcb5_spec.h"
#include "dxclib/dxc.h"
#include <windows.h>
#include <cwchar>
#include <string>
#include <vector>

static std::string WideToUtf8(const wchar_t *W) {
  if (!W || !*W)
    return {};
  const int N = WideCharToMultiByte(CP_UTF8, 0, W, -1, nullptr, 0, nullptr, nullptr);
  if (N <= 1)
    return {};
  std::string Out(static_cast<size_t>(N - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, W, -1, &Out[0], N - 1, nullptr, nullptr);
  return Out;
}

int __cdecl wmain(int Argc, const wchar_t **Argv) {
  std::vector<const wchar_t *> Arguments;
  Arguments.reserve(Argc + 1);
  Arguments.push_back(Argv[0]);
  Arguments.push_back(L"-fdxcb5");
  std::string DisabledPasses;
  std::string DumpStagesDir;
  std::string DumpMetricsPath;
  bool ListPasses = false;
  for (int I = 1; I < Argc; ++I) {
    if (std::wcscmp(Argv[I], L"--disable-pass") == 0 && I + 1 < Argc) {
      if (!DisabledPasses.empty())
        DisabledPasses.push_back(',');
      DisabledPasses += WideToUtf8(Argv[++I]);
      continue;
    }
    if ((std::wcscmp(Argv[I], L"--dump-mir-stages") == 0 ||
         std::wcscmp(Argv[I], L"--dxcb5-dump-mir-stages") == 0) &&
        I + 1 < Argc) {
      DumpStagesDir = WideToUtf8(Argv[++I]);
      continue;
    }
    if ((std::wcscmp(Argv[I], L"--dump-metrics") == 0 ||
         std::wcscmp(Argv[I], L"--dxcb5-dump-metrics") == 0) &&
        I + 1 < Argc) {
      DumpMetricsPath = WideToUtf8(Argv[++I]);
      continue;
    }
    if (std::wcscmp(Argv[I], L"--list-passes") == 0) {
      ListPasses = true;
      continue;
    }
    Arguments.push_back(Argv[I]);
  }
  if (!DisabledPasses.empty() &&
      dxcb5_set_disabled_passes(DisabledPasses.c_str()) != 0)
    return 1;
  if (!DumpStagesDir.empty() || !DumpMetricsPath.empty() || ListPasses)
    dxcb5_configure_oracle(
        DumpStagesDir.empty() ? nullptr : DumpStagesDir.c_str(),
        DumpMetricsPath.empty() ? nullptr : DumpMetricsPath.c_str(),
        ListPasses ? 1 : 0);
  return dxc::main(static_cast<int>(Arguments.size()), Arguments.data());
}
