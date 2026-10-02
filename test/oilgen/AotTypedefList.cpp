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
// AOT capture of a C-style intrusive singly linked list (the shape of e.g.
// nginx's thread pool task queue), with chase-raw-pointers and capture-bytes
// on. Regression test for three bugs that each broke this shape:
//
//  1. ClangTypeParser recursed into function-pointer pointees when chasing
//     and threw "unsupported TypeClass `Paren`" (`handler` below).
//  2. BreakCycles didn't look through Typedefs, so `task_t* next` (pointee
//     is the Typedef, not the on-path struct) was left unbroken and
//     DetectCycles rejected the type.
//  3. OICycleBreaker's TypeHandler had no processor for its capture-bytes
//     DynBytes payload, so iterating the IntrospectionResult misaligned on
//     the first non-null `next` and every later element read garbage.
//
// `ctx` (a void*) is stubbed via AotTypedefList.toml: reconstructing a
// chased void* doesn't compile yet.
//
#include <oi/oi.h>

#if !defined(OIL_AOT_COMPILATION)
#error "This file must be compiled with -DOIL_AOT_COMPILATION=1"
#endif

#include <cstdint>
#include <cstring>
#include <iostream>
#include <new>
#include <string>

extern "C" {
typedef struct task_s task_t;
struct task_s {
  task_t* next;
  uint64_t id;
  void* ctx;
  void (*handler)(void* data, int level);
};

typedef struct {
  task_t* first;
  int32_t waiting;
} queue_t;
}

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << '\n';
    failures++;
  }
}

std::string path(const oi::result::Element& el) {
  std::string p;
  for (size_t i = 1; i < el.type_path.size(); i++) {
    p += (p.empty() ? "" : ".") + std::string(el.type_path[i]);
  }
  return p;
}

void runCase(int n) {
  const std::string tag = "n=" + std::to_string(n) + ": ";

  queue_t q{nullptr, 42};
  task_t** last = &q.first;
  for (int i = 0; i < n; i++) {
    *last =
        new task_t{nullptr, static_cast<uint64_t>(100 + i), nullptr, nullptr};
    last = &(*last)->next;
  }

  const auto result = oi::introspect(q);

  // Bug 3: the element after the list must still decode correctly.
  bool sawWaiting = false;
  for (const auto& el : result) {
    if (path(el) != "waiting") {
      continue;
    }
    sawWaiting = true;
    const auto* b = std::get_if<oi::result::Element::Bytes>(&el.data);
    int32_t v = 0;
    if (b != nullptr && b->value.size() == sizeof(v)) {
      std::memcpy(&v, b->value.data(), sizeof(v));
    }
    check(v == 42, tag + "waiting decoded as " + std::to_string(v));
  }
  check(sawWaiting, tag + "no 'waiting' element");

  // The whole chain is in the captured bytes: reconstruct it.
  queue_t rc = oi::reconstruct<queue_t>(result.rawBytes());
  check(rc.waiting == 42, tag + "reconstructed waiting");
  int len = 0;
  for (task_t* t = rc.first; t != nullptr; len++) {
    check(t->id == static_cast<uint64_t>(100 + len),
          tag + "reconstructed id at " + std::to_string(len));
    task_t* next = t->next;
    ::operator delete(t);
    t = next;
  }
  check(len == n, tag + "reconstructed length " + std::to_string(len));

  for (task_t* t = q.first; t != nullptr;) {
    task_t* next = t->next;
    delete t;
    t = next;
  }
}

}  // namespace

int main() {
  try {
    for (int n : {0, 1, 2, 3, 64, 255}) {
      runCase(n);
    }
  } catch (const std::exception& e) {
    std::cerr << "FAIL: exception: " << e.what() << '\n';
    return 1;
  }

  if (failures != 0) {
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}
