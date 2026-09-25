/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <unordered_map>
#include <unordered_set>

#include "PassManager.h"
#include "Types.h"
#include "Visitor.h"

namespace oi::detail::type_graph {

class TypeGraph;

/*
 * BreakCycles
 *
 * Research groundwork for byte-accurate object capture/reconstruction (see
 * docs/object-capture-initial-thoughts.md, not part of this repo) - Stage 3
 * of the agreed fix for
 * https://github.com/facebookexperimental/object-introspection/issues/293
 * ("Cycles are problematic in TreeBuilder V2").
 *
 * Runs before DetectCycles. Walks the type graph with the same ordinary
 * three-colour DFS (in-progress / done / unvisited, tracked via each node's
 * own stable NodeId) that DetectCycles uses to report cycles, but instead of
 * throwing when a Pointer/Reference edge points back to a live ancestor, it
 * rewrites that one edge's pointee to a `CycleBreaker` wrapper: an empty
 * subclass of the real pointee with identical size/layout but a distinct
 * C++ identity, so the member's declared pointer type in the generated code
 * (`OICycleBreaker<Node_0>* next;` rather than `Node_0* next;`) no longer
 * names the type that's still being defined further up the same DFS path.
 * `CodeGen::genCycleBreakerTypeHandler` gives `OICycleBreaker<T>` its own
 * trivial `TypeHandler` specialization that reinterprets back to the real
 * type at the point it's actually traversed, once that real type is fully
 * defined - see that function for the full mechanism.
 *
 * Whatever cycle shape this pass can't safely break (anything not mediated
 * by a plain Class-member Pointer/Reference edge - e.g. a cycle running
 * through a container's template parameter, such as a self-referential
 * `std::shared_ptr<Node>` member) is deliberately left alone; DetectCycles
 * runs immediately afterwards and still catches it with its usual clear
 * diagnostic, exactly as before this pass existed.
 */
class BreakCycles final : public RecursiveMutator {
 public:
  static Pass createPass();

  void breakCycles(TypeGraph& typeGraph,
                   std::vector<std::reference_wrapper<Type>>& rootTypes);

  using RecursiveMutator::mutate;

  Type& mutate(Type& type) override;
  Type& visit(Pointer& p) override;
  Type& visit(Reference& r) override;

 private:
  Type& wrapInCycleBreaker(Type& pointee);

  TypeGraph* typeGraph_ = nullptr;

  // Nodes on the current DFS path (a live ancestor of whatever we're
  // visiting right now) - a Pointer/Reference edge into one of these is
  // exactly the shape #293 can't compile.
  std::unordered_set<NodeId> onPath_;
  // Nodes whose entire reachable subgraph has already been fully mutated -
  // safe to reuse the result if reached again via a different path
  // (ordinary DAG sharing, not a cycle).
  std::unordered_map<NodeId, Type*> done_;
  // One CycleBreaker per underlying type, reused across every edge that
  // wraps the same pointee (e.g. both `next` and `prev` of a
  // doubly-linked node) - TopoSorter/CodeGen key off of node identity, so
  // wrapping the same type twice would otherwise emit two identically-named
  // `TypeHandler<Ctx, OICycleBreaker<T>>` specializations.
  std::unordered_map<NodeId, Type*> cycleBreakers_;
};

}  // namespace oi::detail::type_graph
