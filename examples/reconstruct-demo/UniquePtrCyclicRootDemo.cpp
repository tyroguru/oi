//
// The std::unique_ptr root-closing counterpart to
// ContainerCyclicReconstructDemo.cpp's non-root success case - proves the
// deliberate rejection still works correctly once the reconstruction root
// itself is the self-referential type.
//
// This needs its own file/root type: unlike ContainerCyclicReconstructDemo's
// Entry wrapper (which, by construction, can never be part of a
// std::unique_ptr/std::shared_ptr cycle reached only through its own
// non-owning raw pointer members), the root-closing case requires the
// literal oi::reconstruct<T>() root to be the self-referential class
// itself - a different root type than Entry, and oilgen allows only one
// introspect/reconstruct call site per translation unit.
//
// Per std::unique_ptr's exclusive-ownership topology (only one unique_ptr
// can ever own a given address), a bare self-referential std::unique_ptr
// root can *only* ever produce this shape, never the non-root-success one -
// see the digression on this in the conversation this demo was built from,
// and docs/object-capture-initial-thoughts.md's cyclic-reconstruction notes.
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

int main() {
  bool ok = true;

  auto node1 = std::make_unique<UniqueNode>();
  UniqueNode* node1Raw = node1.get();
  node1->value = 1;
  node1->next = std::make_unique<UniqueNode>();
  node1->next->value = 2;
  node1->next->next = std::make_unique<UniqueNode>();
  node1->next->next->value = 3;
  node1->next->next->next = std::move(node1);  // closes directly onto the root

  try {
    const auto result = oi::introspect(*node1Raw);
    const auto bytes = result.rawBytes();
    std::cout << "captured " << bytes.size() << " bytes\n";

    const UniqueNode reconstructed = oi::reconstruct<UniqueNode>(bytes);
    std::cerr << "error: expected reconstruct() to throw, but it returned a "
                 "value (reconstructed.value = "
              << reconstructed.value << ")\n";
    ok = false;
  } catch (const std::exception& ex) {
    std::cout << "got expected exception: " << ex.what() << '\n';
  }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
