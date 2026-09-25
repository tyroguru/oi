#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "PassManager.h"
#include "Types.h"
#include "Visitor.h"

namespace oi::detail::type_graph {

/*
 * DetectCycles
 *
 * Research groundwork for byte-accurate object capture/reconstruction (see
 * docs/object-capture-initial-thoughts.md, not part of this repo) - Stage 1
 * of the agreed fix for
 * https://github.com/facebookexperimental/object-introspection/issues/293
 * ("Cycles are problematic in TreeBuilder V2"): any cycle in the type graph
 * - a self-referential struct via a raw pointer/reference, or via a
 * container like std::shared_ptr - makes it impossible for TreeBuilder V2's
 * static type system (a plain, non-recursive C++ type alias chain) to be
 * generated at all. Today that fails with either an enormous, illegible
 * template-instantiation error or an outright compiler segfault, deep
 * inside generated code the user never wrote and can't see.
 *
 * This pass runs first (before anything else needs the graph to be
 * acyclic), walks the *already fully-built* type graph - not the user's
 * source - looking for a cycle with an ordinary three-colour DFS
 * (in-progress / done / unvisited, tracked via each node's own stable
 * NodeId), and if it finds one, aborts code generation immediately with a
 * clear error naming the exact chain of member accesses that closes the
 * loop, in terms of the user's own type and member names - not OI's
 * internal template machinery.
 *
 * Deliberately does not attempt to fix or work around the cycle - see
 * #293's own comment thread for that follow-up ("Stage 3", the
 * `CycleBreaker` design - not implemented here). This pass only turns a
 * guaranteed, incomprehensible failure into an immediate, legible one.
 *
 * Scope for this first pass: walks Class members/base classes and
 * Container template parameters (the two edge kinds actually capable of
 * forming a cycle a real user would hit - a self-referential struct field,
 * or a self-referential smart pointer). Deliberately does not walk Class
 * template parameters or polymorphic children (e.g. CRTP) - a different,
 * rarer shape of self-reference, left for a later increment if it turns
 * out to matter in practice.
 */
class DetectCycles : public RecursiveVisitor {
 public:
  static Pass createPass();

  void detectCycles(std::vector<std::reference_wrapper<Type>>& rootTypes);

  void accept(Type& type) override;
  void visit(Class& c) override;
  void visit(Container& c) override;
  void visit(CycleBreaker&) override;

 private:
  void pushEdge(std::string description);
  void popEdge();
  [[noreturn]] void reportCycle(Type& target);

  // Nodes on the current DFS path (an ancestor of whatever we're visiting
  // right now) - finding one of these again is exactly what a cycle is.
  std::unordered_set<NodeId> onPath_;
  // Nodes whose entire reachable subgraph has already been fully explored
  // with no cycle found - safe to skip re-exploring if reached again via a
  // different path (ordinary DAG sharing, not a cycle).
  std::unordered_set<NodeId> done_;
  // For each node currently on the path, the index into pathDescriptions_
  // where its own subtree's edges begin - lets reportCycle slice out
  // exactly the chain that closes the loop, not the whole path from the
  // root.
  std::unordered_map<NodeId, size_t> pathStart_;
  std::vector<std::string> pathDescriptions_;
};

}  // namespace oi::detail::type_graph
