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

#include <array>
#include <filesystem>
#include <memory>
#include <optional>

#include "oi/OICodeGenConfig.h"
#include "oi/OIParser.h"
#include "oi/SymbolService.h"

namespace oi::detail {

class OICache {
 public:
  OICache(const OICodeGenConfig& generatorConfig)
      : generatorConfig(generatorConfig) {
  }

  std::filesystem::path basePath{};
  std::shared_ptr<SymbolService> symbols{};
  bool downloadedRemote = false;
  bool enableUpload = false;
  bool enableDownload = false;
  bool abortOnLoadFail = false;

  // We need the generator config to download the cache
  // with the matching configuration.
  const OICodeGenConfig& generatorConfig;

  // Entity is used to index the `extensions` array
  // So we must keep the Entity enum and `extensions` array in sync!
  //
  // Only generated code is cached. Function and global descriptors used to be
  // too, but they hold drgn types, which can't be serialized; they're
  // recomputed from the debug info instead.
  enum class Entity { Source, Object, MAX };
  static constexpr std::array<const char*, static_cast<size_t>(Entity::MAX)>
      extensions{".cc", ".o"};

  bool isEnabled() const {
    return !basePath.empty();
  }
  std::optional<std::filesystem::path> getPath(const irequest&, Entity) const;

  bool upload(const irequest& req);
  bool download(const irequest& req);

 private:
  std::string generateRemoteHash(const irequest&);
};

}  // namespace oi::detail
