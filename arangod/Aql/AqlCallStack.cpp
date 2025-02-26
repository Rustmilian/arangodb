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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "AqlCallStack.h"

#include <absl/strings/str_cat.h>
#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

// TODO: This class is not yet memory efficient or optimized in any way.
// it might be reimplemented soon to have the above features, Focus now is on
// the API we want to use.

using namespace arangodb;
using namespace arangodb::aql;

AqlCallStack::AqlCallStack(AqlCallStack::Empty) {}

AqlCallStack::AqlCallStack(AqlCallList call) : _operations{{std::move(call)}} {}

AqlCallStack::AqlCallStack(AqlCallStack const& other, AqlCallList call)
    : _operations{other._operations} {
  // We can only use this constructor on relevant levels
  // All others need to use passThrough constructor
  _operations.emplace_back(std::move(call));
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  validateNoCallHasSkippedRows();
#endif
}

AqlCallStack::AqlCallStack(std::vector<AqlCallList>&& operations) noexcept
    : _operations(std::move(operations)) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  validateNoCallHasSkippedRows();
#endif
}

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
auto AqlCallStack::validateNoCallHasSkippedRows() -> void {
  for (auto const& list : _operations) {
    TRI_ASSERT(list.peekNextCall().getSkipCount() == 0);
  }
}
#endif

auto AqlCallStack::popCall() -> AqlCallList {
  TRI_ASSERT(!_operations.empty());
  auto call = std::move(_operations.back());
  _operations.pop_back();
  return call;
}

void AqlCallStack::popDepthsLowerThan(size_t depth) {
  TRI_ASSERT(!_operations.empty());
  TRI_ASSERT(depth <= _operations.size());
  for (auto i = _operations.size() - depth; i < _operations.size(); ++i) {
    auto& operation = _operations[i];
    if (operation.hasMoreCalls()) {
      std::ignore = operation.popNextCall();
    }
  }
}

auto AqlCallStack::peek() const noexcept -> AqlCall const& {
  TRI_ASSERT(!_operations.empty());
  return _operations.back().peekNextCall();
}

void AqlCallStack::pushCall(AqlCallList&& call) {
  _operations.emplace_back(std::move(call));
}

void AqlCallStack::pushCall(AqlCallList const& call) {
  _operations.emplace_back(call);
}

auto AqlCallStack::fromVelocyPack(velocypack::Slice slice)
    -> ResultT<AqlCallStack> {
  if (ADB_UNLIKELY(!slice.isArray())) {
    return Result(
        TRI_ERROR_TYPE_ERROR,
        absl::StrCat("When deserializing AqlCallStack: expected array, got ",
                     slice.typeName()));
  }
  if (ADB_UNLIKELY(slice.isEmptyArray())) {
    return Result(TRI_ERROR_TYPE_ERROR,
                  "When deserializing AqlCallStack: stack is empty");
  }

  auto stack = std::vector<AqlCallList>{};
  stack.reserve(slice.length());
  for (auto entry : VPackArrayIterator(slice)) {
    auto maybeAqlCall = AqlCallList::fromVelocyPack(entry);

    if (ADB_UNLIKELY(maybeAqlCall.fail())) {
      return Result(
          TRI_ERROR_TYPE_ERROR,
          absl::StrCat("When deserializing AqlCallStack: entry ", stack.size(),
                       ": ", std::move(maybeAqlCall).errorMessage()));
    }

    stack.emplace_back(std::move(maybeAqlCall.get()));
  }

  TRI_ASSERT(!stack.empty());

  return AqlCallStack{std::move(stack)};
}

void AqlCallStack::toVelocyPack(velocypack::Builder& builder) const {
  builder.openArray();
  for (auto const& call : _operations) {
    call.toVelocyPack(builder);
  }
  builder.close();
}

auto AqlCallStack::toString() const -> std::string {
  auto result = std::string{};
  result += "[";
  bool isFirst = true;
  for (auto const& op : _operations) {
    if (!isFirst) {
      result += ",";
    }
    isFirst = false;
    result += " ";
    result += op.toString();
  }
  result += " ]";
  return result;
}

auto AqlCallStack::createEquivalentFetchAllShadowRowsStack() const
    -> AqlCallStack {
  AqlCallStack res{*this};
  // We can always overfetch the next subquery here.
  // We gonna need all data anyways.
  for (auto& op : res._operations) {
    op.createEquivalentFetchAllRowsCall();
  }
  return res;
}

auto AqlCallStack::needToCountSubquery() const noexcept -> bool {
  return std::any_of(_operations.begin(), _operations.end(),
                     [](AqlCallList const& call) -> bool {
                       auto const& nextCall = call.peekNextCall();
                       return nextCall.needSkipMore() || nextCall.hasLimit();
                     });
}

auto AqlCallStack::needToSkipSubquery() const noexcept -> bool {
  return std::any_of(_operations.begin(), _operations.end(),
                     [](AqlCallList const& call) -> bool {
                       auto const& nextCall = call.peekNextCall();
                       return nextCall.needSkipMore() ||
                              nextCall.hardLimit == 0;
                     });
}

auto AqlCallStack::shadowRowDepthToSkip() const -> size_t {
  TRI_ASSERT(needToCountSubquery());
  size_t const n = _operations.size();
  for (size_t i = 0; i < n; ++i) {
    auto& call = _operations[i];
    auto const& nextCall = call.peekNextCall();
    if (nextCall.needSkipMore() || nextCall.getLimit() == 0) {
      return n - i - 1;
    }
  }
  return 0;
}

auto AqlCallStack::modifyCallAtDepth(size_t depth) -> AqlCall& {
  // depth 0 is back of vector
  TRI_ASSERT(_operations.size() > depth);
  // Take the depth-most from top of the vector.
  auto& callList = _operations.at(_operations.size() - 1 - depth);
  return callList.modifyNextCall();
}

auto AqlCallStack::modifyCallListAtDepth(size_t depth) -> AqlCallList& {
  TRI_ASSERT(_operations.size() > depth);
  return _operations.at(_operations.size() - 1 - depth);
}

auto AqlCallStack::getCallAtDepth(size_t depth) const -> AqlCall const& {
  // depth 0 is back of vector
  TRI_ASSERT(_operations.size() > depth);
  // Take the depth-most from top of the vector.
  auto& callList = _operations.at(_operations.size() - 1 - depth);
  return callList.peekNextCall();
}

auto AqlCallStack::modifyTopCall() -> AqlCall& {
  TRI_ASSERT(!_operations.empty());
  return modifyCallAtDepth(0);
}

auto AqlCallStack::hasAllValidCalls() const noexcept -> bool {
  return std::all_of(
      _operations.begin(), _operations.end(), [](AqlCallList const& list) {
        if (!list.hasMoreCalls()) {
          return false;
        }
        auto const& nextCall = list.peekNextCall();
        // We cannot continue if any of our calls has a softLimit reached.
        return !(nextCall.hasSoftLimit() && nextCall.getLimit() == 0 &&
                 nextCall.getOffset() == 0);
      });
}

auto AqlCallStack::requestLessDataThan(AqlCallStack const& other) const noexcept
    -> bool {
  if (_operations.size() != other._operations.size()) {
    return false;
  }
  for (size_t i = 0; i < _operations.size(); ++i) {
    if (!_operations[i].requestLessDataThan(other._operations[i])) {
      return false;
    }
  }
  return true;
}

#ifdef ARANGODB_USE_GOOGLE_TESTS
// For tests
AqlCallStack::AqlCallStack(std::initializer_list<AqlCallList> calls)
    : _operations{std::move(calls)} {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  validateNoCallHasSkippedRows();
#endif
}
#endif
