//
// The std::shared_ptr root-closing counterpart to
// ContainerCyclicReconstructDemo.cpp's non-root success case - see
// UniquePtrCyclicRootDemo.cpp's own header comment for why this needs its
// own file/root type.
//
// Unlike std::unique_ptr, std::shared_ptr genuinely allows multi-ownership,
// so a bare self-referential std::shared_ptr root *can* also produce the
// non-root-success shape (closing onto a middle ancestor instead of the
// root) - this file specifically builds the root-closing shape to prove
// that outcome still works correctly too, not because it's the only shape
// std::shared_ptr can produce.
//
#include <oi/oi.h>

#if !defined(OIL_AOT_COMPILATION)
#error "This file must be compiled with -DOIL_AOT_COMPILATION=1"
#endif

#include <cstdlib>
#include <iostream>
#include <memory>

struct SharedNode {
  int value;
  std::shared_ptr<SharedNode> next;
};

int main() {
  bool ok = true;

  auto node1 = std::make_shared<SharedNode>();
  SharedNode* node1Raw = node1.get();
  node1->value = 1;
  node1->next = std::make_shared<SharedNode>();
  node1->next->value = 2;
  node1->next->next = std::make_shared<SharedNode>();
  node1->next->next->value = 3;
  node1->next->next->next = node1;  // closes directly onto the root itself

  try {
    const auto result = oi::introspect(*node1Raw);
    const auto bytes = result.rawBytes();
    std::cout << "captured " << bytes.size() << " bytes\n";

    const SharedNode reconstructed = oi::reconstruct<SharedNode>(bytes);
    std::cerr << "error: expected reconstruct() to throw, but it returned a "
                 "value (reconstructed.value = "
              << reconstructed.value << ")\n";
    ok = false;
  } catch (const std::exception& ex) {
    std::cout << "got expected exception: " << ex.what() << '\n';
  }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
