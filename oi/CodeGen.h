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

#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ContainerInfo.h"
#include "OICodeGenConfig.h"
#include "type_graph/TypeGraph.h"

struct drgn_type;
struct TypeHierarchy;
namespace oi::detail {
class SymbolService;
}
namespace oi::detail::type_graph {
class Class;
class Container;
class Member;
class PassManager;
class Primitive;
class Type;
}  // namespace oi::detail::type_graph

namespace oi::detail {

class CodeGen {
 public:
  CodeGen(const OICodeGenConfig& config);
  CodeGen(const OICodeGenConfig& config, SymbolService& symbols)
      : config_(config),
        symbols_(&symbols),
        addPolymorphicInheritanceChildrenPtr_(
            &CodeGen::addPolymorphicInheritanceChildren),
        getClassSizeFuncDefPolymorphicPtr_(
            &CodeGen::getClassSizeFuncDefPolymorphic) {
  }

  struct ExactName {
    std::string name;
  };
  struct HashedComponent {
    std::string name;
  };
  using RootFunctionName = std::variant<ExactName, HashedComponent>;

  /*
   * Helper function to perform all the steps required for code generation for a
   * single drgn_type.
   */
  bool codegenFromDrgn(struct drgn_type* drgnType, std::string& code);
  bool codegenFromDrgn(struct drgn_type* drgnType,
                       std::string linkageName,
                       std::string& code);
  void exportDrgnTypes(TypeHierarchy& th,
                       std::list<drgn_type>& drgnTypes,
                       drgn_type** rootType) const;

  bool registerContainers();
  void registerContainer(std::unique_ptr<ContainerInfo> containerInfo);
  void registerContainer(const std::filesystem::path& path);
  void addDrgnRoot(struct drgn_type* drgnType,
                   type_graph::TypeGraph& typeGraph);
  void transform(type_graph::TypeGraph& typeGraph);
  void generate(type_graph::TypeGraph& typeGraph,
                std::string& code,
                RootFunctionName rootName);

  /*
   * Research groundwork for byte-accurate object capture (see
   * docs/object-capture-initial-thoughts.md, not part of this repo) -
   * generates the read-side counterpart to generate() above: given a root
   * type's captured bytes, produce a live instance of it. Supports a
   * scalar (Primitive) root type, or a struct/class root whose members may
   * be any mix of scalars, enums, trivially-copyable unions, reconstructable
   * containers, and nested (non-union) classes/structs, recursively - no
   * pointer fixup yet - see
   * generateReconstructScalar/generateReconstructClassBody/emitReconstructValue.
   * Self-contained: clears `code` and emits its own includes/preamble, for
   * use when nothing else populated `code` for this TypeGraph first. For
   * the combined introspect+reconstruct-of-the-same-root case, see
   * appendReconstructFunctionBody below instead.
   */
  void generateReconstruct(type_graph::TypeGraph& typeGraph,
                           std::string& code,
                           RootFunctionName rootName);

  /*
   * The combined introspect+reconstruct case: appends reconstructImpl<T>'s
   * function body to `code` right after a generate() call already
   * populated it for introspectImpl<T> of the *same* root T (see
   * OIGenerator::generate()'s same-type check, which is what guarantees
   * that). Unlike generateReconstruct(), does not clear `code`, does not
   * emit includes/preamble, and - for a Class root - does not re-emit the
   * struct's OIInternal redeclaration, since generate() already emitted an
   * equivalent one for the same root; emitting it twice would be a
   * redefinition error.
   */
  void appendReconstructFunctionBody(type_graph::TypeGraph& typeGraph,
                                     std::string& code,
                                     RootFunctionName rootName);

 private:
  void generateReconstructScalar(type_graph::Primitive& primitive,
                                 const std::string& typeToHash,
                                 std::string& code);
  void generateReconstructClassPreamble(type_graph::TypeGraph& typeGraph,
                                        type_graph::Class& cls,
                                        std::string& code);
  void generateReconstructClassBody(type_graph::TypeGraph& typeGraph,
                                    type_graph::Class& cls,
                                    const std::string& typeToHash,
                                    std::string& code);
  std::string emitReconstructClassValue(type_graph::Class& cls,
                                        const std::string& parsedDataExpr,
                                        size_t& idCounter,
                                        std::string& code);
  void emitReconstructClassValueInto(type_graph::Class& cls,
                                     const std::string& storagePtrExpr,
                                     const std::string& parsedDataExpr,
                                     size_t& idCounter,
                                     std::string& code);
  void collectReconstructFieldExprs(type_graph::Class& cls,
                                    const std::string& parsedDataExpr,
                                    size_t& idCounter,
                                    std::string& code,
                                    std::vector<std::string>& namesOut,
                                    std::vector<std::string>& fieldExprsOut);
  void generateReconstructContainerBody(type_graph::TypeGraph& typeGraph,
                                        type_graph::Container& container,
                                        const std::string& typeToHash,
                                        std::string& code);
  std::string emitReconstructValue(type_graph::Type& elemType,
                                   const std::string& parsedDataExpr,
                                   size_t& idCounter,
                                   std::string& code);
  std::string emitReconstructPointerValue(type_graph::Type& pointeeType,
                                          const std::string& v,
                                          size_t& idCounter,
                                          std::string& code);
  bool emitReconstructContainerCyclicPointerValue(type_graph::Container& cont,
                                                  const std::string& v,
                                                  std::string& code);
  void emitReconstructTypeHandlerSupport(type_graph::TypeGraph& typeGraph,
                                         std::string& code);
  void emitAliasRegistries(type_graph::TypeGraph& typeGraph, std::string& code);
  const std::string& getOrEmitCycleReconstructHelper(type_graph::Class& cls);

  type_graph::TypeGraph typeGraph_;
  const OICodeGenConfig& config_;
  SymbolService* symbols_ = nullptr;
  std::vector<std::unique_ptr<ContainerInfo>> containerInfos_;
  std::unordered_set<const ContainerInfo*> definedContainers_;
  std::unordered_map<const type_graph::Class*, const type_graph::Member*>
      thriftIssetMembers_;
  // Research groundwork for byte-accurate object capture/reconstruction
  // (see docs/object-capture-initial-thoughts.md, not part of this repo) -
  // populated fresh by emitAliasRegistries for each reconstruct-generation
  // call, mapping a "container-type-name|pointee-type-name" key to the
  // index of the __oi_alias_registry_N variable it just declared, so
  // emitReconstructValue's later, per-member "pointer" branch calls know
  // which one to reference for a given container instance without needing
  // that index threaded through every function's parameters.
  std::unordered_map<std::string, size_t> aliasRegistryIndices_;
  // Also populated fresh by emitAliasRegistries for each reconstruct-
  // generation call - every Class that appears as *some* CycleBreaker's
  // underlying type anywhere in the graph (see BreakCycles), regardless of
  // which specific edge(s) BreakCycles actually rewrote to reach it. This
  // has to be a type-wide property, not a per-edge one: a Class capable of
  // closing a cycle through *one* of its own members might be reached
  // elsewhere in the same graph via a perfectly ordinary edge that
  // BreakCycles never touched (e.g. the very first edge that reaches it,
  // before it's "on path") - that instance still needs to register its own
  // address before its fields are decoded, since some *other* edge further
  // down the same recursion may still refer back to it.
  std::unordered_set<const type_graph::Class*> cycleCapableClasses_;
  // getOrEmitCycleReconstructHelper's cache: a cycle-capable Class's own
  // field-decode logic (emitReconstructClassValueInto) must be generated
  // exactly once, as a named, reusable function, rather than re-expanded
  // inline every time some edge reaches it - the type graph has no notion
  // of recursion depth, so naively inlining it at every reference recurses
  // this codegen itself unboundedly (this project's own code hitting the
  // same class of problem #293 was originally about, just one remove
  // further). Keyed and populated the same way cycleCapableClasses_ is
  // (fresh per reconstruct-generation call) - see
  // getOrEmitCycleReconstructHelper's own doc for the full mechanism.
  std::unordered_map<const type_graph::Class*, std::string>
      cycleReconstructHelperNames_;
  std::string cycleReconstructHelpersCode_;
  size_t cycleReconstructHelperCounter_ = 0;

  bool codegenFromDrgn(struct drgn_type* drgnType,
                       std::string& code,
                       RootFunctionName name);

  /*
   * Adds the drgn-based passes which discover a polymorphic type's children,
   * for the `Feature::PolymorphicInheritance` branch of `transform()`.
   * Defined in CodeGenDrgn.cpp, and only ever reached when this CodeGen was
   * constructed with a SymbolService (see the constructor's DCHECK).
   *
   * transform() calls this indirectly, through
   * `addPolymorphicInheritanceChildrenPtr_`, rather than by name: CodeGen.cpp
   * (part of the drgn-free `codegen` library) must never itself reference a
   * symbol that only CodeGenDrgn.cpp (part of `codegen_drgn`) defines, or
   * consumers which only use the 1-arg constructor (i.e. oilgen) would be
   * unable to link without also pulling in SymbolService/drgn. Taking this
   * function's address is confined to the 2-arg constructor above, which is
   * only ever instantiated in translation units that already link
   * codegen_drgn.
   */
  void addPolymorphicInheritanceChildren(type_graph::PassManager& pm,
                                         type_graph::TypeGraph& typeGraph);

  void genDefsThrift(const type_graph::TypeGraph& typeGraph, std::string& code);
  void addGetSizeFuncDefs(const type_graph::TypeGraph& typeGraph,
                          std::string& code);
  void getClassSizeFuncDef(const type_graph::Class& c, std::string& code);
  void getClassSizeFuncConcrete(std::string_view funcName,
                                const type_graph::Class& c,
                                std::string& code) const;
  /*
   * The polymorphic-inheritance variant of `getClassSizeFuncDef`, which
   * resolves each concrete subclass's vtable address via SymbolService.
   * Defined in CodeGenDrgn.cpp; only reached when this CodeGen was
   * constructed with a SymbolService (see the constructor's DCHECK). Called
   * indirectly via `getClassSizeFuncDefPolymorphicPtr_` - see the comment on
   * `addPolymorphicInheritanceChildren` above for why.
   */
  void getClassSizeFuncDefPolymorphic(const type_graph::Class& c,
                                      std::string& code);
  void addTypeHandlers(const type_graph::TypeGraph& typeGraph,
                       std::string& code);

  void genClassTypeHandler(const type_graph::Class& c, std::string& code);
  void genClassStaticType(const type_graph::Class& c, std::string& code);
  void genClassTraversalFunction(const type_graph::Class& c, std::string& code);
  void genClassTreeBuilderInstructions(const type_graph::Class& c,
                                       std::string& code);

  void (CodeGen::*addPolymorphicInheritanceChildrenPtr_)(
      type_graph::PassManager&, type_graph::TypeGraph&) = nullptr;
  void (CodeGen::*getClassSizeFuncDefPolymorphicPtr_)(const type_graph::Class&,
                                                      std::string&) = nullptr;
};

}  // namespace oi::detail
