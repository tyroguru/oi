//
// The container counterpart to CyclicReconstructDemo.cpp: that demo proved
// true cyclic reconstruction for a raw pointer/reference edge
// (BreakCycles::visit(Pointer&)/visit(Reference&)); this one proves the
// same capability for std::unique_ptr and std::shared_ptr, once
// BreakCycles::visit(Container&) could wrap a Container's own template
// param (see BreakCycles.h) and
// CodeGen::emitReconstructContainerCyclicPointerValue gained the matching
// reconstruction-side dispatch.
//
// Both container kinds' non-root success case are proven here, together,
// in one program - oilgen allows at most one oi::introspect<T>()/
// oi::reconstruct<T>() call site per translation unit, so a single root
// type (Entry) reaches a self-referential std::unique_ptr<UniqueNode> cycle
// and a self-referential std::shared_ptr<SharedNode> cycle via two
// ordinary, non-owning raw pointer members - Entry itself is never part of
// either cycle, so this always exercises the success path, never the
// root-closing rejection (see UniquePtrCyclicRootDemo.cpp/
// SharedPtrCyclicRootDemo.cpp for that - it needs the literal
// reconstruction root to *be* the self-referential type, which Entry, by
// construction, never is).
//
#include <oi/oi.h>

#if !defined(OIL_AOT_COMPILATION)
#error "This file must be compiled with -DOIL_AOT_COMPILATION=1"
#endif

#include <cstdlib>
#include <iostream>
#include <memory>

struct UniqueNode {
  int value;
  std::unique_ptr<UniqueNode> next;
};

struct SharedNode {
  int value;
  std::shared_ptr<SharedNode> next;
};

// The one root type this file's one oilgen call site reconstructs - a
// non-owning observer of each cycle (standing in for a real
// std::reference_wrapper, which doesn't have reconstruct_kind wired up
// yet - see docs/object-capture-initial-thoughts.md).
struct Entry {
  UniqueNode* uniqueNode;
  SharedNode* sharedNode;
};

UniqueNode* buildUniqueNonRootCycle() {
  auto first = std::make_unique<UniqueNode>();
  first->value = 1;
  UniqueNode* firstPtr = first.get();
  first->next = std::make_unique<UniqueNode>();
  first->next->value = 2;
  UniqueNode* third =
      (first->next->next = std::make_unique<UniqueNode>()).get();
  third->value = 3;
  third->next =
      std::move(first);  // third now owns firstPtr's node; genuine cycle
  return firstPtr;
}

SharedNode* buildSharedNonRootCycle() {
  auto first = std::make_shared<SharedNode>();
  first->value = 1;
  SharedNode* firstPtr = first.get();
  first->next = std::make_shared<SharedNode>();
  first->next->value = 2;
  first->next->next = std::make_shared<SharedNode>();
  first->next->next->value = 3;
  first->next->next->next = first;  // real aliasing cycle (copy, not move)
  return firstPtr;
}

int main() {
  bool ok = true;

  Entry entry{buildUniqueNonRootCycle(), buildSharedNonRootCycle()};

  try {
    const auto result = oi::introspect(entry);
    const auto bytes = result.rawBytes();
    std::cout << "captured " << bytes.size() << " bytes\n";

    const Entry reconstructed = oi::reconstruct<Entry>(bytes);

    std::cout << "--- std::unique_ptr cycle ---\n";
    UniqueNode* u = reconstructed.uniqueNode;
    std::cout << "values: " << u->value << ", " << u->next->value << ", "
              << u->next->next->value << '\n';
    bool uniqueAliasCorrect = u->next->next->next.get() == u;
    std::cout << "closing edge aliases back to root: " << std::boolalpha
              << uniqueAliasCorrect << std::noboolalpha << '\n';
    if (u->value != 1 || u->next->value != 2 || u->next->next->value != 3 ||
        !uniqueAliasCorrect) {
      std::cerr << "error: std::unique_ptr cycle values/aliasing mismatch\n";
      ok = false;
    }

    std::cout << "--- std::shared_ptr cycle ---\n";
    SharedNode* s = reconstructed.sharedNode;
    std::cout << "values: " << s->value << ", " << s->next->value << ", "
              << s->next->next->value << '\n';
    bool sharedAliasCorrect = s->next->next->next.get() == s;
    std::cout << "closing edge aliases back to root: " << std::boolalpha
              << sharedAliasCorrect << std::noboolalpha << '\n';
    std::cout << "root use_count via closing edge: "
              << s->next->next->next.use_count() << " (expect 1)\n";
    if (s->value != 1 || s->next->value != 2 || s->next->next->value != 3 ||
        !sharedAliasCorrect || s->next->next->next.use_count() != 1) {
      std::cerr << "error: std::shared_ptr cycle values/aliasing/use_count "
                   "mismatch\n";
      ok = false;
    }
  } catch (const std::exception& ex) {
    std::cerr << "error: unexpected exception: " << ex.what() << '\n';
    ok = false;
  }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
