/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "oi/Timeline.h"

#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace oi::detail::timeline {
namespace {

struct Entry {
  uint64_t start;
  uint64_t end;
  const char* what;
  long arg;
  pid_t pid;
  long result;
};

/*
 * Written out by the destructor, at exit, so that nothing is written while
 * oid has the target stopped. oid is single threaded, so no locking.
 */
struct Timeline {
  const char* path = std::getenv(envKey);
  std::vector<Entry> entries;

  Timeline() {
    if (path != nullptr) {
      entries.reserve(1 << 16);
    }
  }

  ~Timeline() {
    if (path == nullptr) {
      return;
    }
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
      std::perror(path);
      return;
    }
    std::fprintf(f, "start_ns,end_ns,what,arg,pid,result\n");
    for (const auto& e : entries) {
      std::fprintf(f,
                   "%lu,%lu,%s,%ld,%d,%ld\n",
                   e.start,
                   e.end,
                   e.what,
                   e.arg,
                   e.pid,
                   e.result);
    }
    std::fclose(f);
  }
};

Timeline& get() {
  static Timeline timeline;
  return timeline;
}

}  // namespace

bool enabled() {
  return get().path != nullptr;
}

uint64_t now() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000 +
         static_cast<uint64_t>(ts.tv_nsec);
}

void record(const char* what,
            long arg,
            pid_t pid,
            uint64_t start,
            uint64_t end,
            long result) {
  get().entries.push_back({start, end, what, arg, pid, result});
}

}  // namespace oi::detail::timeline
