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
// A pointer that another thread changes while it is being captured.
//
// oid captures a live process without taking its locks, so a pointer can
// change between the capture code reading it and following it. The capture
// code used to read a pointer field several times (to record it, to check it
// for null, and again to follow it): if it changed to null in between, the
// capture dereferenced null inside the target. It now takes one snapshot.
//
// Here a second thread flips Holder::node between a Node and nullptr as fast
// as it can while the main thread introspects the Holder for a few seconds.
// Each capture must be consistent: the pointer it records is either null and
// not followed, or non-null and followed. Before the fix this test failed
// within a few thousand captures: a capture recorded the pointer as null and
// then followed it (or, with the reads the other way round, dereferenced
// null).
//
#include <oi/oi.h>

#if !defined(OIL_AOT_COMPILATION)
#error "This file must be compiled with -DOIL_AOT_COMPILATION=1"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>

struct Node {
  std::uint64_t value;
};

struct Holder {
  Node* node;
};

int main() {
  try {
    Node target{.value = 42};
    Holder holder{.node = &target};
    std::atomic<bool> stop{false};

    // The racing writer: deliberately a plain (racy) field, as in a target
    // process that oid captures without its locks; relaxed atomic stores
    // keep the writer's own code from being optimised away.
    std::thread writer([&] {
      bool on = false;
      while (!stop.load(std::memory_order_relaxed)) {
        __atomic_store_n(
            &holder.node, on ? &target : nullptr, __ATOMIC_RELAXED);
        on = !on;
      }
    });
    // Stop and join the writer on every way out of this scope.
    struct Joiner {
      std::atomic<bool>& stop;
      std::thread& thread;
      ~Joiner() {
        stop.store(true, std::memory_order_relaxed);
        thread.join();
      }
    } joiner{stop, writer};

    std::size_t captures = 0, followed = 0, null = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto result = oi::introspect(holder);
      // Elements: holder, holder.node, then holder.node's pointee (and its
      // value) only if the pointer was followed.
      bool sawPointer = false, pointerNull = false, sawPointee = false;
      for (const auto& el : result) {
        if (el.name == "node") {
          sawPointer = true;
          pointerNull = !el.pointer.has_value() || *el.pointer == 0;
        } else if (el.name == "*") {
          sawPointee = true;
        }
      }
      if (!sawPointer) {
        std::cerr << "capture " << captures << ": no pointer element\n";
        return EXIT_FAILURE;
      }
      if (pointerNull && sawPointee) {
        std::cerr << "capture " << captures
                  << ": followed a pointer it recorded as null\n";
        return EXIT_FAILURE;
      }
      ++captures;
      (sawPointee ? followed : null)++;
    }

    std::cout << captures << " captures: " << followed << " followed, " << null
              << " null\n";
    if (followed == 0 || null == 0) {
      std::cerr << "the writer never raced the captures\n";
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
