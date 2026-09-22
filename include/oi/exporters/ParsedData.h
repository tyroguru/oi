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
#ifndef INCLUDED_OI_EXPORTERS_PARSED_DATA_H
#define INCLUDED_OI_EXPORTERS_PARSED_DATA_H 1

#include <oi/types/dy.h>

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <variant>
#include <vector>

namespace oi::exporters {

struct ParsedData {
  class Lazy {
   public:
    Lazy(std::vector<uint8_t>::const_iterator& it, types::dy::Dynamic ty)
        : it_(it), ty_(ty) {
    }

    ParsedData operator()() {
      return ParsedData::parse(it_, ty_);
    }

   private:
    std::vector<uint8_t>::const_iterator& it_;
    types::dy::Dynamic ty_;
  };

  struct Unit {};
  struct VarInt {
    uint64_t value;
  };
  struct Bytes {
    std::vector<uint8_t> value;
  };
  struct DynBytes {
    std::vector<uint8_t> value;
  };
  struct Pair {
    Lazy first;
    Lazy second;
  };
  struct List {
    uint64_t length;
    Lazy values;
  };
  struct Sum {
    uint64_t index;
    Lazy value;
  };

  static ParsedData parse(std::vector<uint8_t>::const_iterator& it,
                          types::dy::Dynamic ty);

  ParsedData(Unit&& val_) : val(val_) {
  }
  ParsedData(VarInt&& val_) : val(val_) {
  }
  ParsedData(Bytes&& val_) : val(std::move(val_)) {
  }
  ParsedData(DynBytes&& val_) : val(std::move(val_)) {
  }
  ParsedData(Pair&& val_) : val(val_) {
  }
  ParsedData(List&& val_) : val(val_) {
  }
  ParsedData(Sum&& val_) : val(val_) {
  }

  std::variant<Unit, VarInt, Bytes, DynBytes, Pair, List, Sum> val;
};

/*
 * Reconstructs the exact scalar value a leaf's captured Bytes payload
 * represents - the inverse of the capture side's
 * std::bit_cast<std::array<uint8_t, sizeof(T)>>(t) (see
 * FuncGen::DefineBasicTypeHandlers). T must be the same trivially-copyable
 * type that was captured; like every other part of this system, there is
 * no way to identify T from the stream itself - the caller is expected to
 * already know it (see docs/object-capture-initial-thoughts.md's "do we
 * have enough information to identify container types" discussion).
 */
template <typename T>
T reconstructScalar(const ParsedData::Bytes& bytes) {
  assert(bytes.value.size() == sizeof(T));
  std::array<uint8_t, sizeof(T)> arr;
  std::copy(bytes.value.begin(), bytes.value.end(), arr.begin());
  return std::bit_cast<T>(arr);
}

/*
 * Fully consumes a ParsedData value's underlying bytes, however deeply
 * nested, without extracting anything from it - for skipping a field
 * reconstruction has no use for (e.g. a container's profiler-only
 * bookkeeping processors - va-intervals, pointer, capacity - ahead of its
 * actual content; see CodeGen::generateReconstructContainerBody).
 *
 * This is needed, and not a no-op, because Pair/List/Sum are Lazy at their
 * *immediate* children only: obtaining a ParsedData::Pair (say) does not by
 * itself consume the bytes either of its own fields represent - each field
 * is only actually parsed (and the shared iterator advanced) when that
 * field's Lazy is invoked. A compound field that's "skipped" by simply
 * never invoking it - or invoked only one level deep, leaving *its own*
 * Pair/List/Sum children un-invoked - leaves those bytes unread, silently
 * misaligning every read that follows. Scalars (Unit/VarInt/Bytes/DynBytes)
 * are eagerly consumed by ParsedData::parse itself, so there is nothing
 * left for this function to do once it reaches one.
 */
inline void drainParsedData(const ParsedData& pd) {
  std::visit(
      [](auto v) {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, ParsedData::Pair>) {
          drainParsedData(v.first());
          drainParsedData(v.second());
        } else if constexpr (std::is_same_v<V, ParsedData::List>) {
          for (uint64_t i = 0; i < v.length; i++) {
            drainParsedData(v.values());
          }
        } else if constexpr (std::is_same_v<V, ParsedData::Sum>) {
          drainParsedData(v.value());
        }
      },
      pd.val);
}

}  // namespace oi::exporters

#endif
