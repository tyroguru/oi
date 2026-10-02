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
// It also checks that the chain is visible to plain Element iteration
// (every node's `id`, once each): cycle-broken edges are captured inline
// rather than as an opaque blob. The circular case checks that a genuine
// cycle still terminates (address dedup) for both iteration and
// reconstruction. See AotListCaptureScaling.cpp for the cost side.
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
#include <set>
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

template <typename T>
bool decode(const oi::result::Element& el, T* out) {
  const auto* b = std::get_if<oi::result::Element::Bytes>(&el.data);
  if (b == nullptr || b->value.size() != sizeof(T)) {
    return false;
  }
  std::memcpy(out, b->value.data(), sizeof(T));
  return true;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void runCase(int n, bool circular) {
  const std::string tag =
      "n=" + std::to_string(n) + (circular ? " circular" : "") + ": ";

  queue_t q{nullptr, 42};
  task_t** last = &q.first;
  for (int i = 0; i < n; i++) {
    *last =
        new task_t{nullptr, static_cast<uint64_t>(100 + i), nullptr, nullptr};
    last = &(*last)->next;
  }
  if (circular && n > 0) {
    *last = q.first;
  }

  const auto result = oi::introspect(q);

  bool sawWaiting = false;
  std::multiset<uint64_t> ids;
  for (const auto& el : result) {
    const std::string p = path(el);
    if (p == "waiting") {
      // Bug 3: the element after the list must still decode correctly.
      sawWaiting = true;
      int32_t v = 0;
      decode(el, &v);
      check(v == 42, tag + "waiting decoded as " + std::to_string(v));
    } else if (p == "first.*.id" || endsWith(p, ".next.*.id")) {
      uint64_t id = 0;
      check(decode(el, &id), tag + "undecodable id at " + p);
      ids.insert(id);
    }
  }
  check(sawWaiting, tag + "no 'waiting' element");

  // Every node is visible to iteration, exactly once.
  check(ids.size() == static_cast<size_t>(n),
        tag + "iteration saw " + std::to_string(ids.size()) + " ids");
  for (int i = 0; i < n; i++) {
    check(ids.count(100 + i) == 1,
          tag + "iteration id " + std::to_string(100 + i));
  }

  // The whole chain is in the captured bytes: reconstruct it.
  queue_t rc = oi::reconstruct<queue_t>(result.rawBytes());
  check(rc.waiting == 42, tag + "reconstructed waiting");
  int len = 0;
  for (task_t* t = rc.first; t != nullptr && len < n; len++) {
    check(t->id == static_cast<uint64_t>(100 + len),
          tag + "reconstructed id at " + std::to_string(len));
    t = t->next;
  }
  check(len == n, tag + "reconstructed length " + std::to_string(len));
  if (circular && n > 0) {
    // The cycle must be rebuilt as a cycle (aliasing, not a copy).
    task_t* t = rc.first;
    for (int i = 0; i < n; i++) {
      t = t->next;
    }
    check(t == rc.first, tag + "reconstructed list isn't circular");
  } else {
    check(len == 0 || rc.first != nullptr, tag + "reconstructed tail");
  }

  for (auto* list : {rc.first, q.first}) {
    task_t* t = list;
    for (int i = 0; i < n; i++) {
      task_t* next = t->next;
      ::operator delete(t);
      t = next;
    }
  }
}

}  // namespace

int main() {
  try {
    for (int n : {0, 1, 2, 3, 64, 255}) {
      runCase(n, false);
      runCase(n, true);
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
