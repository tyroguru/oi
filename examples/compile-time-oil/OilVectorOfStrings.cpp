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
// A minimal example of OIL's Ahead-of-Time (AOT) introspection path. See the
// README in this directory for how the two-pass build (this file, then
// `oilgen` parsing this same file) fits together, and why it's needed.
//
#include <oi/oi.h>

#include <iostream>
#include <string>
#include <vector>

// The type being introspected must have external linkage: it's used as a
// template argument to a weak symbol that oilgen defines in a separate
// object file, so it can't live in an anonymous namespace or be a local
// class.
struct Foo {
  std::vector<std::string> strings;
};

int main() {
  Foo foo;
  foo.strings.push_back("Lorem ipsum dolor");
  foo.strings.push_back("sit amet,");
  foo.strings.push_back("consectetur adipiscing elit,");

  // oi::introspect<T>() throws if introspectImpl<T> was never generated -
  // i.e. if the oilgen pass was skipped, or ran over a source file that
  // doesn't instantiate oi::introspect<Foo>() the same way this one does.
  const auto result = oi::introspect(foo);

  std::size_t count = 0;
  for (const auto& element : result) {
    std::cout << element.name << " static=" << element.static_size
              << " exclusive=" << element.exclusive_size << '\n';
    ++count;
  }

  std::cout << "elements=" << count << '\n';
  return 0;
}
