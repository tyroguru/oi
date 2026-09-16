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
#include <glog/logging.h>

#include <boost/format.hpp>

#include "CodeGen.h"
#include "oi/SymbolService.h"
#include "oi/TypeHierarchy.h"
#include "type_graph/AddChildren.h"
#include "type_graph/AlignmentCalc.h"
#include "type_graph/DrgnExporter.h"
#include "type_graph/DrgnParser.h"
#include "type_graph/Flattener.h"
#include "type_graph/IdentifyContainers.h"
#include "type_graph/PassManager.h"
#include "type_graph/Prune.h"
#include "type_graph/TypeIdentifier.h"

/*
 * This file holds every part of CodeGen which depends on drgn: parsing types
 * directly from DWARF (`addDrgnRoot`/`codegenFromDrgn`), exporting the type
 * graph back to drgn types (`exportDrgnTypes`, used by OICache), and the
 * polymorphic-inheritance pass which re-parses a type's children via drgn.
 *
 * Keeping these out of CodeGen.cpp means consumers which only ever build a
 * type graph from a Clang AST (i.e. oilgen, via ClangTypeParser) don't need
 * to link against SymbolService/drgn at all.
 */
namespace oi::detail {

using type_graph::AddChildren;
using type_graph::AlignmentCalc;
using type_graph::Class;
using type_graph::DrgnParser;
using type_graph::DrgnParserOptions;
using type_graph::Flattener;
using type_graph::IdentifyContainers;
using type_graph::Prune;
using type_graph::Type;
using type_graph::TypeGraph;
using type_graph::TypeIdentifier;

bool CodeGen::codegenFromDrgn(struct drgn_type* drgnType,
                              std::string linkageName,
                              std::string& code) {
  return codegenFromDrgn(drgnType, code, ExactName{std::move(linkageName)});
}

bool CodeGen::codegenFromDrgn(struct drgn_type* drgnType, std::string& code) {
  return codegenFromDrgn(
      drgnType, code, HashedComponent{SymbolService::getTypeName(drgnType)});
}

bool CodeGen::codegenFromDrgn(struct drgn_type* drgnType,
                              std::string& code,
                              RootFunctionName name) {
  if (!registerContainers())
    return false;

  try {
    addDrgnRoot(drgnType, typeGraph_);
  } catch (const type_graph::DrgnParserError& err) {
    LOG(ERROR) << "Error parsing DWARF: " << err.what();
    return false;
  }

  transform(typeGraph_);
  generate(typeGraph_, code, std::move(name));
  return true;
}

void CodeGen::exportDrgnTypes(TypeHierarchy& th,
                              std::list<drgn_type>& drgnTypes,
                              drgn_type** rootType) const {
  assert(typeGraph_.rootTypes().size() == 1);

  type_graph::DrgnExporter drgnExporter{th, drgnTypes};
  for (auto& type : typeGraph_.rootTypes()) {
    *rootType = drgnExporter.accept(type);
  }
}

void CodeGen::addDrgnRoot(struct drgn_type* drgnType, TypeGraph& typeGraph) {
  DrgnParserOptions options{
      .chaseRawPointers = config_.features[Feature::ChaseRawPointers],
  };
  DrgnParser drgnParser{typeGraph, options};
  Type& parsedRoot = drgnParser.parse(drgnType);
  typeGraph.addRoot(parsedRoot);
}

void CodeGen::getClassSizeFuncDefPolymorphic(const Class& c,
                                             std::string& code) {
  getClassSizeFuncConcrete("getSizeTypeConcrete", c, code);

  std::vector<SymbolInfo> childVtableAddrs;
  childVtableAddrs.reserve(c.children.size());

  for (const Type& childType : c.children) {
    auto* childClass = dynamic_cast<const Class*>(&childType);
    if (childClass == nullptr) {
      abort();  // TODO
    }
    //      TODO:
    //      auto fqChildName = *fullyQualifiedName(child);
    auto fqChildName = "TODO - implement me";

    // We must split this assignment and append because the C++ standard lacks
    // an operator for concatenating std::string and std::string_view...
    std::string childVtableName = "vtable for ";
    childVtableName += fqChildName;

    auto optVtableSym = symbols_->locateSymbol(childVtableName, true);
    if (!optVtableSym) {
      //        LOG(ERROR) << "Failed to find vtable address for '" <<
      //        childVtableName; LOG(ERROR) << "Falling back to non dynamic
      //        mode";
      childVtableAddrs.clear();  // TODO why??
      break;
    }
    childVtableAddrs.push_back(*optVtableSym);
  }

  code += "void getSizeType(const " + c.name() + " &t, size_t &returnArg) {\n";
  code += "  auto *vptr = *reinterpret_cast<uintptr_t * const *>(&t);\n";
  code += "  uintptr_t topOffset = *(vptr - 2);\n";
  code += "  uintptr_t vptrVal = reinterpret_cast<uintptr_t>(vptr);\n";

  for (size_t i = 0; i < c.children.size(); i++) {
    // The vptr will point to *somewhere* in the vtable of this object's
    // concrete class. The exact offset into the vtable can vary based on a
    // number of factors, so we compare the vptr against the vtable range for
    // each possible class to determine the concrete type.
    //
    // This works for C++ compilers which follow the GNU v3 ABI, i.e. GCC and
    // Clang. Other compilers may differ.
    const Type& child = c.children[i];
    auto& vtableSym = childVtableAddrs[i];
    uintptr_t vtableMinAddr = vtableSym.addr;
    uintptr_t vtableMaxAddr = vtableSym.addr + vtableSym.size;
    code += "  if (vptrVal >= 0x" +
            (boost::format("%x") % vtableMinAddr).str() + " && vptrVal < 0x" +
            (boost::format("%x") % vtableMaxAddr).str() + ") {\n";
    code += "    SAVE_DATA(" + std::to_string(i) + ");\n";
    code +=
        "    uintptr_t baseAddress = reinterpret_cast<uintptr_t>(&t) + "
        "topOffset;\n";
    code += "    getSizeTypeConcrete(*reinterpret_cast<const " + child.name() +
            "*>(baseAddress), returnArg);\n";
    code += "    return;\n";
    code += "  }\n";
  }

  code += "  SAVE_DATA(-1);\n";
  code += "  getSizeTypeConcrete(t, returnArg);\n";
  code += "}\n";
}

void CodeGen::addPolymorphicInheritanceChildren(
    type_graph::PassManager& pm, type_graph::TypeGraph& typeGraph) {
  // Parse new children nodes
  DrgnParserOptions options{
      .chaseRawPointers = config_.features[Feature::ChaseRawPointers],
  };
  DrgnParser drgnParser{typeGraph, options};
  pm.addPass(AddChildren::createPass(drgnParser, *symbols_));

  // Re-run passes over newly added children
  pm.addPass(IdentifyContainers::createPass(containerInfos_));
  pm.addPass(Flattener::createPass());
  pm.addPass(AlignmentCalc::createPass());
  pm.addPass(TypeIdentifier::createPass(config_.passThroughTypes));
  if (config_.features[Feature::PruneTypeGraph])
    pm.addPass(Prune::createPass());
}

}  // namespace oi::detail
