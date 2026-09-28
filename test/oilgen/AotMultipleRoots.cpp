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
// Proves oilgen's support for generating introspection code for more than
// one oi::introspect<T>() root type in a single translation unit - see
// docs/object-capture-initial-thoughts.md. Two unrelated structs are each
// introspected in the same file; oilgen must discover both
// introspectImpl<T> specializations, emit shared OIInternal declarations
// for the whole (two-root) type graph exactly once, and emit two
// independent top-level entry points into the same generated object file.
//
#include <oi/oi.h>

#if !defined(OIL_AOT_COMPILATION)
#error "This file must be compiled with -DOIL_AOT_COMPILATION=1"
#endif

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

struct FirstStruct {
  std::uint32_t id;
  std::uint64_t count;
};

struct SecondStruct {
  std::string name;
  bool active;
};

namespace {

bool checkResult(const oi::IntrospectionResult& result,
                 const char* expectedFieldA,
                 const char* expectedFieldB) {
  bool sawFieldA = false;
  bool sawFieldB = false;

  for (const auto& element : result) {
    std::cout << "name=\"" << element.name
              << "\" static_size=" << element.static_size
              << " exclusive_size=" << element.exclusive_size << '\n';

    if (element.name == expectedFieldA) {
      sawFieldA = true;
    } else if (element.name == expectedFieldB) {
      sawFieldB = true;
    }
  }

  if (!sawFieldA || !sawFieldB) {
    std::cerr << "Expected to see fields: " << expectedFieldA << ", "
              << expectedFieldB << '\n'
              << "sawFieldA=" << std::boolalpha << sawFieldA
              << " sawFieldB=" << sawFieldB << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  try {
    FirstStruct first{
        .id = 42,
        .count = 123456789,
    };
    SecondStruct second{
        .name = "hello",
        .active = true,
    };

    const auto firstResult = oi::introspect(first);
    const auto secondResult = oi::introspect(second);

    if (!checkResult(firstResult, "id", "count")) {
      return EXIT_FAILURE;
    }
    if (!checkResult(secondResult, "name", "active")) {
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
