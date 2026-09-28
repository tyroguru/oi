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

#include "oi/OIGenerator.h"

#include <clang/AST/Mangle.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Sema/Sema.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/Tooling.h>
#include <glog/logging.h>

#include <fstream>
#include <range/v3/core.hpp>
#include <range/v3/view/drop.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/for_each.hpp>
#include <range/v3/view/take.hpp>
#include <range/v3/view/transform.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>

#include "oi/CodeGen.h"
#include "oi/Config.h"
#include "oi/Headers.h"
#include "oi/OICodeGenConfig.h"
#include "oi/OICompiler.h"
#include "oi/type_graph/ClangTypeParser.h"
#include "oi/type_graph/TypeGraph.h"
#include "oi/type_graph/Types.h"

namespace oi::detail {
namespace {

class ConsumerContext;

class CreateTypeGraphConsumer;
class CreateTypeGraphAction : public clang::ASTFrontendAction {
 public:
  CreateTypeGraphAction(ConsumerContext& ctx_) : ctx{ctx_} {
  }

  void ExecuteAction() override;
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
      clang::CompilerInstance& CI, clang::StringRef file) override;

 private:
  ConsumerContext& ctx;
};

class CreateTypeGraphActionFactory
    : public clang::tooling::FrontendActionFactory {
 public:
  CreateTypeGraphActionFactory(ConsumerContext& ctx_) : ctx{ctx_} {
  }

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<CreateTypeGraphAction>(ctx);
  }

 private:
  ConsumerContext& ctx;
};

class ConsumerContext {
 public:
  ConsumerContext(const std::vector<std::unique_ptr<ContainerInfo>>& cis)
      : containerInfos{cis} {
  }

  type_graph::TypeGraph typeGraph;
  std::unordered_map<std::string, type_graph::Type*> nameToTypeMap;
  std::unordered_map<std::string, type_graph::Type*> nameToReconstructTypeMap;
  // Each introspect root's linkage name, paired with the rootTypes() index
  // it was added at (see HandleTranslationUnit's addRoot loop below). The
  // index - not a Type* - is what stays valid after transform() runs, since
  // RemoveTopLevelPointer can replace the Type& stored at a given rootTypes()
  // slot in place. At most one reconstruct root is currently supported, so a
  // single optional suffices for it.
  std::vector<std::pair<std::string, size_t>> introspectRootIndices;
  std::optional<std::pair<std::string, size_t>> reconstructRootIndex;
  std::optional<bool> pic;
  const std::vector<std::unique_ptr<ContainerInfo>>& containerInfos;
  std::set<std::string_view> typesToStub;
  std::set<std::string_view> mustProcessTemplateParams;
  bool chaseRawPointers = false;

 private:
  clang::Sema* sema = nullptr;
  friend CreateTypeGraphConsumer;
  friend CreateTypeGraphAction;
};

}  // namespace

int OIGenerator::generate(clang::tooling::CompilationDatabase& db,
                          const std::vector<std::string>& sourcePaths) {
  std::map<Feature, bool> featuresMap = {
      {Feature::TypeGraph, true},
      {Feature::TreeBuilderV2, true},
      {Feature::Library, true},
      {Feature::PackStructs, true},
      {Feature::PruneTypeGraph, true},
  };

  OICodeGenConfig generatorConfig{};
  OICompiler::Config compilerConfig{};

  auto features = config::processConfigFiles(
      configFilePaths, featuresMap, compilerConfig, generatorConfig);
  if (!features) {
    LOG(ERROR) << "failed to process config file";
    return -1;
  }
  generatorConfig.features = *features;
  compilerConfig.features = *features;

  std::vector<std::unique_ptr<ContainerInfo>> containerInfos;
  containerInfos.reserve(generatorConfig.containerConfigPaths.size());
  try {
    for (const auto& path : generatorConfig.containerConfigPaths) {
      auto info = std::make_unique<ContainerInfo>(path);
      if (info->requiredFeatures != (*features & info->requiredFeatures)) {
        VLOG(1) << "Skipping container (feature conflict): " << info->typeName;
        continue;
      }
      containerInfos.emplace_back(std::move(info));
    }
  } catch (const ContainerInfoError& err) {
    LOG(ERROR) << "Error reading container TOML file " << err.what();
    return -1;
  }

  ConsumerContext ctx{containerInfos};
  ctx.chaseRawPointers = generatorConfig.features[Feature::ChaseRawPointers];

  for (const auto& [stubType, stubMember] : generatorConfig.membersToStub) {
    if (stubMember == "*")
      ctx.typesToStub.insert(stubType);
  }

  for (const auto& cInfo : generatorConfig.passThroughTypes)
    ctx.mustProcessTemplateParams.insert(cInfo.typeName);

  CreateTypeGraphActionFactory factory{ctx};

  clang::tooling::ClangTool tool{db, sourcePaths};

  // The compilation database's own recorded flags aren't guaranteed to
  // include the compiler's default include search paths - reconstructing
  // those generally requires actually running the compiler driver as a
  // subprocess (e.g. to expand a wrapper script's implicit flags), which
  // ClangTool's in-process reinterpretation of the database doesn't do. Add
  // whatever user/system header paths the config file supplies (see
  // tools/config_gen.py) after the database's own flags, so they only fill
  // gaps rather than override anything the database already specifies
  // explicitly.
  std::vector<std::string> configHeaderArgs;
  for (const auto& path : compilerConfig.userHeaderPaths) {
    configHeaderArgs.push_back("-I" + path.string());
  }
  for (const auto& path : compilerConfig.sysHeaderPaths) {
    configHeaderArgs.push_back("-isystem");
    configHeaderArgs.push_back(path.string());
  }
  if (!configHeaderArgs.empty()) {
    tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
        configHeaderArgs, clang::tooling::ArgumentInsertPosition::END));
  }

  if (auto ret = tool.run(&factory); ret != 0) {
    return ret;
  }

  if (ctx.nameToReconstructTypeMap.size() > 1)
    throw std::logic_error(
        "found more than one oi::reconstruct<T>() site to generate for in "
        "one translation unit but we can't currently handle this case "
        "(multiple simultaneous oi::introspect<T>() sites are supported)");

  const bool haveIntrospect = !ctx.nameToTypeMap.empty();
  const bool haveReconstruct = !ctx.nameToReconstructTypeMap.empty();

  if (!haveIntrospect && !haveReconstruct) {
    LOG(ERROR) << "Nothing to generate!";
    return failIfNothingGenerated ? -1 : 0;
  }

  compilerConfig.usePIC = ctx.pic.value();
  CodeGen codegen{generatorConfig};
  for (auto&& ptr : containerInfos)
    codegen.registerContainer(std::move(ptr));
  codegen.transform(ctx.typeGraph);

  // Both an introspect<T>() and a reconstruct<U>() site were found - this is
  // only supported when U is identical to one of the (possibly many)
  // introspect roots. They weren't directly comparable at addRoot() time
  // (see HandleTranslationUnit): an introspect root was still wrapped in a
  // Reference type-graph node there, since nothing had stripped it yet,
  // while reconstruct's root never had one. transform()'s
  // RemoveTopLevelPointer pass (run just above, as part of the normal
  // pipeline) is what normalizes that away, which is why this check has to
  // live here and not earlier. Root identity is resolved by rootTypes()
  // index (see ConsumerContext's own comment on why), not by dereferencing
  // the Type* stored in nameToTypeMap/nameToReconstructTypeMap.
  std::optional<size_t> combinedIntrospectRootIndex;
  if (haveIntrospect && haveReconstruct) {
    type_graph::Type& reconstructRoot =
        ctx.typeGraph.rootTypes()[ctx.reconstructRootIndex->second];
    for (const auto& [name, index] : ctx.introspectRootIndices) {
      type_graph::Type& introspectRoot = ctx.typeGraph.rootTypes()[index];
      if (&introspectRoot == &reconstructRoot) {
        combinedIntrospectRootIndex = index;
        break;
      }
    }
    if (!combinedIntrospectRootIndex) {
      throw std::logic_error(
          "oi::introspect<T>() and oi::reconstruct<U>() for unrelated types "
          "in the same translation unit are not yet supported - U must be "
          "identical to one of this translation unit's oi::introspect<T>() "
          "root types");
    }
  }

  std::string code;
  if (haveIntrospect) {
    // forReconstruct is TU-wide (affects how every reachable class's
    // container-typed members are spelled - see genDefsClass's own
    // comment), not scoped to just the paired root - true whenever the
    // lone reconstruct site (if any) is being combined with one of these
    // introspect roots.
    codegen.generateSharedDefinitions(
        ctx.typeGraph,
        code,
        /* forReconstruct = */ combinedIntrospectRootIndex.has_value());
    for (const auto& [linkageName, index] : ctx.introspectRootIndices) {
      codegen.generateIntrospectRoot(code,
                                     ctx.typeGraph.rootTypes()[index],
                                     index,
                                     CodeGen::ExactName{linkageName});
    }
  }
  if (haveReconstruct) {
    const auto& linkageName = ctx.reconstructRootIndex->first;
    if (combinedIntrospectRootIndex) {
      // Same root as one of the generateIntrospectRoot() calls just above
      // (enforced by the type-identity check above) - append
      // reconstructImpl<T>'s function body to the code that call already
      // produced, instead of emitting a second, colliding copy of T's
      // OIInternal redeclaration.
      codegen.appendReconstructFunctionBody(ctx.typeGraph,
                                            code,
                                            CodeGen::ExactName{linkageName},
                                            *combinedIntrospectRootIndex);
    } else {
      codegen.generateReconstruct(
          ctx.typeGraph, code, CodeGen::ExactName{linkageName});
    }
  }

  std::string sourcePath = sourceFileDumpPath;
  if (sourceFileDumpPath.empty()) {
    // This is the path Clang acts as if it has compiled from e.g. for debug
    // information. It does not need to exist.
    sourcePath = "oil_jit.cpp";
  } else {
    std::ofstream outputFile(sourcePath);
    outputFile << code;
  }

  OICompiler compiler{{}, compilerConfig};
  return compiler.compile(code, sourcePath, outputPath) ? 0 : -1;
}

namespace {

class CreateTypeGraphConsumer : public clang::ASTConsumer {
 private:
  ConsumerContext& ctx;

 public:
  CreateTypeGraphConsumer(ConsumerContext& ctx_) : ctx(ctx_) {
  }

  void HandleTranslationUnit(clang::ASTContext& Context) override {
    auto* tu_decl = Context.getTranslationUnitDecl();
    auto decls = tu_decl->decls();
    auto oi_namespaces = decls | ranges::views::transform([](auto* p) {
                           return llvm::dyn_cast<clang::NamespaceDecl>(p);
                         }) |
                         ranges::views::filter([](auto* ns) {
                           return ns != nullptr && ns->getName() == "oi";
                         });
    if (oi_namespaces.empty()) {
      LOG(WARNING) << "Failed to find `oi` namespace. Does this input "
                      "include <oi/oi.h>?";
      return;
    }

    auto findFunctionTemplate = [&oi_namespaces](llvm::StringRef name) {
      return oi_namespaces |
             ranges::views::for_each([](auto* ns) { return ns->decls(); }) |
             ranges::views::transform([](auto* p) {
               return llvm::dyn_cast<clang::FunctionTemplateDecl>(p);
             }) |
             ranges::views::filter([name](auto* td) {
               return td != nullptr && td->getName() == name;
             }) |
             ranges::views::take(1) | ranges::to<std::vector>();
    };

    auto introspectImplTemplate = findFunctionTemplate("introspectImpl");
    auto reconstructImplTemplate = findFunctionTemplate("reconstructImpl");
    if (introspectImplTemplate.empty() && reconstructImplTemplate.empty()) {
      LOG(WARNING)
          << "Failed to find `oi::introspect` or `oi::reconstruct` within "
             "the `oi` namespace. Did you compile with "
             "`OIL_AOT_COMPILATION=1`?";
      return;
    }

    type_graph::ClangTypeParserOptions opts;
    opts.typesToStub = ctx.typesToStub;
    opts.mustProcessTemplateParams = ctx.mustProcessTemplateParams;
    opts.chaseRawPointers = ctx.chaseRawPointers;
    type_graph::ClangTypeParser parser{ctx.typeGraph, ctx.containerInfos, opts};
    auto& Sema = *ctx.sema;

    if (!introspectImplTemplate.empty()) {
      auto nameToClangTypeMap =
          introspectImplTemplate | ranges::views::for_each([](auto* td) {
            return td->specializations();
          }) |
          ranges::views::transform(
              [](auto* p) { return llvm::dyn_cast<clang::FunctionDecl>(p); }) |
          ranges::views::filter([](auto* p) { return p != nullptr; }) |
          ranges::views::transform(
              [](auto* fd) -> std::pair<std::string, const clang::Type*> {
                clang::ASTContext& Ctx = fd->getASTContext();
                clang::ASTNameGenerator ASTNameGen(Ctx);
                std::string name = ASTNameGen.getName(fd);

                assert(fd->getNumParams() == 1);
                const clang::Type* type =
                    fd->parameters()[0]->getType().getTypePtr();
                return {name, type};
              }) |
          ranges::to<std::unordered_map>();

      auto els =
          nameToClangTypeMap |
          ranges::views::transform(
              [&parser, &Context, &Sema](
                  auto& p) -> std::pair<std::string, type_graph::Type*> {
                return {p.first, &parser.parse(Context, Sema, *p.second)};
              });
      ctx.nameToTypeMap.insert(els.begin(), els.end());
    }

    if (!reconstructImplTemplate.empty()) {
      auto nameToClangTypeMap =
          reconstructImplTemplate | ranges::views::for_each([](auto* td) {
            return td->specializations();
          }) |
          ranges::views::transform(
              [](auto* p) { return llvm::dyn_cast<clang::FunctionDecl>(p); }) |
          ranges::views::filter([](auto* p) { return p != nullptr; }) |
          ranges::views::transform(
              [](auto* fd) -> std::pair<std::string, const clang::Type*> {
                clang::ASTContext& Ctx = fd->getASTContext();
                clang::ASTNameGenerator ASTNameGen(Ctx);
                std::string name = ASTNameGen.getName(fd);

                // reconstructImpl<T>(std::span<const uint8_t>) - T comes
                // from the template argument, unlike introspectImpl<T>
                // above where T is the (unrelated) parameter type.
                const auto* templateArgs = fd->getTemplateSpecializationArgs();
                assert(templateArgs && templateArgs->size() == 1);
                const clang::Type* type =
                    templateArgs->get(0).getAsType().getTypePtr();
                return {name, type};
              }) |
          ranges::to<std::unordered_map>();

      auto els =
          nameToClangTypeMap |
          ranges::views::transform(
              [&parser, &Context, &Sema](
                  auto& p) -> std::pair<std::string, type_graph::Type*> {
                return {p.first, &parser.parse(Context, Sema, *p.second)};
              });
      ctx.nameToReconstructTypeMap.insert(els.begin(), els.end());
    }

    // Not deduplicated here, deliberately: introspectImpl<T>(const T&)'s
    // parameter type and reconstructImpl<T>'s template argument are NOT
    // the same clang::Type* even for identical T - the parameter type is
    // still wrapped in a Reference type-graph node at this point (nothing
    // has stripped it yet), while the template argument never had one.
    // They only become comparable once transform()'s RemoveTopLevelPointer
    // pass has normalized both - see OIGenerator::generate(), which does
    // that comparison (and the corresponding rootTypes() cleanup) after
    // calling transform(). Every introspect root lands before the (at most
    // one) reconstruct root - recorded here as (linkageName, rootTypes()
    // index) pairs, since the index - not the Type* - is what stays valid
    // once transform() runs (see ConsumerContext's own comment on this).
    for (const auto& [name, type] : ctx.nameToTypeMap) {
      ctx.introspectRootIndices.emplace_back(name,
                                             ctx.typeGraph.rootTypes().size());
      ctx.typeGraph.addRoot(*type);
    }
    for (const auto& [name, type] : ctx.nameToReconstructTypeMap) {
      ctx.reconstructRootIndex = {name, ctx.typeGraph.rootTypes().size()};
      ctx.typeGraph.addRoot(*type);
    }
  }
};

void CreateTypeGraphAction::ExecuteAction() {
  clang::CompilerInstance& CI = getCompilerInstance();

  // Compile the output as position independent if any input is position
  // independent
  bool pic = CI.getCodeGenOpts().RelocationModel == llvm::Reloc::PIC_;
  ctx.pic = ctx.pic.value_or(false) || pic;

  if (!CI.hasSema())
    CI.createSema(clang::TU_Complete, nullptr);
  ctx.sema = &CI.getSema();

  clang::ASTFrontendAction::ExecuteAction();
}

std::unique_ptr<clang::ASTConsumer> CreateTypeGraphAction::CreateASTConsumer(
    [[maybe_unused]] clang::CompilerInstance& CI,
    [[maybe_unused]] clang::StringRef file) {
  return std::make_unique<CreateTypeGraphConsumer>(ctx);
}

}  // namespace
}  // namespace oi::detail
