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

TEST(BreakCyclesTest, ContainerParamCycleBroken) {
  // A Container's template-param edge that closes a cycle back to its own
  // ancestor - the shape this pass gained the ability to fix in the joint
  // shared_ptr/unique_ptr increment (see BreakCycles::visit(Container&)).
  // std::vector stands in here for e.g. a self-referential
  // std::shared_ptr<Node>/std::unique_ptr<Node> member - the rewrite is
  // container-kind-agnostic, keyed only on "does this Param's target sit
  // on the current DFS path."
  test(BreakCycles::createPass(),
       R"(
[0] Class: Node (size: 24)
      Member: next (offset: 0)
[1]     Container: std::vector (size: 24)
          Param
            [0]
)",
       R"(
[0] Class: Node (size: 24)
      Member: next (offset: 0)
[1]     Container: std::vector (size: 24)
          Param
            CycleBreaker
              [0]
)");
}

TEST(BreakCyclesTest, NonCyclicContainerParamUntouched) {
  // An ordinary (non-self-referential) container member - nothing for this
  // pass to do, same as NonCyclicPointerUntouched above but for a
  // Container's Param edge.
  testNoChange(BreakCycles::createPass(), R"(
[0] Class: Outer (size: 24)
      Member: values (offset: 0)
[1]     Container: std::vector (size: 24)
          Param
[2]         Class: Inner (size: 4)
              Member: x (offset: 0)
                Primitive: int32_t
)");
}

TEST(BreakCyclesTest, ContainerMediatedUnderlyingCycleLeftAlone) {
  // visit(Container&) only rewrites template-param edges - a cycle running
  // through a container's own `underlying()` type (not a template param)
  // still isn't a shape this pass knows how to break, and falls through to
  // the ordinary mutate() catch-all, left exactly as-is for DetectCycles to
  // catch afterwards with its own diagnostic.
  testNoChange(BreakCycles::createPass(), R"(
[0] Class: Node (size: 24)
      Member: next (offset: 0)
[1]     Container: std::vector (size: 24)
          Underlying
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

TEST(BreakCyclesTest, SameCycleBreakerReusedAcrossPointerAndContainerEdges) {
  // A Node reachable both via an ordinary Pointer edge (`next`) and via a
  // Container's template-param edge (`owned`, standing in for e.g.
  // std::shared_ptr<Node>/std::unique_ptr<Node>) - both closing a cycle
  // back to the same ancestor. wrapInCycleBreaker is one shared helper
  // keyed by NodeId regardless of which visit() called it
  // (visit(Pointer&)/visit(Reference&) vs. the new visit(Container&)), so
  // both edges must end up pointing at the exact same CycleBreaker node -
  // this is what makes the capture-side DynBytes rework (PR #76) and
  // genCycleBreakerTypeHandler apply to shared_ptr/unique_ptr for free,
  // with no changes of their own.
  std::string_view input = R"(
[0] Class: Node (size: 32)
      Member: value (offset: 0)
        Primitive: int32_t
      Member: next (offset: 8)
[1]     Pointer
          [0]
      Member: owned (offset: 16)
[2]     Container: std::vector (size: 24)
          Param
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
  auto& owned = dynamic_cast<Container&>(node.members[2].type());
  ASSERT_EQ(owned.templateParams.size(), 1u);

  EXPECT_NE(dynamic_cast<CycleBreaker*>(&next.pointeeType()), nullptr);
  EXPECT_EQ(&next.pointeeType(), &owned.templateParams[0].type())
      << "the Pointer edge and the Container Param edge should share the "
         "exact same CycleBreaker node, not two separately-allocated ones";
}
