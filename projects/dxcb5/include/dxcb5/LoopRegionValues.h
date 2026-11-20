// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_LOOP_REGION_VALUES_H
#define DXCB5_LOOP_REGION_VALUES_H

namespace llvm {
class Function;
}
namespace hlsl {
class DxilModule;
}

namespace dxcb5 {
// Reuse equivalent dominating values after the caller finds a cyclic SCC.
bool ReuseLoopRegionValues(llvm::Function &Entry, const hlsl::DxilModule &Module);
}
#endif
