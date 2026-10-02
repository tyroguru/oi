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
// Capture cost of a self-referential list (a cycle-broken `next` edge) must
// be linear in its length, with capture-bytes and chase-raw-pointers on.
// Cycle-broken edges used to capture each tail into a private, length-
// prefixed buffer and copy it into the parent's, which is quadratic:
// 2048 nodes took ~23ms versus ~40us now.
//
// Compares best-of-N capture time at kSmall and kLarge (16x) nodes. Linear
// is ~16x; quadratic would be ~256x. Fails above 64x, leaving a wide margin
// for timing noise.
//
#include <oi/oi.h>

#if !defined(OIL_AOT_COMPILATION)
#error "This file must be compiled with -DOIL_AOT_COMPILATION=1"
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

struct Node {
  Node* next;
  uint64_t id;
};

namespace {

constexpr int kSmall = 256;
constexpr int kLarge = kSmall * 16;
constexpr double kMaxRatio = 64.0;

double bestCaptureMicros(Node* head, size_t* bytes) {
  double best = 1e18;
  for (int r = 0; r < 15; r++) {
    auto t0 = std::chrono::steady_clock::now();
    auto result = oi::introspect(*head);
    auto t1 = std::chrono::steady_clock::now();
    *bytes = result.rawBytes().size();
    best = std::min(best,
                    std::chrono::duration<double, std::micro>(t1 - t0).count());
  }
  return best;
}

}  // namespace

int main() {
  std::vector<Node> nodes(kLarge);
  for (int i = 0; i < kLarge; i++) {
    nodes[i] = Node{i + 1 < kLarge ? &nodes[i + 1] : nullptr,
                    static_cast<uint64_t>(i)};
  }

  try {
    size_t smallBytes = 0;
    size_t largeBytes = 0;

    nodes[kSmall - 1].next = nullptr;
    double small = bestCaptureMicros(&nodes[0], &smallBytes);
    nodes[kSmall - 1].next = &nodes[kSmall];
    double large = bestCaptureMicros(&nodes[0], &largeBytes);

    double ratio = large / std::max(small, 0.001);
    std::cout << kSmall << " nodes: " << small << "us, " << smallBytes
              << " bytes; " << kLarge << " nodes: " << large << "us, "
              << largeBytes << " bytes; ratio " << ratio << '\n';

    if (largeBytes > smallBytes * 17) {
      std::cerr << "FAIL: captured bytes grew superlinearly\n";
      return 1;
    }
    if (ratio > kMaxRatio) {
      std::cerr << "FAIL: capture time ratio " << ratio << " > " << kMaxRatio
                << " (quadratic?)\n";
      return 1;
    }
  } catch (const std::exception& e) {
    std::cerr << "FAIL: exception: " << e.what() << '\n';
    return 1;
  }

  std::cout << "ok\n";
  return 0;
}
