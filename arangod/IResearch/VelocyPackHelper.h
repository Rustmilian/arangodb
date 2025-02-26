////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Common.h"
#include "Basics/debugging.h"

#include <velocypack/Slice.h>

#include "utils/string.hpp"  // for std::string_view

namespace arangodb {
namespace velocypack {

class Builder;  // forward declarations

}  // namespace velocypack
}  // namespace arangodb

namespace arangodb {
namespace iresearch {

// according to Slice.h:330
uint8_t const COMPACT_ARRAY = 0x13;
uint8_t const COMPACT_OBJECT = 0x14;

template<typename Char>
irs::basic_string_view<Char> ref(VPackSlice slice) {
  static_assert(sizeof(Char) == sizeof(uint8_t),
                "sizeof(Char) != sizeof(uint8_t)");

  return irs::basic_string_view<Char>(
      reinterpret_cast<Char const*>(slice.begin()), slice.byteSize());
}

template<typename Char>
VPackSlice slice(irs::basic_string_view<Char> const& ref) {
  static_assert(sizeof(Char) == sizeof(uint8_t),
                "sizeof(Char) != sizeof(uint8_t)");

  return VPackSlice(reinterpret_cast<uint8_t const*>(ref.data()));
}

template<typename Char>
VPackSlice slice(irs::basic_string<Char> const& ref) {
  static_assert(sizeof(Char) == sizeof(uint8_t),
                "sizeof(Char) != sizeof(uint8_t)");

  return VPackSlice(reinterpret_cast<uint8_t const*>(ref.c_str()));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief add a string_ref value to the 'builder' (for JSON arrays)
////////////////////////////////////////////////////////////////////////////////
velocypack::Builder& addBytesRef(velocypack::Builder& builder,
                                 irs::bytes_view value);

////////////////////////////////////////////////////////////////////////////////
/// @brief add a string_ref value to the 'builder' (for JSON objects)
////////////////////////////////////////////////////////////////////////////////
velocypack::Builder& addBytesRef(velocypack::Builder& builder,
                                 std::string_view key, irs::bytes_view value);

////////////////////////////////////////////////////////////////////////////////
/// @brief add a string_ref value to the 'builder' (for JSON arrays)
////////////////////////////////////////////////////////////////////////////////
velocypack::Builder& addStringRef(velocypack::Builder& builder,
                                  std::string_view value);

////////////////////////////////////////////////////////////////////////////////
/// @brief wraps bytes ref with VPackValuePair
////////////////////////////////////////////////////////////////////////////////
inline velocypack::ValuePair toValuePair(irs::bytes_view ref) {
  TRI_ASSERT(!irs::IsNull(ref));  // consumers of ValuePair usually use
                                  // memcpy(...) which cannot handle nullptr
  return velocypack::ValuePair(ref.data(), ref.size(),
                               velocypack::ValueType::Binary);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief wraps string ref with VPackValuePair
////////////////////////////////////////////////////////////////////////////////
inline velocypack::ValuePair toValuePair(std::string_view ref) {
  TRI_ASSERT(!irs::IsNull(ref));  // consumers of ValuePair usually use
                                  // memcpy(...) which cannot handle nullptr
  return velocypack::ValuePair(ref.data(), ref.size(),
                               velocypack::ValueType::String);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief add a string_ref value to the 'builder' (for JSON objects)
////////////////////////////////////////////////////////////////////////////////
velocypack::Builder& addStringRef(velocypack::Builder& builder,
                                  std::string_view key, std::string_view value);

inline bool isArrayOrObject(VPackSlice slice) {
  auto const type = slice.type();
  return VPackValueType::Array == type || VPackValueType::Object == type;
}

inline bool isCompactArrayOrObject(VPackSlice slice) {
  TRI_ASSERT(isArrayOrObject(slice));

  auto const head = slice.head();
  return COMPACT_ARRAY == head || COMPACT_OBJECT == head;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief extracts string_ref from VPackSlice, note that provided 'slice'
///        must be a string
/// @return extracted string_ref
//////////////////////////////////////////////////////////////////////////////
inline std::string_view getStringRef(VPackSlice slice) {
  if (slice.isNull()) {
    return {};
  }
  TRI_ASSERT(slice.isString());
  return slice.stringView();
}

//////////////////////////////////////////////////////////////////////////////
/// @brief extracts string_ref from VPackSlice, note that provided 'slice'
///        must be a string
/// @return extracted string_ref
//////////////////////////////////////////////////////////////////////////////
inline irs::bytes_view getBytesRef(VPackSlice slice) {
  TRI_ASSERT(slice.isString());
  return irs::ViewCast<irs::byte_type>(slice.stringView());
}

//////////////////////////////////////////////////////////////////////////////
/// @brief parses a numeric sub-element
/// @return success
//////////////////////////////////////////////////////////////////////////////
template<typename T>
inline bool getNumber(T& buf, velocypack::Slice const& slice) noexcept {
  if (!slice.isNumber()) {
    return false;
  }

  using NumType = std::conditional_t<std::is_floating_point_v<T>, T, double>;

  try {
    auto value = slice.getNumber<NumType>();

    buf = static_cast<T>(value);

    return value == static_cast<decltype(value)>(buf);
  } catch (...) {
    // NOOP
  }

  return false;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief parses a numeric sub-element, or uses a default if it does not exist
/// @return success
//////////////////////////////////////////////////////////////////////////////
template<typename T>
inline bool getNumber(T& buf, velocypack::Slice const& slice,
                      std::string_view fieldName, bool& seen,
                      T fallback) noexcept {
  auto field = slice.get(fieldName);
  seen = !field.isNone();
  if (!seen) {
    buf = fallback;
    return true;
  }
  return getNumber(buf, field);
}

//////////////////////////////////////////////////////////////////////////////
/// @brief parses a string sub-element, or uses a default if it does not exist
/// @return success
//////////////////////////////////////////////////////////////////////////////
inline bool getString(std::string& buf, velocypack::Slice const& slice,
                      std::string_view fieldName, bool& seen,
                      std::string const& fallback) noexcept {
  auto field = slice.get(fieldName);
  seen = !field.isNone();
  if (!seen) {
    buf = fallback;
    return true;
  }
  if (!field.isString()) {
    return false;
  }
  buf = field.stringView();
  return true;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief parses a string sub-element, or uses a default if it does not exist
/// @return success
//////////////////////////////////////////////////////////////////////////////
inline bool getString(std::string_view& buf, velocypack::Slice const& slice,
                      std::string_view fieldName, bool& seen,
                      std::string_view fallback) noexcept {
  auto field = slice.get(fieldName);
  seen = !field.isNone();
  if (!seen) {
    buf = fallback;
    return true;
  }
  if (!field.isString()) {
    return false;
  }
  buf = field.stringView();
  return true;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief look for the specified attribute path inside an Object
/// @return a value denoted by 'fallback' if not found
//////////////////////////////////////////////////////////////////////////////
template<typename T>
VPackSlice get(VPackSlice slice, const T& attributePath,
               VPackSlice fallback = VPackSlice::nullSlice()) {
  if (attributePath.empty()) {
    return fallback;
  }

  for (size_t i = 0, size = attributePath.size(); i < size; ++i) {
    slice = slice.get(attributePath[i].name);

    if (slice.isNone() || (i + 1 < size && !slice.isObject())) {
      return fallback;
    }
  }

  return slice;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief append the contents of the slice to the builder
/// @return success
//////////////////////////////////////////////////////////////////////////////
bool mergeSlice(velocypack::Builder& builder, velocypack::Slice slice);

//////////////////////////////////////////////////////////////////////////////
/// @brief append the contents of the slice to the builder skipping keys
/// @return success
//////////////////////////////////////////////////////////////////////////////
bool mergeSliceSkipKeys(
    velocypack::Builder& builder, velocypack::Slice slice,
    std::function<bool(std::string_view key)> const& acceptor);

//////////////////////////////////////////////////////////////////////////////
/// @brief append the contents of the slice to the builder skipping offsets
/// @return success
//////////////////////////////////////////////////////////////////////////////
bool mergeSliceSkipOffsets(velocypack::Builder& builder,
                           velocypack::Slice slice,
                           std::function<bool(size_t offset)> const& acceptor);

////////////////////////////////////////////////////////////////////////////
/// @struct IteratorValue
/// @brief represents of value of the iterator
////////////////////////////////////////////////////////////////////////////
struct IteratorValue {
  ///////////////////////////////////////////////////////////////////////////
  /// @brief type of the current level (Array or Object)
  ///////////////////////////////////////////////////////////////////////////
  VPackValueType type;

  ///////////////////////////////////////////////////////////////////////////
  /// @brief position at the current level
  ///////////////////////////////////////////////////////////////////////////
  VPackValueLength pos;

  ///////////////////////////////////////////////////////////////////////////
  /// @brief current key at the current level
  ///          type == Array --> key == value;
  ///////////////////////////////////////////////////////////////////////////
  VPackSlice key;

  ///////////////////////////////////////////////////////////////////////////
  /// @brief current value at the current level
  ///////////////////////////////////////////////////////////////////////////
  VPackSlice value;
};  // IteratorValue

class Iterator {
 public:
  explicit Iterator(VPackSlice slice);

  // returns true if iterator exhausted
  bool next() noexcept;
  bool valid() const noexcept { return 0 != _length; }

  IteratorValue const& value() const noexcept { return operator*(); }

  IteratorValue const& operator*() const noexcept { return _value; }

 private:
  VPackValueLength _length;
  uint8_t const* _begin;
  IteratorValue _value;
};  // Iterator

bool parseDirectionBool(arangodb::velocypack::Slice slice, bool& direction);

bool parseDirectionString(arangodb::velocypack::Slice slice, bool& direction);

bool keyFromSlice(VPackSlice keySlice, std::string_view& key);

}  // namespace iresearch
}  // namespace arangodb
