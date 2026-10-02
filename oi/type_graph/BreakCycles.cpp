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

Type* BreakCycles::onPathTarget(Type& type) const {
  // Look through Typedefs: in C's `typedef struct foo_s foo_t;` idiom a
  // self-referential member is `foo_t* next`, so the edge's immediate pointee
  // is the Typedef, not the Class that's on the path. Wrap the underlying
  // Class, exactly as if the member had been declared `struct foo_s* next`.
  Type* t = &type;
  while (true) {
    NodeId id = t->id();
    if (id >= 0 && onPath_.contains(id)) {
      break;
    }
    auto* td = dynamic_cast<Typedef*>(t);
    if (td == nullptr) {
      return nullptr;
    }
    t = &td->underlyingType();
  }
  while (auto* td = dynamic_cast<Typedef*>(t)) {
    t = &td->underlyingType();
  }
  return t;
}

Type& BreakCycles::visit(Pointer& p) {
  Type& pointee = p.pointeeType();
  if (Type* target = onPathTarget(pointee)) {
    p.setPointeeType(wrapInCycleBreaker(*target));
    return p;
  }
  p.setPointeeType(mutate(pointee));
  return p;
}

Type& BreakCycles::visit(Reference& r) {
  Type& pointee = r.pointeeType();
  if (Type* target = onPathTarget(pointee)) {
    r.setPointeeType(wrapInCycleBreaker(*target));
    return r;
  }
  r.setPointeeType(mutate(pointee));
  return r;
}

Type& BreakCycles::visit(Container& c) {
  // Same shape as visit(Pointer&)/visit(Reference&) above, applied to each
  // template param instead of a single pointee - e.g. std::shared_ptr<T>/
  // std::unique_ptr<T>'s one Param. A Container can have more than one
  // param (e.g. std::map's key/value), so each is checked independently;
  // in practice only a single-param, pointer-shaped container can ever
  // legitimately close a cycle this way (T is the Class itself), but there's
  // nothing container-kind-specific in the check.
  for (auto& param : c.templateParams) {
    Type& paramType = param.type();
    if (Type* target = onPathTarget(paramType)) {
      param.setType(wrapInCycleBreaker(*target));
    } else {
      param.setType(mutate(paramType));
    }
  }

  // `underlying()` is a container-kind-internal detail (not a template
  // param), not the shape this pass targets - falls through to the
  // ordinary mutate() catch-all (leaves an on-path cycle untouched for
  // DetectCycles to catch), unchanged from before this override existed.
  c.setUnderlying(mutate(c.underlying()));

  return c;
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
