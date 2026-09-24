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
#include <boost/regex.hpp>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "oi/ContainerTypeEnum.h"
#include "oi/Features.h"

ContainerTypeEnum containerTypeEnumFromStr(std::string& str);
const char* containerTypeEnumToStr(ContainerTypeEnum ty);

struct ContainerInfo {
  struct Processor {
    std::string type;
    std::string func;
  };

  struct Codegen {
    std::string decl;
    std::string func;
    std::string traversalFunc = "";
    std::string extra = "";
    std::string scopedExtra = "";
    /*
     * Research groundwork for byte-accurate object capture/reconstruction
     * (see docs/object-capture-initial-thoughts.md, not part of this repo) -
     * a C++ function body assembling one instance of this container from
     * already-reconstructed content, the read-side counterpart to
     * traversalFunc. Empty (the default) means this container isn't
     * reconstructable yet - reconstructing a root of this type throws a
     * clear error rather than guessing. Always paired with reconstructKind
     * (below), which selects which names this body has in scope.
     *
     * %1% substitutes to the container's own bare name exactly as decl/func
     * already do, so `%1%<T0>` (or `%1%<T0, T1>` for a "map"-kind
     * container) names the concrete container type.
     */
    std::string reconstruct = "";
    /*
     * Which calling convention `reconstruct` (above) was written against -
     * required whenever `reconstruct` is set, since the wire shape (and so
     * what can be decoded ahead of time versus what `reconstruct` must
     * assemble itself) differs by container kind:
     *
     *   "list" - a homogeneous single-element container (sequences, sets).
     *   `length` (the decoded element count) and `nextElement()` (decodes
     *   and returns the next element, of type T0) are in scope.
     *
     *   "bytes" - a container whose entire reconstructable content is one
     *   contiguous captured byte blob (e.g. a string's characters).
     *   `contentBytes` (a std::vector<uint8_t> of the captured bytes) is in
     *   scope.
     *
     *   "map" - a homogeneous key/value container (e.g. std::map).
     *   `length` and `nextEntry()` (decodes and returns the next entry as
     *   a std::pair<T0, T1> - key first, value second) are in scope.
     *
     *   "pointer" - a single, possibly-absent owned value (std::unique_ptr
     *   today - std::shared_ptr/std::weak_ptr and raw pointers share the
     *   same underlying wire shape but aren't wired up to this calling
     *   convention yet, see docs/object-capture-initial-thoughts.md).
     *   `present` (a bool - whether the pointee was captured) and
     *   `pointeeVal()` (decodes and returns the pointee, of type T0 - must
     *   be called at most once, and only when `present` is true) are in
     *   scope. No aliasing/cycle support: assumes sole ownership of the
     *   pointee, true for unique_ptr by construction.
     */
    std::string reconstructKind = "";
    std::vector<Processor> processors{};
  };

  explicit ContainerInfo(const std::filesystem::path& path);  // Throws
  ContainerInfo(std::string typeName,
                ContainerTypeEnum ctype,
                std::string header);

  // Old ctors, remove with OICodeGen:
  ContainerInfo() = default;
  ContainerInfo(std::string typeName_,
                boost::regex matcher,
                std::optional<size_t> numTemplateParams_,
                ContainerTypeEnum ctype_,
                std::string header_,
                std::vector<std::string> ns_,
                std::vector<size_t> replaceTemplateParamIndex_,
                std::optional<size_t> allocatorIndex_,
                std::optional<size_t> underlyingContainerIndex_,
                std::vector<size_t> stubTemplateParams_,
                std::vector<size_t> completeTemplateParamIndexes_,
                oi::detail::FeatureSet requiredFeatures,
                ContainerInfo::Codegen codegen_)
      : typeName(std::move(typeName_)),
        numTemplateParams(numTemplateParams_),
        ctype(ctype_),
        header(std::move(header_)),
        ns(std::move(ns_)),
        replaceTemplateParamIndex(std::move(replaceTemplateParamIndex_)),
        allocatorIndex(allocatorIndex_),
        underlyingContainerIndex(underlyingContainerIndex_),
        stubTemplateParams(std::move(stubTemplateParams_)),
        completeTemplateParamIndexes(std::move(completeTemplateParamIndexes_)),
        requiredFeatures(requiredFeatures),
        codegen(std::move(codegen_)),
        matcher_(std::move(matcher)) {
  }

  ContainerInfo(ContainerInfo&&) = default;
  ContainerInfo& operator=(ContainerInfo&&) = default;

  // Explicit interface for copying
  ContainerInfo clone() const {
    ContainerInfo copy{*this};
    return copy;
  }

  bool matches(std::string_view sv) const {
    return boost::regex_search(sv.begin(), sv.end(), matcher_);
  }

  std::string typeName;
  std::optional<size_t> numTemplateParams;
  ContainerTypeEnum ctype = UNKNOWN_TYPE;
  std::string header;
  std::vector<std::string> ns;
  std::vector<size_t> replaceTemplateParamIndex{};
  std::optional<size_t> allocatorIndex{};
  // Index of underlying container in template parameters for a container
  // adapter
  std::optional<size_t> underlyingContainerIndex{};
  std::vector<size_t> stubTemplateParams{};
  // Template parameters that must be complete before generated code
  // instantiates this container type. Inline-storage containers need this
  // even when their ctype normally allows incomplete element types.
  std::vector<size_t> completeTemplateParamIndexes{};
  bool captureKeys = false;
  oi::detail::FeatureSet requiredFeatures;

  Codegen codegen;

  static std::unique_ptr<ContainerInfo> loadFromFile(
      const std::filesystem::path& path);

  bool operator<(const ContainerInfo& rhs) const {
    return (typeName < rhs.typeName);
  }

 private:
  ContainerInfo(const ContainerInfo&) = default;
  ContainerInfo& operator=(const ContainerInfo& other) = default;

  boost::regex matcher_;
};

class ContainerInfoError : public std::runtime_error {
 public:
  ContainerInfoError(const std::filesystem::path& path, const std::string& msg)
      : std::runtime_error{std::string{path} + ": " + msg} {
  }
};

using ContainerInfoRefSet =
    std::set<std::reference_wrapper<const ContainerInfo>,
             std::less<ContainerInfo>>;
