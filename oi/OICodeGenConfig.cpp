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
#include "oi/OICodeGenConfig.h"

#include <boost/algorithm/string/join.hpp>

namespace oi::detail {

std::string OICodeGenConfig::toString() const {
  using namespace std::string_literals;

  // The list of ignored members must also be part of the remote hash
  std::string ignoreMembers = "IgnoreMembers=";
  for (const auto& ignore : membersToStub) {
    ignoreMembers += ignore.first;
    ignoreMembers += "::";
    ignoreMembers += ignore.second;
    ignoreMembers += ';';
  }

  std::string defines = "PreprocessorDefines=";
  for (const auto& define : preprocessorDefines) {
    defines += define;
    defines += ';';
  }

  return boost::algorithm::join(toOptions(), ",") + "," + ignoreMembers + "," +
         defines;
}

std::vector<std::string> OICodeGenConfig::toOptions() const {
  std::vector<std::string> options;
  options.reserve(allFeatures.size());

  for (const auto f : allFeatures) {
    if (features[f]) {
      options.emplace_back(std::string("-f") + featureToStr(f));
    } else {
      options.emplace_back(std::string("-F") + featureToStr(f));
    }
  }

  return options;
}

}  // namespace oi::detail
