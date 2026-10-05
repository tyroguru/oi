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
#pragma once

#include <sys/types.h>

#include <cstdint>

/*
 * A timeline of what oid does to the target: when each ptrace(2), waitpid(2)
 * and process_vm_{read,write}v(2) call starts and ends, and named phases.
 * Its purpose is to break down the time oid stops the target's threads, so
 * recording costs two clock reads and a store per call, and nothing is
 * written until oid exits. Timestamps are CLOCK_MONOTONIC, as perf's
 * `-k CLOCK_MONOTONIC`, so the timeline can be aligned with the target's
 * context switches.
 *
 * Enabled by setting OID_TIMELINE to the path of the CSV file to write.
 */
namespace oi::detail::timeline {

constexpr auto envKey = "OID_TIMELINE";

bool enabled();
uint64_t now();

/*
 * One call: `what` names it ("ptrace", "waitpid", ...) and must be a string
 * literal; `arg` is the ptrace request or the waitpid options.
 */
void record(const char* what,
            long arg,
            pid_t pid,
            uint64_t start,
            uint64_t end,
            long result);

/* A phase of oid's run starting now. `what` must be a string literal. */
inline void mark(const char* what) {
  if (enabled()) {
    auto t = now();
    record(what, 0, 0, t, t, 0);
  }
}

}  // namespace oi::detail::timeline
