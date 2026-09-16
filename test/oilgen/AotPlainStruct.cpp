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
//
// This is the Ahead-of-Time (AOT) counterpart to
// test/oil/oil_plain_struct.cpp. It exercises the same PlainStruct through
// the same public API surface, but via `oi::introspect()` instead of
// `oi::setupAndIntrospect()`.
//
// Unlike the JIT path, AOT introspection needs a second tool - `oilgen` - to
// have already generated the actual implementation of introspectImpl<T> for
// every type this translation unit instantiates oi::introspect<T>() for.
// That's what makes this a *build-time* pipeline rather than a *runtime*
// one: see test/oilgen/CMakeLists.txt for how the two compilation passes
// (this file compiled normally, and oilgen parsing this same file to
// generate the missing symbol) are wired together and linked into one
// binary.
//
#include <oi/oi.h>

#if !defined(OIL_AOT_COMPILATION)
#error "This file must be compiled with -DOIL_AOT_COMPILATION=1"
#endif

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>

// Unlike the JIT path, a type introspected via oi::introspect<T>() must have
// external linkage: T is a template argument to the weak introspectImpl<T>
// symbol that oilgen fills in below, and a type declared in an anonymous
// namespace has no linkage at all, which Clang correctly rejects.
struct PlainStruct {
  std::uint32_t id;
  std::uint64_t count;
  bool enabled;
};

int main() {
  try {
    PlainStruct object{
        .id = 42,
        .count = 123456789,
        .enabled = true,
    };

    // No config/options to pass at runtime: everything oilgen needed to know
    // (container definitions, header search paths, ...) was supplied when it
    // generated this type's introspection code at build time.
    const auto result = oi::introspect(object);

    std::size_t elementCount = 0;
    bool sawId = false;
    bool sawCount = false;
    bool sawEnabled = false;

    for (const auto& element : result) {
      ++elementCount;
      std::cout << "name=\"" << element.name
                << "\" static_size=" << element.static_size
                << " exclusive_size=" << element.exclusive_size << '\n';

      if (element.name == "id") {
        sawId = true;
      } else if (element.name == "count") {
        sawCount = true;
      } else if (element.name == "enabled") {
        sawEnabled = true;
      }
    }

    std::cout << "element_count=" << elementCount << '\n';

    if (!sawId || !sawCount || !sawEnabled) {
      std::cerr << "Expected to see fields: id, count, enabled\n"
                << "sawId=" << std::boolalpha << sawId
                << " sawCount=" << sawCount << " sawEnabled=" << sawEnabled
                << '\n';
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
