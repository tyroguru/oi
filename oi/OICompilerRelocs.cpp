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
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Twine.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/Support/Memory.h>
#include <llvm/Support/raw_os_ostream.h>

#include "oi/OICompiler.h"

#if LLVM_VERSION_MAJOR <= 15
#include <llvm/ADT/Triple.h>
#else
#include <llvm/TargetParser/Triple.h>
#endif

#include <boost/range/combine.hpp>

#include "oi/Metrics.h"
#include "oi/SymbolService.h"

namespace oi::detail {

using namespace llvm;
using namespace llvm::object;

/*
 * Manage memory for the object files and handle symbol resolution.
 * The interface is defined by LLVM and we setup our hooks to
 * allocate memory and prepare the relocation.
 *
 * This class, and everything else in this file, needs a live SymbolService to
 * resolve symbols against a remote process. It is therefore kept out of
 * OICompiler.cpp so that consumers which never call `applyRelocs()` (e.g.
 * oilgen, which only ever calls `compile()`) don't have to link against
 * SymbolService/drgn.
 */
class OIMemoryManager : public RTDyldMemoryManager {
 private:
#if LLVM_VERSION_MAJOR <= 15
  using ReserveAllocationSpaceAlignTy = uint32_t;
#else
  using ReserveAllocationSpaceAlignTy = llvm::Align;
#endif

 public:
  struct Slab {
   private:
    /*
     * Allocate a slab of memory out of which we will allocate text and data.
     * One Slab correspond to one Object file.
     * The slab is divided in two segments in this order:
     * 1. The text segment, to host the executable instructions
     * 2. The data segment, to host the static variables
     * At the minute, we make no differentiation between RW/RO data segments.
     * We don't set the correct permissions on the pages allocated in the target
     * process. Adding that would require making lots of `mprotect(2)` syscalls
     * and introduce more latency.
     */
    static sys::MemoryBlock allocateBlock(size_t totalSize) {
      std::error_code errorCode;
      auto mem = sys::Memory::allocateMappedMemory(
          alignTo(totalSize + 256, 256),  // Extra to fit paddings added below
          nullptr,
          sys::Memory::MF_READ | sys::Memory::MF_WRITE,
          errorCode);

      /*
       * It looks like report_fatal_error() calls exit() by default. If it's
       * not possible to allocate memory then we need to fail anyway but do it
       * gracefully. Try installing an error handler and propogating the
       * failure upwards so we can shutdown cleanly.
       */
      if (errorCode) {
        report_fatal_error(llvm::Twine("Can't allocate enough memory: ") +
                           errorCode.message());
      }

      return mem;
    }

   public:
    Slab(size_t totalSize, size_t codeSize, size_t dataSize)
        : memBlock{allocateBlock(totalSize)},
          /*
           * Allow some extra space to allow for alignment needs of segments.
           * 128 bytes should be ample and well within our "slop" allocation.
           */
          textSegBase{(uintptr_t)memBlock.base()},
          textSegLimit{alignTo(textSegBase + codeSize, 128)},
          dataSegBase{textSegLimit},
          dataSegLimit{alignTo(dataSegBase + dataSize, 128)} {
      assert(dataSegLimit <=
             (uintptr_t)memBlock.base() + memBlock.allocatedSize());

      /* Fill the slab with NOP instructions */
      memset(memBlock.base(), nopInst, memBlock.allocatedSize());
    }

    ~Slab() {
      sys::Memory::releaseMappedMemory(memBlock);
    }

    sys::MemoryBlock memBlock;
    SmallVector<sys::MemoryBlock, 8> functionSections{};
    SmallVector<sys::MemoryBlock, 8> dataSections{};

    uintptr_t textSegBase = 0;
    const uintptr_t textSegLimit = 0;

    uintptr_t dataSegBase = 0;
    const uintptr_t dataSegLimit = 0;

    uint8_t* allocate(uintptr_t Size, unsigned Alignment, bool isCode) {
      auto* allocOffset = isCode ? &textSegBase : &dataSegBase;
      auto allocLimit = isCode ? textSegLimit : dataSegLimit;

      VLOG(1) << "allocateFromSlab " << (isCode ? "Code " : "Data ") << " Size "
              << Size << " allocOffset " << std::hex << *allocOffset
              << " allocLimit " << allocLimit;

      auto allocAddr = alignTo(*allocOffset, Alignment);
      auto newAllocOffset = allocAddr + Size;
      if (newAllocOffset > allocLimit) {
        LOG(ERROR) << "allocateFromSlab: " << (isCode ? "Code " : "Data ")
                   << "allocOffset= " << std::hex << *allocOffset
                   << " Size = " << Size << " allocLimit = " << allocLimit;

        /* See above comment about failure handling */
        report_fatal_error("Can't allocate enough memory from slab");
      }

      auto& sections = isCode ? functionSections : dataSections;
      sections.emplace_back((void*)allocAddr, Size);

      *allocOffset = newAllocOffset;

      VLOG(1) << "allocateFromSlab return: " << std::hex << allocAddr;
      return (uint8_t*)allocAddr;
    }
  };

  SmallVector<Slab, 4> Slabs{};
  OIMemoryManager(std::shared_ptr<SymbolService> ss,
                  const std::unordered_map<std::string, uintptr_t>& synths)
      : RTDyldMemoryManager{},
        symbols{std::move(ss)},
        syntheticSymbols{synths} {
  }

  /* Hook to make LLVM call `reserveAllocationSpace()` for each Object file */
  bool needsToReserveAllocationSpace(void) override {
    return true;
  }

  void reserveAllocationSpace(uintptr_t,
                              ReserveAllocationSpaceAlignTy,
                              uintptr_t,
                              ReserveAllocationSpaceAlignTy,
                              uintptr_t,
                              ReserveAllocationSpaceAlignTy) override;

  uint8_t* allocateCodeSection(uintptr_t,
                               unsigned,
                               unsigned,
                               StringRef) override;
  uint8_t* allocateDataSection(
      uintptr_t, unsigned, unsigned, StringRef, bool) override;

  /* Hook to set up proper memory permission. We don't handle that */
  bool finalizeMemory(std::string*) override {
    return false;
  }

  /* Hook to locate symbols in the remote process */
  JITSymbol findSymbol(const std::string&) override;

  /*
   * We don't use EH frames in this context, as we generate then copy to another
   * process, and enabling them causes issues with folly crashing on oid exit.
   */
  void registerEHFrames(uint8_t*, uint64_t, size_t) override {
  }
  void deregisterEHFrames() override {
  }

 private:
  std::shared_ptr<SymbolService> symbols;
  const std::unordered_map<std::string, uintptr_t>& syntheticSymbols;

  Slab& currentSlab() {
    assert(!Slabs.empty());
    return Slabs.back();
  }
};

void OIMemoryManager::reserveAllocationSpace(
    uintptr_t codeSize,
    ReserveAllocationSpaceAlignTy codeAlignIn,
    uintptr_t roDataSize,
    ReserveAllocationSpaceAlignTy roDataAlignIn,
    uintptr_t rwDataSize,
    ReserveAllocationSpaceAlignTy rwDataAlignIn) {
  llvm::Align codeAlign{codeAlignIn};
  llvm::Align roDataAlign{roDataAlignIn};
  llvm::Align rwDataAlign{rwDataAlignIn};

  /*
   * It looks like the sizes given to us already take into account the
   * alignment restrictions the different type of sections may have. Aligning
   * to the next 1KB boundary just for a bit of safety-slush (paranoia really).
   */
  uint64_t totalSz = alignTo((codeSize + roDataSize + rwDataSize), 1024);

  VLOG(1) << "reserveAllocationSpace: codesize " << codeSize << " codeAlign "
          << codeAlign.value() << " roDataSize " << roDataSize
          << " roDataAlign " << roDataAlign.value() << " rwDataSize "
          << rwDataSize << " rwDataAlign " << rwDataAlign.value()
          << " (Total Size: " << totalSz << ")";

  Slabs.emplace_back(totalSz, codeSize, roDataSize + rwDataSize + 128);

  const auto& currSlab = currentSlab();
  VLOG(1) << "reserveAllocationSpace: " << std::hex << "SlabBase "
          << currSlab.memBlock.base() << " textSegBaseAlloc "
          << currSlab.textSegBase << " textSegLimit " << currSlab.textSegLimit
          << " dataSegBaseAlloc " << currSlab.dataSegBase << " dataSegLimit "
          << currSlab.dataSegLimit;
}

uint8_t* OIMemoryManager::allocateCodeSection(
    uintptr_t size,
    unsigned alignment,
    [[maybe_unused]] unsigned sectionID,
    StringRef sectionName) {
  VLOG(1) << "allocateCodeSection(Size = " << size
          << ", Alignment = " << alignment
          << ", SectionName = " << sectionName.data() << ")";

  return currentSlab().allocate(size, alignment, true /* isCode */);
}

uint8_t* OIMemoryManager::allocateDataSection(
    uintptr_t size,
    unsigned alignment,
    [[maybe_unused]] unsigned sectionID,
    StringRef sectionName,
    [[maybe_unused]] bool isReadOnly) {
  VLOG(1) << "allocateDataSection(Size = " << size
          << ", Alignment = " << alignment
          << ", SectionName = " << sectionName.data() << ")";

  return currentSlab().allocate(size, alignment, false /* isCode */);
}

/*
 * This is called to locate external symbols when relocations are
 * resolved. We have to lookup the symbol in the remote process every time,
 * which sucks for performance. However, relocation can happen while the remote
 * process is running, so this code is out of the hot path.
 * We can't rely on LLVM to do this job because we are resolving symbols of a
 * remote process. LLVM only handles resolving symbols for the current process.
 */
JITSymbol OIMemoryManager::findSymbol(const std::string& name) {
  if (auto synth = syntheticSymbols.find(name);
      synth != end(syntheticSymbols)) {
    VLOG(1) << "findSymbol(" << name << ") = Synth " << std::hex
            << synth->second;
    return JITSymbol(synth->second, JITSymbolFlags::Exported);
  }

  if (auto sym = symbols->locateSymbol(name)) {
    VLOG(1) << "findSymbol(" << name << ") = " << std::hex << sym->addr;
    return JITSymbol(sym->addr, JITSymbolFlags::Exported);
  }

  if (name.compare(0, 37, "_ZN6apache6thrift18TStructDataStorage") == 0 &&
      name.compare(name.size() - 16, 16, "13isset_indexesE") == 0) {
    /*
     * Hack to make weak symbols work with MCJIT.
     *
     * MCJIT converts weak symbols into strong symbols, which means weak symbols
     * we define in the JIT code will not be overridden by strong symbols in the
     * remote process.
     *
     * Instead, if we want something to act as a weak symbol, we must not
     * provide a definition at all. Then MCJIT will always search for it in the
     * remote processes.
     *  - If a symbol is found in the remote process, it will be used as normal
     *  - If no symbol is found, we end up here. Return an address of "-1" to
     *    signal that the symbol was not resolved without raising an error.
     *
     * Before dereferencing the weak symbol in the JIT code, it should be
     * compared against nullptr (not "-1"!).
     *
     * Note that __attribute__((weak)) is still required on the "weak" symbol's
     * declaration. Otherwise the compiler may optimise away the null-checks.
     */
    VLOG(1) << "findSymbol(" << name << ") = -1";
    return JITSymbol(-1, JITSymbolFlags::Exported);
  }

  VLOG(1) << "findSymbol(" << name << ") = not found";
  return JITSymbol(nullptr);
}

/*
 * Disassembles the opcodes housed in the Slabs' code segments.
 */
static constexpr size_t kMaxInterFuncInstrPadding = 16;

static void debugDisAsm(
    const SmallVector<OIMemoryManager::Slab, 4>& Slabs,
    const OICompiler::RelocResult::RelocInfos& ObjectRelocInfos) {
  VLOG(1) << "\nDisassembled Code";

  /* Outer loop on each Object files that has been loaded */
  assert(Slabs.size() == ObjectRelocInfos.size());
  for (const auto& S : boost::combine(Slabs, ObjectRelocInfos)) {
    const auto& [ObjFile, ObjRelInfo] = std::tie(S.get<0>(), S.get<1>());

    /* Inner loop on each Function Section of a given Object file */
    for (const auto& textSec : ObjFile.functionSections) {
      const auto offset =
          (uintptr_t)textSec.base() - (uintptr_t)ObjFile.memBlock.base();
      const auto baseRelocAddress = ObjRelInfo.RelocAddr + offset;

      size_t instrCnt = 0;
      size_t byteCnt = 0;
      size_t consNop = 0;
      auto dg = OICompiler::Disassembler((uint8_t*)textSec.base(),
                                         textSec.allocatedSize());
      while (auto inst = dg()) {
        instrCnt++;
        byteCnt += inst->opcodes.size();

        /*
         * I currently don't know the size of the generated object code housed
         * in the slab. I don't want to display all the 'nop' instructions at
         * the end of that buffer but I do want to display the 'nops' that are
         * padding in between the generated instructions. The following kinda
         * sucks...
         */
        if (inst->opcodes.size() == 1 && inst->opcodes[0] == nopInst) {
          if (++consNop == kMaxInterFuncInstrPadding + 1) {
            /*
             * We're in the nop padding after all the generated instructions so
             * stop.
             */
            break;
          }
        } else {
          consNop = 0;
        }

        VLOG(1) << std::hex << inst->offset + baseRelocAddress << ": "
                << inst->disassembly.data();
      }

      VLOG(1) << "Number of Instructions: " << instrCnt
              << " Instruction bytes: " << byteCnt;
    }
  }
}

std::optional<OICompiler::RelocResult> OICompiler::applyRelocs(
    uintptr_t baseRelocAddress,
    const std::set<fs::path>& objectFiles,
    const std::unordered_map<std::string, uintptr_t>& syntheticSymbols) {
  metrics::Tracing relocationTracing("relocation");

  memMgr = {new OIMemoryManager(symbols, syntheticSymbols),
            [](OIMemoryManager* p) { delete p; }};
  RuntimeDyld dyld(*memMgr, *memMgr);

  /* Load all the object files into the MemoryManager */
  for (const auto& objPath : objectFiles) {
    VLOG(1) << "Loading object file " << objPath;
    auto objFile = ObjectFile::createObjectFile(objPath.c_str());
    if (!objFile) {
      raw_os_ostream(LOG(ERROR)) << "Failed to load object file " << objPath
                                 << ": " << objFile.takeError();
      return std::nullopt;
    }

    dyld.loadObject(*objFile->getBinary());
    if (dyld.hasError()) {
      LOG(ERROR) << "load object failed: " << dyld.getErrorString().data();
      return std::nullopt;
    }
  }

  RelocResult res;
  res.relocInfos.reserve(memMgr->Slabs.size());

  /* Provides mapping addresses to the MemoryManager */
  uintptr_t currentRelocAddress = baseRelocAddress;
  for (const auto& slab : memMgr->Slabs) {
    for (const auto& funcSection : slab.functionSections) {
      auto offset =
          (uintptr_t)funcSection.base() - (uintptr_t)slab.memBlock.base();
      dyld.mapSectionAddress(funcSection.base(), currentRelocAddress + offset);

      VLOG(1) << std::hex << "Relocated code " << funcSection.base() << " to "
              << currentRelocAddress + offset;
    }

    for (const auto& dataSection : slab.dataSections) {
      auto offset =
          (uintptr_t)dataSection.base() - (uintptr_t)slab.memBlock.base();
      dyld.mapSectionAddress(dataSection.base(), currentRelocAddress + offset);

      VLOG(1) << std::hex << "Relocated data " << dataSection.base() << " to "
              << currentRelocAddress + offset;
    }

    res.relocInfos.push_back(
        RelocResult::RelocInfo{(uintptr_t)slab.memBlock.base(),
                               currentRelocAddress,
                               slab.memBlock.allocatedSize()});
    currentRelocAddress =
        alignTo(currentRelocAddress + slab.memBlock.allocatedSize(), 128);
    res.newBaseRelocAddr = currentRelocAddress;
  }

  /* Apply relocation, record EH, etc. */
  dyld.finalizeWithMemoryManagerLocking();

  if (dyld.hasError()) {
    LOG(ERROR) << "relocation finalization failed: "
               << dyld.getErrorString().str();
    return std::nullopt;
  }

  /* Copy symbol table into `res` */
  auto symbolTable = dyld.getSymbolTable();
  res.symbols.reserve(symbolTable.size());
  for (const auto& [symName, sym] : symbolTable) {
    res.symbols.emplace(symName.str(), sym.getAddress());
  }

  relocationTracing.stop();

  if (VLOG_IS_ON(3)) {
    debugDisAsm(memMgr->Slabs, res.relocInfos);
  }

  return res;
}

}  // namespace oi::detail
