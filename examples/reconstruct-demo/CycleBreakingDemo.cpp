//
// Standalone AOT demo proving BreakCycles/CycleBreaker (the fix for
// facebookexperimental/object-introspection#293's stage 3, "Cycles are
// problematic in TreeBuilder V2") actually works: a genuinely
// self-referential, cyclic raw-pointer structure - the same shape
// test/integration/cycles.toml's `cycles_raw_ptr` case exercises, three
// real objects sharing one self-referential type, closing a genuine
// three-node cycle - can now be introspected without oilgen crashing or
// aborting, which is what happened before this fix existed.
//
// Deliberately introspect-only, and deliberately its own small program
// rather than a member added to ReconstructDemo.cpp's DemoObject (see
// docs/object-capture-initial-thoughts.md, not part of this repo, and
// CodeGen::resolveTypeName's explicit CycleBreaker case): reconstructing a
// genuinely cyclic object needs a fundamentally different,
// allocate-before-recursing construction discipline that hasn't been built
// yet, so a single translation unit calling both oi::introspect() and
// oi::reconstruct() for a cyclic type would fail oilgen's own code
// generation before producing *either* half - breaking this demo's build
// entirely, not just failing cleanly at runtime, since DemoObject's own
// introspect+reconstruct pair is generated together in one oilgen pass
// (see ReconstructDemo.cpp's own header comment). This file exists to
// prove the introspect half in isolation, cleanly, without that risk.
//
#include <oi/oi.h>

#if !defined(OIL_AOT_COMPILATION)
#error "This file must be compiled with -DOIL_AOT_COMPILATION=1"
#endif

#include <cstdlib>
#include <iostream>

// The exact shape test/integration/cycles.toml's `cycles_raw_ptr` case
// exercises: a self-referential struct via a genuinely chased raw pointer
// (this target enables the chase-raw-pointers feature - see
// PrependChaseRawPointersFeature.cmake - without which `next` would
// silently degrade to an inert StubbedPointer and never exercise
// BreakCycles at all).
struct RawNode {
  int value;
  RawNode* next;
};

RawNode* buildThreeNodeCycle() {
  RawNode* first = new RawNode{1, nullptr};
  RawNode* second = new RawNode{2, nullptr};
  RawNode* third = new RawNode{3, nullptr};
  first->next = second;
  second->next = third;
  third->next = first;
  return first;
}

int main() {
  try {
    RawNode* first = buildThreeNodeCycle();

    const auto result = oi::introspect(*first);
    const auto bytes = result.rawBytes();

    std::cout << "Captured " << bytes.size()
              << " bytes from a genuinely self-referential, three-node "
                 "cyclic RawNode graph without oilgen crashing or "
                 "aborting - BreakCycles (object-introspection#293 stage "
                 "3) is working.\n";

    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
