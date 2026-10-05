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

#include <glog/logging.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>

#include "oi/Features.h"
#include "oi/OICache.h"
#include "oi/OICodeGenConfig.h"
#include "oi/OICompiler.h"
#include "oi/OIParser.h"
#include "oi/SymbolService.h"
#include "oi/TrapInfo.h"
#include "oi/X86InstDefs.h"

namespace oi::detail {

// What oid does with the data it captures.
struct OidOutputConfig {
  FeatureSet features;
  // Dump each argument's raw data segment instead of decoding it.
  bool dumpDataSegment = false;
  std::optional<std::string> jsonPath;
};

class OIDebugger {
  OIDebugger(const OICodeGenConfig&, OICompiler::Config, OidOutputConfig);

 public:
  OIDebugger(pid_t,
             const OICodeGenConfig&,
             OICompiler::Config,
             OidOutputConfig);
  OIDebugger(std::filesystem::path,
             const OICodeGenConfig&,
             OICompiler::Config,
             OidOutputConfig);

  bool segmentInit(void);
  bool stopTarget(void);
  /*
   * Seize all the target's threads without stopping any: enough to write
   * the breakpoints (via /proc/<pid>/mem) and catch the threads that hit
   * them.
   */
  bool attachThreads(void);
  /*
   * Look up the global variables probed and write their addresses into the
   * prologue, before any thread is stopped.
   */
  bool prepareGlobals(void);
  /* Seize and stop only the main thread: for a global variable probe. */
  bool stopMainThread(void);
  /* Put a thread that is in the capture code back where it was trapped. */
  bool abandonCapture(pid_t);
  /*
   * Whether segmentInit() needs a stopped target thread: to map the
   * segments with remote syscalls, or set up JIT logging. Not when this
   * process's segments already exist, at the right size.
   */
  bool segmentInitNeedsThread(void) const;
  bool interruptTarget(void);
  bool compileCode();
  bool processTargetData();
  bool executeCode(pid_t);
  void setDataSegmentSize(size_t);
  void restoreState(void);
  bool segConfigExists(void) const {
    return segConfig.existingConfig;
  };
  enum oidMode { OID_MODE_THREAD, OID_MODE_FUNC };
  void setMode(oidMode newMode) {
    mode = newMode;
  };
  bool targetAttach(void);
  enum processTrapRet { OID_ERR, OID_CONT, OID_DONE };
  OIDebugger::processTrapRet processTrap(pid_t, bool = true, bool = true);
  bool contTargetThread(bool detach = true) const;
  bool isGlobalDataProbeEnabled(void) const;
  static uint64_t singlestepInst(pid_t, struct user_regs_struct&);
  static bool singleStepFunc(pid_t, uint64_t);
  bool parseScript(std::istream& script);
  bool patchFunctions();
  void stopAll();
  bool removeTraps(pid_t);
  bool removeTrap(pid_t, const trapInfo&);
  /*
   * Write the first byte of the instruction at addr through /proc/<pid>/mem,
   * which needs no thread of the target to be stopped.
   */
  bool writeTextByte(uintptr_t addr, uint8_t value);
  /*
   * Interrupt a running thread so that it can be detached, dealing with a
   * breakpoint it may have hit and not yet reported. Returns the signal to
   * detach it with, or nothing if it is gone.
   */
  std::optional<int> stopForDetach(pid_t);
  bool sigtrapPending(pid_t) const;
  /* Whether any of the target's own code still has a breakpoint of ours. */
  bool targetTrapsActive() const;
  void enableDrgn();
  bool unmapSegments(bool deleteSegConf = false);
  bool isInterrupted(void) const {
    return oidShouldExit;
  };

  void setCacheBasePath(std::filesystem::path basePath) {
    if (std::filesystem::exists(basePath.parent_path()) &&
        !std::filesystem::exists(basePath)) {
      // Create cachedir if parent directory exists
      // TODO if returning false here, throw an error
      std::filesystem::create_directory(basePath);
    }
    cache.basePath = std::move(basePath);
  }

  void setCacheRemoteEnabled(bool upload, bool download) {
    cache.enableUpload = upload;
    cache.enableDownload = download;
    cache.abortOnLoadFail = download && !upload;
  }

  bool validateCache() {
    if ((cache.enableUpload || cache.enableDownload) && !cache.isEnabled()) {
      LOG(ERROR) << "Cache download/upload option specified when cache is "
                    "disabled - aborting!";
      return false;
    }
    return true;
  }

  void setHardDisableDrgn(bool val) {
    symbols->setHardDisableDrgn(val);
  }

  bool uploadCache() {
    return std::all_of(
        std::begin(pdata), std::end(pdata), [this](const auto& req) {
          return std::all_of(
              std::begin(req.args),
              std::end(req.args),
              [this, &req](const auto& arg) {
                return cache.upload(irequest{req.type, req.func, arg});
              });
        });
  }
  bool downloadCache() {
    return std::all_of(
        std::begin(pdata), std::end(pdata), [this](const auto& req) {
          return std::all_of(
              std::begin(req.args),
              std::end(req.args),
              [this, &req](const auto& arg) {
                return cache.download(irequest{req.type, req.func, arg});
              });
        });
  };

  void setCustomCodeFile(std::filesystem::path newCCT) {
    customCodeFile = std::move(newCCT);
  }

 private:
  bool debug = false;
  pid_t traceePid{};
  uint64_t objectAddr{};
  oidMode mode{OID_MODE_THREAD};
  enum class SegType { text, data };
  enum class StatusType {
    sleep,
    traced,
    running,
    zombie,
    dead,
    diskSleep,
    stopped,
    other,
    bad
  };
  static OIDebugger::StatusType getTaskState(pid_t pid);
  static std::string taskStateToString(OIDebugger::StatusType);
  size_t dataSegSize{1 << 20};
  size_t textSegSize{(1 << 22) + (1 << 20)};
  std::vector<pid_t> threadList;
  ParseData pdata{};
  uint64_t replayInstsCurIdx{};
  bool oidShouldExit{false};
  uint64_t count{};
  bool sigIntHandlerActive{false};
  const int sizeofInt3 = 1;
  const int sizeofUd2 = 2;
  const int replayInstSize = 512;
  bool trapsRemoved{false};
  std::shared_ptr<SymbolService> symbols;
  /*
   * Every breakpoint address written into the target's code, including those
   * since removed: a thread that executed one just before its removal can
   * report it later, and must then be rewound onto the original instruction.
   */
  std::set<uintptr_t> insertedTrapAddrs;

  struct TargetMem {
    int fd = -1;
    ~TargetMem() {
      if (fd >= 0) {
        close(fd);
      }
    }
  } targetMem;
  OICache cache;

  /*
   * Map address of valid INT3 instruction to metadata for that interrupt.
   * It MUST be an ordered map (std::map) to handle overlapping traps.
   */
  std::map<uint64_t, std::shared_ptr<trapInfo>> activeTraps;
  std::unordered_map<pid_t, std::shared_ptr<trapInfo>> threadTrapState;
  std::unordered_map<uintptr_t, uintptr_t> replayInstMap;

  /*
   * oid's own copy of the JIT code, relocated into this process. The data
   * captured in the target is decoded with the tree builder instructions in
   * this copy: they include functions (processors) as well as data, so oid
   * needs the code too, not just the object file.
   */
  struct DecoderSegment {
    void* addr = nullptr;
    size_t size = 0;
    DecoderSegment() = default;
    DecoderSegment(const DecoderSegment&) = delete;
    DecoderSegment& operator=(const DecoderSegment&) = delete;
    ~DecoderSegment();
  };
  DecoderSegment decoderSeg;
  OICompiler::RelocResult::SymTable decoderSymbols;
  bool loadDecoder(OICompiler&,
                   const std::set<std::filesystem::path>&,
                   const std::unordered_map<std::string, uintptr_t>&);
  std::optional<std::string> rootTypeName(const irequest&);

  template <typename Sys, typename... Args>
  std::optional<typename Sys::RetType> remoteSyscall(Args...);
  bool setupLogFile(void);
  bool cleanupLogFile(void);

  using ObjectAddrMap =
      std::unordered_map<std::variant<std::shared_ptr<GlobalDesc>,
                                      std::shared_ptr<FuncDesc::TargetObject>>,
                         uintptr_t>;

  ObjectAddrMap remoteObjAddrs{};

  bool setupSegment(SegType);
  bool unmapSegment(SegType);
  bool writeTargetMemory(void*, void*, size_t) const;
  bool readTargetMemory(void*, void*, size_t) const;
  std::optional<std::pair<OIDebugger::ObjectAddrMap::key_type, uintptr_t>>
  locateJitCodeStart(const irequest&, const OICompiler::RelocResult::SymTable&);
  bool writePrologue(const prequest&, const OICompiler::RelocResult::SymTable&);
  bool readInstFromTarget(uintptr_t, uint8_t*, size_t);
  void createSegmentConfigFile(void);
  void deleteSegmentConfig(bool);
  std::optional<std::shared_ptr<trapInfo>> makeTrapInfo(const prequest&,
                                                        const trapType,
                                                        const uint64_t);
  bool functionPatch(const prequest&);
  bool canProcessTrapForThread(pid_t) const;
  bool replayTrappedInstr(const trapInfo&,
                          pid_t,
                          struct user_regs_struct&,
                          struct user_fpregs_struct&) const;
  bool locateObjectsAddresses(const trapInfo&, struct user_regs_struct&);

  /*
   * The tail of the data segment holds the target's readable mappings, for
   * the JIT code's pointer validation (validate-pointers): see
   * FuncGen::DefineBasicTypeHandlers' oi_pointer_valid for its layout.
   */
  size_t validRangesBytes() const;
  uintptr_t validRangesAddr() const {
    return segConfig.dataSegBase + dataSegSize - validRangesBytes();
  }
  bool writeValidRanges();
  processTrapRet processFuncTrap(const trapInfo&,
                                 pid_t,
                                 struct user_regs_struct&,
                                 struct user_fpregs_struct&);
  processTrapRet processJitCodeRet(const trapInfo&, pid_t);
  bool processGlobal(const std::string&);
  static void dumpRegs(const char*, pid_t, struct user_regs_struct*);
  std::optional<uintptr_t> nextReplayInstrAddr(const trapInfo&);
  static int getExtendedWaitEventType(int);
  static bool isExtendedWait(int);
  void dumpAlltaskStates(void);
  std::optional<std::vector<uintptr_t>> findRetLocs(FuncDesc&);
  bool contTargetThread(pid_t, unsigned long = 0) const;

  OICompiler::Config compilerConfig{};
  const OICodeGenConfig& generatorConfig;
  OidOutputConfig outputConfig{};
  std::optional<std::string> generateCode(const irequest&);

  std::fstream segmentConfigFile;
  std::filesystem::path segConfigFilePath;
  std::filesystem::path customCodeFile;

  struct {
    int traceeFd = -1;
    int debuggerFd = -1;
  } logFds;

  struct c {
    uintptr_t textSegBase{};
    size_t textSegSize{};
    uintptr_t constStart{};
    uintptr_t jitCodeStart{};
    uintptr_t replayInstBase{};
    bool existingConfig{false};
    uintptr_t dataSegBase{};
    size_t dataSegSize{};
    uintptr_t cookie{};
  } segConfig{};

  /*
   * The first 3 words of the data segment contain:
   *  1. The OID identifier a.k.a. "magic id", 01DE8 in hex
   *  2. A random value (cookie) to make sure that the data
   *      segment we are reading from was not populated in
   *      an older run.
   *  3. The size of the data segment as written by the JIT-ed
   *      code.
   */
  struct DataHeader {
    uintptr_t magicId;
    uintptr_t cookie;
    uintptr_t size;
    uintptr_t timeTakenNs;
    size_t pointersSize;
    size_t pointersCapacity;

    /*
     * Flexible Array Member are not standard in C++, but this is
     * exactly what we need for the `data` field. These pragmas
     * disable the pedantic warnings, so the compiler stops yelling at us.
     * We want the header to be the size of the fields above. This is
     * important for the `decodeTargetData` method, to give the right size
     * to `folly::ByteRange range(…)`.
     */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    uint8_t data[];
#pragma GCC diagnostic pop
  };

  bool checkDataHeader(const DataHeader&) const;

  static constexpr size_t prologueLength = 64;
  static constexpr size_t constLength = 64;
};

}  // namespace oi::detail
