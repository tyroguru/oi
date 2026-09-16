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
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "oi/ContainerInfo.h"
#include "oi/Features.h"

namespace oi::detail {

// Configuration for both `OICodeGen` (the legacy, drgn-based code generator)
// and `CodeGen`/`ClangTypeParser` (the type-graph based code generator).
// Deliberately kept free of any drgn dependency so that consumers which only
// need to generate code from Clang ASTs (e.g. oilgen) don't have to pull in
// drgn just to name this type.
struct OICodeGenConfig {
  OICodeGenConfig() = default;
  OICodeGenConfig(const OICodeGenConfig& other) = delete;
  OICodeGenConfig& operator=(const OICodeGenConfig& other) = delete;
  OICodeGenConfig(OICodeGenConfig&& other) = delete;
  OICodeGenConfig& operator=(OICodeGenConfig&& other) = delete;

  struct KeyToCapture {
    std::optional<std::string> type;
    std::optional<std::string> member;
    bool topLevel = false;
  };

  FeatureSet features;
  std::set<std::filesystem::path> containerConfigPaths;
  std::set<std::string> defaultHeaders;
  std::set<std::string> defaultNamespaces;
  std::vector<std::string> preprocessorDefines;
  std::vector<std::pair<std::string, std::string>> membersToStub;
  std::vector<ContainerInfo> passThroughTypes;
  std::vector<KeyToCapture> keysToCapture;

  std::string toString() const;
  std::vector<std::string> toOptions() const;
};

}  // namespace oi::detail
