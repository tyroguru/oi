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
#include "oi/OICompiler.h"

#include <clang/Basic/LangStandard.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Basic/TargetOptions.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Frontend/FrontendOptions.h>
#include <clang/Lex/HeaderSearchOptions.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <glog/logging.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/MC/TargetRegistry.h>
#if LLVM_VERSION_MAJOR >= 16
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif
#include <llvm/Support/TargetSelect.h>

#if LLVM_VERSION_MAJOR <= 15
#include <llvm/ADT/Triple.h>
#else
#include <llvm/TargetParser/Triple.h>
#endif

#include <array>
#include <boost/scope_exit.hpp>

#include "oi/Headers.h"
#include "oi/Metrics.h"

extern "C" {
#include <llvm-c/Disassembler.h>
}

namespace oi::detail {

using namespace std;
using namespace clang;
using namespace llvm;

static const char* symbolLookupCallback(
    [[maybe_unused]] void* disInfo,
    [[maybe_unused]] uint64_t referenceValue,
    uint64_t* referenceType,
    [[maybe_unused]] uint64_t referencePC,
    [[maybe_unused]] const char** referenceName) {
  *referenceType = LLVMDisassembler_ReferenceType_InOut_None;
  return nullptr;
}

/*
 * This structure's goal is to statically initialize parts of LLVM used by
 * Disassembler. We're declaring a static global variable with a constructor
 * doing the init calls once and for all, on our behalf. The destructor will
 * then take care of the cleanup, at exit.
 */
static LLVMDisasmContextRef disassemblerContext = nullptr;
static struct LLVMInitializer {
  LLVMInitializer() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetDisassembler();

    std::string triple = llvm::sys::getProcessTriple();
    disassemblerContext = LLVMCreateDisasm(
        triple.c_str(), nullptr, 0, nullptr, symbolLookupCallback);
    if (!disassemblerContext) {
      throw std::runtime_error("Failed to initialize disassemblerContext");
    }

    /*
     * Enable Intel assembly syntax and print immediate values in hexadecimal.
     * The order in which the options are set matters. Don't re-order!
     */
    LLVMSetDisasmOptions(disassemblerContext,
                         LLVMDisassembler_Option_AsmPrinterVariant);
    LLVMSetDisasmOptions(disassemblerContext,
                         LLVMDisassembler_Option_PrintImmHex);
  }

  ~LLVMInitializer() {
    LLVMDisasmDispose(disassemblerContext);
  }
} llvmInitializer;

std::optional<OICompiler::Disassembler::Instruction>
OICompiler::Disassembler::operator()() {
  if (disassemblerContext == nullptr || std::empty(funcText)) {
    return std::nullopt;
  }

  size_t instSize =
      LLVMDisasmInstruction(disassemblerContext,
                            const_cast<uint8_t*>(std::data(funcText)),
                            std::size(funcText),
                            0,
                            std::data(disassemblyBuffer),
                            std::size(disassemblyBuffer));
  if (instSize == 0) {
    return std::nullopt;
  }

  Instruction inst{
      offset,
      {std::data(funcText), instSize},
      {std::data(disassemblyBuffer)},
  };

  offset += instSize;
  funcText = funcText.subspan(instSize);

  return inst;
}

std::optional<std::string> OICompiler::decodeInst(
    const std::vector<std::byte>& funcText, uintptr_t offset) {
  auto disassembler = Disassembler((const uint8_t*)funcText.data() + offset,
                                   funcText.size() - offset);

  auto inst = disassembler();
  if (!inst) {
    return std::nullopt;
  }

  VLOG(1) << "Decoded instruction: " << inst->disassembly
          << " size: " << inst->opcodes.size();
  return std::string(inst->disassembly);
}

bool OICompiler::compile(const std::string& code,
                         const fs::path& sourcePath,
                         const fs::path& objectPath) {
  metrics::Tracing _("compile");

  /*
   * Note to whoever: if you're having problems compiling code, especially
   * header issues, then make sure you thoroughly read the options list in
   * include/clang/Basic/LangOptions.def.
   */
  auto compInv = std::make_shared<CompilerInvocation>();

  LangOptions& langOpts =
#if LLVM_VERSION_MAJOR < 18
      *compInv->getLangOpts();
#else
      compInv->getLangOpts();
#endif

  langOpts.CPlusPlus = true;
  langOpts.CPlusPlus11 = true;
  langOpts.CPlusPlus14 = true;
  langOpts.CPlusPlus17 = true;
  langOpts.CPlusPlus20 = true;
  // Required for various `__GCC_ATOMIC_*` macros to be defined
  langOpts.GNUCVersion = 11 * 100 * 100;  // 11.0.0
  langOpts.Bool = true;
  langOpts.WChar = true;
  langOpts.Char8 = true;
  langOpts.CXXOperatorNames = true;
  langOpts.DoubleSquareBracketAttributes = true;
  langOpts.Exceptions = true;
  langOpts.CXXExceptions = true;
  langOpts.Coroutines = true;
  langOpts.AlignedAllocation = true;

  compInv->getPreprocessorOpts();
  compInv->getPreprocessorOpts().addRemappedFile(
      sourcePath.string(), MemoryBuffer::getMemBufferCopy(code).release());
  compInv->getPreprocessorOpts().UsePredefines = true;

  compInv->getFrontendOpts().Inputs.push_back(
      FrontendInputFile(sourcePath.string(), InputKind{Language::CXX}));
  compInv->getFrontendOpts().OutputFile = objectPath.string();
  compInv->getFrontendOpts().ProgramAction = clang::frontend::EmitObj;

  auto& headerSearchOptions = compInv->getHeaderSearchOpts();

  for (const auto& path : config.userHeaderPaths) {
    headerSearchOptions.AddPath(
        path.c_str(),
        clang::frontend::IncludeDirGroup::IndexHeaderMap,
        false,
        false);
  }

  for (const auto& path : config.sysHeaderPaths) {
    headerSearchOptions.AddPath(
        path.c_str(), clang::frontend::IncludeDirGroup::System, false, false);
  }

  static const auto syntheticHeaders =
      std::array<std::pair<Feature, std::pair<std::string_view, std::string>>,
                 7>{{
          {Feature::TreeBuilderV2, {headers::oi_types_st_h, "oi/types/st.h"}},
          {Feature::TreeBuilderV2, {headers::oi_types_dy_h, "oi/types/dy.h"}},
          {Feature::TreeBuilderV2,
           {headers::oi_exporters_inst_h, "oi/exporters/inst.h"}},
          {Feature::TreeBuilderV2,
           {headers::oi_exporters_ParsedData_h, "oi/exporters/ParsedData.h"}},
          {Feature::TreeBuilderV2,
           {headers::oi_result_Element_h, "oi/result/Element.h"}},
          {Feature::Library,
           {headers::oi_IntrospectionResult_h, "oi/IntrospectionResult.h"}},
          {Feature::Library,
           {headers::oi_IntrospectionResult_inl_h,
            "oi/IntrospectionResult-inl.h"}},
      }};
  for (const auto& [k, v] : syntheticHeaders) {
    if (!config.features[k])
      continue;
    compInv->getPreprocessorOpts().addRemappedFile(
        std::string{"/synthetic/headers/"} + v.second,
        MemoryBuffer::getMemBuffer(v.first).release());
  }
  for (const auto& kv : syntheticHeaders) {
    const auto& k = kv.first;
    if (config.features[k]) {
      headerSearchOptions.AddPath(
          "/synthetic/headers",
          clang::frontend::IncludeDirGroup::IndexHeaderMap,
          false,
          false);
      break;
    }
  }

  compInv->getFrontendOpts().OutputFile = objectPath;
  compInv->getTargetOpts().Triple =
      llvm::Triple::normalize(llvm::sys::getProcessTriple());
  if (config.usePIC) {
    compInv->getCodeGenOpts().RelocationModel = llvm::Reloc::PIC_;
  } else {
    compInv->getCodeGenOpts().RelocationModel = llvm::Reloc::Static;
  }
  compInv->getCodeGenOpts().CodeModel = "large";
  compInv->getCodeGenOpts().OptimizationLevel = 3;
  compInv->getCodeGenOpts().NoUseJumpTables = 1;

  if (config.features[Feature::GenJitDebug]) {
    compInv->getCodeGenOpts().setDebugInfo(codegenoptions::FullDebugInfo);
  }
  compInv->getDiagnosticOpts().TemplateBacktraceLimit = 0;

  CompilerInstance compInstance;
  compInstance.setInvocation(compInv);
  compInstance.createDiagnostics();
  EmitObjAction compilerAction;

  bool execute = compInstance.ExecuteAction(compilerAction);

  if (!execute) {
    LOG(ERROR) << "Execute failed";
    return false;
  }

  /*  LLVM 12 seems to be unable to handle the large files we create,
      and consistently dies with the message:
      'fatal error: sorry, this include generates a translation unit too large
      for Clang to process.'
      So this is disabled for now.
  if (VLOG_IS_ON(2)) {
    // TODO: Maybe accept file path as an arg to dump the preprocessed file.
    // Dumping to /tmp seems to require root permission
    if (access("oi_preprocessed", F_OK) == 0 &&
        access("oi_preprocessed", R_OK | W_OK) != 0) {
      LOG(ERROR) << "Trying to write oi_preprocessed, "
                 << "but it cannot be overwritten. Either remove it or run "
                    "oid with root priviledges ";
    } else {
      compInv->getFrontendOpts().OutputFile = "oi_preprocessed";
      compInv->getLangOpts()->LineComment = 1;
      compInv->getPreprocessorOutputOpts().ShowCPP = 1;
      auto act = new PrintPreprocessedAction();
      CI.ExecuteAction(*act);
      VLOG(1) << "Dumped preprocessed output to file: "
              << compInv->getFrontendOpts().OutputFile;
    }
  }
  */

  return true;
}

}  // namespace oi::detail
