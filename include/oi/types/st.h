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
#ifndef INCLUDED_OI_TYPES_ST_H
#define INCLUDED_OI_TYPES_ST_H 1

/*
 * Static Types
 *
 * Static types describe the shape of the data stream written by generated OI
 * traversal code. They are not C++ object-layout descriptions. Instead, they
 * form a small compile-time protocol for writing typed data into a DataBuffer.
 *
 *
 * A DataBuffer is any small, cheaply copyable object that provides the following
 * methods:
 *
 *    void write_byte(uint8_t);
 *    size_t offset();
 *
 * Each static type stores only a DataBuffer. The type itself represents the
 * current write state: "what kind of data is expected next, and what remains
 * after that data has been written".
 *
 * The key idea is that writing advances the static type. A write does not
 * mutate the type object in-place from the caller's point of view; it returns a
 * new static type representing the remaining unwritten stream.
 *
 * For example, given the stream shape:
 *
 *    Pair<DB, VarInt<DB>, VarInt<DB>>
 *
 * the first write consumes the first VarInt and returns the remaining VarInt:
 *
 *    auto second = pair.write(10);
 *
 * The second write consumes that remaining VarInt and returns Unit:
 *
 *    auto done = pair.write(10).write(20);
 *
 * Unit means "this part of the stream is complete" or "there is no interesting
 * data to write for this part". Pair uses Unit internally as the hand-off point
 * between the completed first part and the remaining second part.
 *
 * The primitive stream type is:
 *
 * VarInt<DB>
 * ----------
 *
 * VarInt writes one unsigned integer value using variable-length encoding and
 * then returns Unit.
 *
 * The compound stream types are:
 *
 *    Pair<DB, First, Rest>
 *
 * A sequence. First is written before Rest. Pair is used recursively to
 * represent longer sequences:
 *
 *    Pair<DB, A, Pair<DB, B, C>>
 *
 * describes the stream A, then B, then C.
 *
 * Sum<DB, Alternatives...>
 * ------------------------
 *
 * Sum is a tagged union. Sum::write<I>() first writes the selected alternative
 * index I as a VarInt, then returns the static type for that selected
 * alternative so its payload can be written.
 *
 * List<DB, Element>
 * -----------------
 *
 * List is a length-prefixed sequence. The list first writes its length, then
 * exposes a ListContents<Element> state for writing each element.
 *
 * There are three ways to advance a static type:
 *
 *    .write(value)
 *
 * Use this when the current first component can be fully written from one
 * value. For example:
 *
 *    Pair<DB, VarInt<DB>, VarInt<DB>> pair{db};
 *
 *    pair.write(length).write(capacity);
 *
 * The values passed to write() are payload values written into the data
 * stream; they are not positions or indexes.
 *
 *    .delegate(callback)
 *
 * Use delegate when the current first component is itself compound and needs
 * multiple operations to write. The callback receives the first component
 * and must consume it completely, returning Unit. delegate() then converts
 * that Unit into the remaining type.
 *
 * For example:
 *    using Header = Pair<DB, VarInt<DB>, VarInt<DB>>;
 *    using Stream = Pair<DB, Header, VarInt<DB>>;
 *
 *    Stream stream{db};
 *    stream.delegate([&](auto header) {
 *      return header.write(length).write(capacity);
 *    }).write(flags);
 *
 * Here the callback consumes Header and returns Unit. delegate() converts
 * that completed Header into the remaining VarInt<DB>, allowing flags to be
 * written next.
 *
 *    .consume(callback)
 *
 * Use this at the end of a generated write chain, or when the caller wants
 * the callback to consume the entire current static type. The callback is
 * given the current type and must return Unit.
 *
 * This design lets generated code express the stream shape in the C++ type
 * system. If generated code writes too little, writes too much, or writes the
 * wrong part of a compound shape, the error is usually caught as a compile-time
 * type mismatch rather than as a later decoding error.
 *
 * DEFINE_DESCRIBE enables the runtime mirror of these static types. When it is
 * defined, each static type exposes a constexpr `describe` value from
 * oi::types::dy. The dynamic description is used by the reader side to decode
 * the byte stream with the same shape that the writer used.
 */

namespace oi::types::st {

#ifdef DEFINE_DESCRIBE
#include "oi/types/dy.h"
#endif

/*
 * Unit
 *
 * Represents the case of having completely written the type, or having nothing
 * of interest to write. Examples are after having written the final element of
 * the object, after having completely delegated a field, or having a field of
 * a struct that makes sense structurally but holds no interesting data.
 */
template <typename DataBuffer>
class Unit {
 public:
  Unit(DataBuffer db) : _buf(db) {
  }

  size_t offset() {
    return _buf.offset();
  }

  template <typename F>
  Unit<DataBuffer> consume(F const& cb) {
    return cb(*this);
  }

#ifdef DEFINE_DESCRIBE
  static constexpr types::dy::Unit describe{};
#endif

 private:
  /*
   * Allows you to cast the Unit type to another Static Type. Think very
   * carefully before using it. It is private so that only friends can access
   * it. Good use cases are Pair::write and Pair::delegate to cast the result to
   * the second element. Bad use cases are within a type handler because the
   * type doesn't quite fit.
   */
  template <typename T>
  T cast() {
    return T(_buf);
  }

 private:
  DataBuffer _buf;

  template <typename DB, typename T1, typename T2>
  friend class Pair;
  template <typename DB, typename T>
  friend class ListContents;
};

/*
 * VarInt
 *
 * Represents a variable length integer. The only primitive type at present,
 * used for all data transfer.
 */
template <typename DataBuffer>
class VarInt {
 public:
  VarInt(DataBuffer db) : _buf(db) {
  }

  Unit<DataBuffer> write(uint64_t val) {
    while (val >= 0x80) {
      _buf.write_byte(0x80 | (val & 0x7f));
      val >>= 7;
    }
    _buf.write_byte(uint8_t(val));
    return Unit<DataBuffer>(_buf);
  }

  template <typename F>
  Unit<DataBuffer> consume(F const& cb) {
    return cb(*this);
  }

#ifdef DEFINE_DESCRIBE
  static constexpr types::dy::VarInt describe{};
#endif

 private:
  DataBuffer _buf;
};

/*
 * Pair<T1,T2>
 *
 * Represents a pair of types. Can be combined to hold an arbitrary number of
 * types, e.g. Pair<VarInt, Pair<VarInt, VarInt>> allows you to write three
 * integers.
 */
template <typename DataBuffer, typename T1, typename T2>
class Pair {
 public:
  Pair(DataBuffer db) : _buf(db) {
  }

  template <typename Value>
  T2 write(Value value) {
    Unit<DataBuffer> finishedFirstPart = T1(_buf).write(value);
    return finishedFirstPart.template cast<T2>();
  }

  template <typename WriteFirstPart>
  T2 delegate(WriteFirstPart const& writeFirstPart) {
    T1 firstPart = T1(_buf);
    Unit<DataBuffer> finishedFirstPart = writeFirstPart(firstPart);
    return finishedFirstPart.template cast<T2>();
  }

  template <typename ConsumePair>
  Unit<DataBuffer> consume(ConsumePair const& consumePair) {
    return consumePair(*this);
  }

#ifdef DEFINE_DESCRIBE
  static constexpr types::dy::Pair describe{T1::describe, T2::describe};
#endif

 private:
  DataBuffer _buf;
};

/*
 * Sum<Types...>
 *
 * Represents a tagged union of types.
 */
template <typename DataBuffer, typename... Types>
class Sum {
 private:
  /*
   * Selector<I, Elements...>
   *
   * Selects the Ith type of Elements... and makes it available at ::type.
   */
  template <size_t I, typename... Elements>
  struct Selector;
  template <size_t I, typename Head, typename... Tail>
  struct Selector<I, Head, Tail...> {
    using type = typename std::conditional<
        I == 0,
        Head,
        typename Selector<I - 1, Tail...>::type>::type;
  };
  template <size_t I>
  struct Selector<I> {
    using type = int;
  };

 public:
  Sum(DataBuffer db) : _buf(db) {
  }

  template <size_t I>
  typename Selector<I, Types...>::type write() {
    Pair<DataBuffer, VarInt<DataBuffer>, typename Selector<I, Types...>::type>
        buf(_buf);
    return buf.write(I);
  }

  template <size_t I, typename F>
  Unit<DataBuffer> delegate(F const& cb) {
    auto tail = write<I>();
    return cb(tail);
  }

  template <typename F>
  Unit<DataBuffer> consume(F const& cb) {
    return cb(*this);
  }

#ifdef DEFINE_DESCRIBE
 private:
  static constexpr std::array<types::dy::Dynamic, sizeof...(Types)> members{
      Types::describe...};

 public:
  static constexpr types::dy::Sum describe{members};
#endif

 private:
  DataBuffer _buf;
};

/*
 * ListContents<T>
 *
 * Repeatedly delegate instances of type T, writing them one after the other.
 * Terminate with a call to finish().
 */
template <typename DataBuffer, typename T>
class ListContents {
 public:
  ListContents(DataBuffer db) : _buf(db) {
  }

  template <typename F>
  ListContents<DataBuffer, T> delegate(F const& cb) {
    T head = T(_buf);
    Unit<DataBuffer> tail = cb(head);
    return tail.template cast<ListContents<DataBuffer, T>>();
  }

  Unit<DataBuffer> finish() {
    return {_buf};
  }

 private:
  DataBuffer _buf;
};

/*
 * List<T>
 *
 * Holds the length of a list followed by the elements. Write the length of the
 * list first then that number of elements.
 *
 * BEWARE: There is NO static or dynamic checking that you write the number of
 * elements promised.
 */
template <typename DataBuffer, typename T>
class List
    : public Pair<DataBuffer, VarInt<DataBuffer>, ListContents<DataBuffer, T>> {
 public:
  List(DataBuffer db)
      : Pair<DataBuffer, VarInt<DataBuffer>, ListContents<DataBuffer, T>>(db) {
  }

  template <typename F>
  Unit<DataBuffer> consume(F const& cb) {
    return cb(*this);
  }

#ifdef DEFINE_DESCRIBE
 public:
  static constexpr types::dy::List describe{T::describe};
#endif
};

}  // namespace oi::types::st

#endif
