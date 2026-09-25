//
// The follow-up to CycleBreakingDemo.cpp: that demo proved BreakCycles lets
// oilgen generate working *introspection* code for a genuinely cyclic
// raw-pointer structure, but deliberately stopped there (see its own header
// comment) - true cyclic *reconstruction* needed a further capture-side
// rework (recursively capturing each cycle-capable node's real content into
// a nested, length-prefixed blob, instead of just its address) plus a
// register-before-recursing construction discipline on the read side (see
// CodeGen::emitReconstructPointerValue/getOrEmitCycleReconstructHelper and
// docs/object-capture-initial-thoughts.md, not part of this repo). This demo
// proves that follow-up work end-to-end, on the same RawNode shape
// CycleBreakingDemo.cpp and test/integration/cycles.toml's `cycles_raw_ptr`
// case use.
//
// oilgen allows at most one oi::introspect<T>()/oi::reconstruct<T>() call
// site per translation unit (see OIGenerator.cpp's "found more than one
// introspect (or reconstruct) site" check), so this single file can only
// have one root type - RawNode itself, reused across two different runtime
// graphs to prove the two outcomes reconstructing a cyclic type can have:
//
//   - buildNonRootCycle(): a 3-node cycle that closes back onto an
//     *ancestor* (`second`), but never onto the literal object being
//     reconstructed (`first`, the root). This is fully supported: real
//     values round-trip for every node, and the closing pointer is
//     correctly aliased back to the already-reconstructed `second` rather
//     than being duplicated.
//   - buildRootClosingCycle(): a 3-node cycle that closes directly back onto
//     the root itself. oi::reconstruct<T>() returns T by value, so a
//     self-pointer into the value being returned can't be represented today
//     (see the thrown message) - this is deliberately rejected with a clear
//     error instead of silently producing a dangling pointer.
//
#include <oi/oi.h>

#if !defined(OIL_AOT_COMPILATION)
#error "This file must be compiled with -DOIL_AOT_COMPILATION=1"
#endif

#include <cstdlib>
#include <iostream>

// The exact shape CycleBreakingDemo.cpp and test/integration/cycles.toml's
// `cycles_raw_ptr` case use (this target enables capture-bytes and
// chase-raw-pointers - see PrependCaptureBytesFeature.cmake - without
// chase-raw-pointers `next` would silently degrade to an inert
// StubbedPointer and never exercise BreakCycles at all).
struct RawNode {
  int value;
  RawNode* next;
};

RawNode* buildNonRootCycle() {
  RawNode* first = new RawNode{1, nullptr};
  RawNode* second = new RawNode{2, nullptr};
  RawNode* third = new RawNode{3, nullptr};
  first->next = second;
  second->next = third;
  third->next = second;  // closes onto `second`, an ancestor but not `first`
  return first;
}

RawNode* buildRootClosingCycle() {
  RawNode* first = new RawNode{1, nullptr};
  RawNode* second = new RawNode{2, nullptr};
  RawNode* third = new RawNode{3, nullptr};
  first->next = second;
  second->next = third;
  third->next = first;  // closes directly onto the root itself
  return first;
}

int main() {
  bool ok = true;

  std::cout << "--- non-root cycle (first -> second -> third -> second) ---\n";
  try {
    RawNode* first = buildNonRootCycle();

    const auto result = oi::introspect(*first);
    const auto bytes = result.rawBytes();
    std::cout << "captured " << bytes.size() << " bytes\n";

    const RawNode reconstructed = oi::reconstruct<RawNode>(bytes);
    std::cout << "reconstructed.value = " << reconstructed.value << '\n';
    std::cout << "reconstructed.next->value = " << reconstructed.next->value
              << '\n';
    std::cout << "reconstructed.next->next->value = "
              << reconstructed.next->next->value << '\n';
    const bool aliasedCorrectly =
        reconstructed.next->next->next == reconstructed.next;
    std::cout << "reconstructed.next->next->next == reconstructed.next: "
              << std::boolalpha << aliasedCorrectly << std::noboolalpha << '\n';

    if (reconstructed.value != 1 || reconstructed.next->value != 2 ||
        reconstructed.next->next->value != 3 || !aliasedCorrectly) {
      std::cerr
          << "error: reconstructed values/aliasing don't match expectations\n";
      ok = false;
    }
  } catch (const std::exception& ex) {
    std::cerr << "error: unexpected exception: " << ex.what() << '\n';
    ok = false;
  }

  std::cout
      << "\n--- root-closing cycle (first -> second -> third -> first) ---\n";
  try {
    RawNode* first = buildRootClosingCycle();

    const auto result = oi::introspect(*first);
    const auto bytes = result.rawBytes();
    std::cout << "captured " << bytes.size() << " bytes\n";

    const RawNode reconstructed = oi::reconstruct<RawNode>(bytes);
    std::cerr << "error: expected reconstruct() to throw, but it returned a "
                 "value (reconstructed.value = "
              << reconstructed.value << ")\n";
    ok = false;
  } catch (const std::exception& ex) {
    std::cout << "got expected exception: " << ex.what() << '\n';
  }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
