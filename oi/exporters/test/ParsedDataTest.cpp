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
#include <gtest/gtest.h>

#define DEFINE_DESCRIBE 1
#include "oi/types/dy.h"
#include "oi/types/st.h"

#include "oi/exporters/ParsedData.h"

#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

using oi::exporters::ParsedData;

namespace {

// A minimal, real DataBuffer (see the contract documented in
// oi/types/st.h): generated code uses a code-string equivalent
// (FuncGen::DefineBackInserterDataBuffer) that isn't available to link
// against directly from a unit test.
class VectorDataBuffer {
 public:
  explicit VectorDataBuffer(std::vector<uint8_t>& buf) : buf_(&buf) {
  }

  void write_byte(uint8_t byte) {
    buf_->push_back(byte);
  }

  size_t offset() {
    return buf_->size();
  }

 private:
  std::vector<uint8_t>* buf_;
};

template <typename T>
T roundTripBytes(const T& value) {
  std::vector<uint8_t> buf;
  VectorDataBuffer db{buf};
  using Writer = oi::types::st::Bytes<VectorDataBuffer, sizeof(T)>;
  Writer{db}.write(std::bit_cast<std::array<uint8_t, sizeof(T)>>(value));

  auto it = buf.cbegin();
  ParsedData parsed = ParsedData::parse(it, Writer::describe);

  const auto& bytes = std::get<ParsedData::Bytes>(parsed.val).value;
  EXPECT_EQ(bytes.size(), sizeof(T));

  std::array<uint8_t, sizeof(T)> arr;
  std::copy(bytes.begin(), bytes.end(), arr.begin());
  return std::bit_cast<T>(arr);
}

}  // namespace

TEST(ParsedDataBytes, RoundTripsPositiveInt) {
  int32_t value = 123456;
  EXPECT_EQ(roundTripBytes(value), value);
}

TEST(ParsedDataBytes, RoundTripsNegativeInt) {
  // The whole point of Bytes over VarInt: a negative value must survive
  // unchanged, not get reinterpreted/sign-mangled through a numeric cast.
  int32_t value = -1;
  EXPECT_EQ(roundTripBytes(value), value);
}

TEST(ParsedDataBytes, RoundTripsBool) {
  EXPECT_EQ(roundTripBytes(true), true);
  EXPECT_EQ(roundTripBytes(false), false);
}

TEST(ParsedDataBytes, RoundTripsFloatNaN) {
  // NaN != NaN, so compare bit patterns directly.
  float value = std::numeric_limits<float>::quiet_NaN();
  float result = roundTripBytes(value);
  EXPECT_EQ(std::bit_cast<uint32_t>(result), std::bit_cast<uint32_t>(value));
}

TEST(ParsedDataBytes, RoundTripsNegativeZero) {
  // -0.0 == 0.0 numerically, which would hide a lost/flipped sign bit -
  // compare bit patterns to actually prove nothing was reinterpreted.
  double value = -0.0;
  double result = roundTripBytes(value);
  EXPECT_EQ(std::bit_cast<uint64_t>(result), std::bit_cast<uint64_t>(value));
  EXPECT_NE(std::bit_cast<uint64_t>(result), std::bit_cast<uint64_t>(0.0));
}

TEST(ParsedDataBytes, RoundTripsMultiByteStruct) {
  struct Blob {
    uint8_t a;
    uint16_t b;
    uint8_t c;
  };
  Blob value{.a = 0xAB, .b = 0xCDEF, .c = 0x12};

  Blob result = roundTripBytes(value);
  EXPECT_EQ(result.a, value.a);
  EXPECT_EQ(result.b, value.b);
  EXPECT_EQ(result.c, value.c);
}

// Baseline/control: confirms this file's own write+parse test methodology
// is sound against the pre-existing VarInt primitive, not just the new one.
TEST(ParsedDataBytes, ControlVarIntStillRoundTrips) {
  std::vector<uint8_t> buf;
  VectorDataBuffer db{buf};
  using Writer = oi::types::st::VarInt<VectorDataBuffer>;
  Writer{db}.write(300);

  auto it = buf.cbegin();
  ParsedData parsed = ParsedData::parse(it, Writer::describe);
  EXPECT_EQ(std::get<ParsedData::VarInt>(parsed.val).value, 300u);
}
