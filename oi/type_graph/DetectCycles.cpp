#include "DetectCycles.h"

#include <stdexcept>

#include "TypeGraph.h"

namespace oi::detail::type_graph {

Pass DetectCycles::createPass() {
  auto fn = [](TypeGraph& typeGraph, NodeTracker&) {
    DetectCycles pass;
    pass.detectCycles(typeGraph.rootTypes());
  };

  return Pass("DetectCycles", fn);
}

void DetectCycles::detectCycles(
    std::vector<std::reference_wrapper<Type>>& rootTypes) {
  for (Type& root : rootTypes) {
    accept(root);
  }
}

void DetectCycles::accept(Type& type) {
  NodeId id = type.id();
  if (id < 0) {
    // No stable id (a synthetic, one-off node) - can't be part of a cycle
    // detectable this way; just recurse.
    type.accept(*this);
    return;
  }

  if (onPath_.contains(id)) {
    reportCycle(type);
  }
  if (done_.contains(id)) {
    // Already fully explored via some other path, with no cycle found -
    // ordinary DAG sharing, not a cycle. Safe to skip.
    return;
  }

  onPath_.insert(id);
  pathStart_[id] = pathDescriptions_.size();
  type.accept(*this);
  onPath_.erase(id);
  pathStart_.erase(id);
  done_.insert(id);
}

void DetectCycles::visit(Class& c) {
  for (auto& parent : c.parents) {
    pushEdge(c.name() + " (base class): " + parent.type().name());
    accept(parent.type());
    popEdge();
  }
  for (auto& mem : c.members) {
    pushEdge(c.name() + "::" + mem.name + " : " + mem.type().name());
    accept(mem.type());
    popEdge();
  }
}

void DetectCycles::visit(Container& c) {
  for (size_t i = 0; i < c.templateParams.size(); i++) {
    pushEdge(c.name() + " (template parameter " + std::to_string(i) +
             "): " + c.templateParams[i].type().name());
    accept(c.templateParams[i].type());
    popEdge();
  }
}

void DetectCycles::pushEdge(std::string description) {
  pathDescriptions_.push_back(std::move(description));
}

void DetectCycles::popEdge() {
  pathDescriptions_.pop_back();
}

void DetectCycles::reportCycle(Type& target) {
  size_t start = pathStart_.at(target.id());
  std::string msg =
      "OIL cannot generate code for this type: it contains a reference "
      "cycle, which its static type system cannot represent (see "
      "https://github.com/facebookexperimental/object-introspection/"
      "issues/293). The cycle is:\n";
  for (size_t i = start; i < pathDescriptions_.size(); i++) {
    msg += "  " + pathDescriptions_[i] + "\n";
  }
  msg += "  ...back to " + target.name() +
         ", which is already being processed above.\n";
  throw std::runtime_error(msg);
}

}  // namespace oi::detail::type_graph
