#include <gtest/gtest.h>

#include "oi/type_graph/BreakCycles.h"
#include "oi/type_graph/NodeTracker.h"
#include "oi/type_graph/TypeGraph.h"
#include "oi/type_graph/Types.h"
#include "test/TypeGraphParser.h"
#include "test/type_graph_utils.h"

using namespace type_graph;

TEST(BreakCyclesTest, PointerCycleBroken) {
  // A plain Class-member Pointer edge that closes a cycle back to its own
  // ancestor - exactly the shape this pass exists to fix (see BreakCycles.h).
  test(BreakCycles::createPass(),
       R"(
[0] Class: Node (size: 16)
      Member: value (offset: 0)
        Primitive: int32_t
      Member: next (offset: 8)
[1]     Pointer
          [0]
)",
       R"(
[0] Class: Node (size: 16)
      Member: value (offset: 0)
        Primitive: int32_t
      Member: next (offset: 8)
[1]     Pointer
          CycleBreaker
            [0]
)");
}

TEST(BreakCyclesTest, NonCyclicPointerUntouched) {
  // An ordinary (non-self-referential) pointer member - nothing for this
  // pass to do.
  testNoChange(BreakCycles::createPass(), R"(
[0] Class: Outer (size: 8)
      Member: ptr (offset: 0)
[1]     Pointer
[2]       Class: Inner (size: 4)
            Member: x (offset: 0)
              Primitive: int32_t
)");
}

TEST(BreakCyclesTest, ContainerMediatedCycleLeftAlone) {
  // A cycle mediated by a container's template parameter (standing in here
  // for e.g. a self-referential std::shared_ptr<Node> member) isn't a shape
  // this pass can safely rewrite (see BreakCycles::mutate's onPath_ branch) -
  // left exactly as-is for DetectCycles to catch afterwards with its own
  // diagnostic.
  testNoChange(BreakCycles::createPass(), R"(
[0] Class: Node (size: 24)
      Member: next (offset: 0)
[1]     Container: std::vector (size: 24)
          Param
            [0]
)");
}

TEST(BreakCyclesTest, SameCycleBreakerReusedAcrossSiblingEdges) {
  // A doubly-linked-style Node where both `next` and `prev` close a cycle
  // back to the same ancestor. TopoSorter/CodeGen key off node identity, so
  // wrapping the same underlying type twice would otherwise emit two
  // identically-named TypeHandler<Ctx, OICycleBreaker<T>> specializations
  // (see BreakCycles.h's cycleBreakers_ comment) - both edges must end up
  // pointing at the exact same CycleBreaker node, not merely two nodes that
  // happen to look the same when printed.
  std::string_view input = R"(
[0] Class: Node (size: 24)
      Member: value (offset: 0)
        Primitive: int32_t
      Member: next (offset: 8)
[1]     Pointer
          [0]
      Member: prev (offset: 16)
[2]     Pointer
          [0]
)";
  input.remove_prefix(1);  // Remove initial '\n'

  TypeGraph typeGraph;
  TypeGraphParser parser{typeGraph};
  parser.parse(input);

  NodeTracker tracker;
  BreakCycles::createPass().run(typeGraph, tracker);

  auto& node = dynamic_cast<Class&>(typeGraph.rootTypes()[0].get());
  ASSERT_EQ(node.members.size(), 3u);

  auto& next = dynamic_cast<Pointer&>(node.members[1].type());
  auto& prev = dynamic_cast<Pointer&>(node.members[2].type());

  EXPECT_NE(dynamic_cast<CycleBreaker*>(&next.pointeeType()), nullptr);
  EXPECT_EQ(&next.pointeeType(), &prev.pointeeType())
      << "next and prev should share the exact same CycleBreaker node, not "
         "two separately-allocated ones";
}
