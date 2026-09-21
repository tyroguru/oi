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
#ifndef INCLUDED_OI_OI_H
#define INCLUDED_OI_OI_H 1

#include <oi/IntrospectionResult.h>
#include <oi/types/dy.h>

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace oi {

enum class Feature {
  ChaseRawPointers,
  CaptureThriftIsset,
  GenJitDebug,
};

#ifdef OIL_AOT_COMPILATION

template <class T, Feature... Fs>
IntrospectionResult __attribute__((weak)) introspectImpl(const T& objectAddr);

template <typename T, Feature... Fs>
__attribute__((noinline)) IntrospectionResult introspect(const T& objectAddr) {
  if (!introspectImpl<T, Fs...>)
    throw std::logic_error(
        "OIL is expecting AoT compilation but it doesn't appear to have run.");

  return introspectImpl<T, Fs...>(objectAddr);
}

template <typename T, Feature... Fs>
std::optional<IntrospectionResult> tryIntrospect(const T& objectAddr) {
  if (!introspectImpl<T, Fs...>)
    return std::nullopt;

  // This checks twice but is necessary for compile time as it currently
  // depends on the presence of the strong symbol.
  return introspect(objectAddr);
}

/*
 * Research groundwork for byte-accurate object capture (see
 * docs/object-capture-initial-thoughts.md, not part of this repo) -
 * reconstruct<T> is the read-side counterpart to introspect<T>: given the
 * raw bytes a capture-enabled introspect<T>() produced, build a live T.
 * Symmetric to introspectImpl above in every way that matters: a weak
 * template with no definition here, given a strong definition by oilgen
 * (when it also finds a reconstruct<T>() call site) under the identical
 * mangled name, resolved by ordinary linker symbol resolution. Deliberately
 * takes raw bytes rather than an IntrospectionResult - nothing here should
 * assume the bytes came from a still-in-scope capture in this same
 * process, since that assumption would need undoing the moment capture and
 * reconstruction stop happening in the same process.
 */
template <class T>
T __attribute__((weak)) reconstructImpl(std::span<const uint8_t> bytes);

template <typename T>
T reconstruct(std::span<const uint8_t> bytes) {
  if (!reconstructImpl<T>)
    throw std::logic_error(
        "OIL is expecting AoT compilation but it doesn't appear to have run.");

  return reconstructImpl<T>(bytes);
}

#endif

}  // namespace oi

#ifndef OIL_AOT_COMPILATION
#include "oi/oi-jit.h"
#endif

#endif
