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
#include "BreakCycles.h"

#include "TypeGraph.h"

namespace oi::detail::type_graph {

Pass BreakCycles::createPass() {
  auto fn = [](TypeGraph& typeGraph, NodeTracker&) {
    BreakCycles pass;
    pass.breakCycles(typeGraph, typeGraph.rootTypes());
  };

  return Pass("BreakCycles", fn);
}

void BreakCycles::breakCycles(
    TypeGraph& typeGraph,
    std::vector<std::reference_wrapper<Type>>& rootTypes) {
  typeGraph_ = &typeGraph;
  for (auto& root : rootTypes) {
    root = mutate(root);
  }
}

Type& BreakCycles::mutate(Type& type) {
  NodeId id = type.id();
  if (id < 0) {
    // No stable id (a synthetic, one-off node) - can't be part of a cycle
    // detectable this way; just recurse.
    return type.accept(*this);
  }

  if (auto it = done_.find(id); it != done_.end()) {
    return *it->second;
  }

  if (onPath_.contains(id)) {
    // A cycle this pass doesn't know how to break - not a plain Class-member
    // Pointer/Reference edge (those are intercepted in visit(Pointer&)/
    // visit(Reference&) before they ever reach here), e.g. one mediated by
    // a container's template parameter instead. Leave it exactly as-is
    // rather than recursing back into a node that's still being processed
    // further up this same call stack (which would recurse unbounded, an
    // actual stack overflow, not just a bad type graph) - DetectCycles runs
    // immediately after this pass and will still catch it with its own
    // clear diagnostic.
    return type;
  }

  onPath_.insert(id);
  Type& mutated = type.accept(*this);
  onPath_.erase(id);
  done_.emplace(id, &mutated);
  return mutated;
}

Type& BreakCycles::visit(Pointer& p) {
  Type& pointee = p.pointeeType();
  NodeId id = pointee.id();
  if (id >= 0 && onPath_.contains(id)) {
    p.setPointeeType(wrapInCycleBreaker(pointee));
    return p;
  }
  p.setPointeeType(mutate(pointee));
  return p;
}

Type& BreakCycles::visit(Reference& r) {
  Type& pointee = r.pointeeType();
  NodeId id = pointee.id();
  if (id >= 0 && onPath_.contains(id)) {
    r.setPointeeType(wrapInCycleBreaker(pointee));
    return r;
  }
  r.setPointeeType(mutate(pointee));
  return r;
}

Type& BreakCycles::wrapInCycleBreaker(Type& pointee) {
  NodeId id = pointee.id();
  if (id >= 0) {
    if (auto it = cycleBreakers_.find(id); it != cycleBreakers_.end()) {
      return *it->second;
    }
  }

  auto& cycleBreaker = typeGraph_->makeType<CycleBreaker>(pointee);
  if (id >= 0) {
    cycleBreakers_.emplace(id, &cycleBreaker);
  }
  return cycleBreaker;
}

}  // namespace oi::detail::type_graph
