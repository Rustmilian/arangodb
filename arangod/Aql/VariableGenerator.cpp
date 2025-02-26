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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "Aql/VariableGenerator.h"
#include "Basics/Exceptions.h"
#include "Basics/debugging.h"
#include "Basics/ResourceUsage.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

using namespace arangodb::aql;

/// @brief create the generator
VariableGenerator::VariableGenerator(arangodb::ResourceMonitor& resourceMonitor)
    : _id(0), _resourceMonitor(resourceMonitor) {
  _variables.reserve(8);
}

/// @brief visit all variables
void VariableGenerator::visit(std::function<void(Variable*)> const& visitor) {
  for (auto& it : _variables) {
    visitor(it.second.get());
  }
}

/// @brief return a map of all variable ids with their names
std::unordered_map<VariableId, std::string const> VariableGenerator::variables(
    bool includeTemporaries) const {
  std::unordered_map<VariableId, std::string const> result;

  for (auto const& it : _variables) {
    // check if we should include this variable...
    if (!includeTemporaries && !it.second->isUserDefined()) {
      continue;
    }

    result.try_emplace(it.first, it.second->name);
  }

  return result;
}

/// @brief generate a variable
Variable* VariableGenerator::createVariable(std::string_view name,
                                            bool isUserDefined) {
  if (isUserDefined && !isValidName(name.data(), name.data() + name.size())) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_QUERY_PARSE,
        arangodb::basics::Exception::FillExceptionString(
            TRI_ERROR_QUERY_VARIABLE_NAME_INVALID, std::string{name}.c_str()));
  }

  auto id = nextId();
  auto nameCopy = [&] {
    if (name.empty()) {
      TRI_ASSERT(!isUserDefined);
      return std::to_string(id);
    } else {
      return std::string{name};
    }
  }();
  auto variable = std::make_unique<Variable>(std::move(nameCopy), id, false,
                                             _resourceMonitor);

  TRI_ASSERT(!isUserDefined || variable->isUserDefined());
  auto [it, inserted] = _variables.emplace(id, std::move(variable));
  TRI_ASSERT(inserted);
  return (*it).second.get();
}

Variable* VariableGenerator::createVariable(Variable const* original) {
  TRI_ASSERT(original != nullptr);
  std::unique_ptr<Variable> variable(original->clone());

  // check if insertion into the table actually works.
  VariableId const id = variable->id;
  auto [it, inserted] = _variables.emplace(id, std::move(variable));
  if (!inserted) {
    // variable was already present. this is unexpected...
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "cloned AQL variable already present");
  }
  // variable was inserted, return the clone
  _id = std::max(id + 1, _id);
  return (*it).second.get();
}

/// @brief generate a variable from VelocyPack
Variable* VariableGenerator::createVariable(VPackSlice slice) {
  auto variable = std::make_unique<Variable>(slice, _resourceMonitor);
  VariableId const id = variable->id;

  // make sure _id is at least as high as the highest variable id
  // we get.
  _id = std::max(id + 1, _id);

  auto it = _variables.try_emplace(id, std::move(variable)).first;
  return (*it).second.get();
}

/// @brief generate a temporary variable
Variable* VariableGenerator::createTemporaryVariable() {
  return createVariable({}, false);
}

/// @brief renames a variable (assigns a temporary name)
Variable* VariableGenerator::renameVariable(VariableId id) {
  return renameVariable(id, nextName());
}

/// @brief renames a variable (assigns the specified name
Variable* VariableGenerator::renameVariable(VariableId id,
                                            std::string const& name) {
  Variable* v = getVariable(id);

  if (v != nullptr) {
    v->name = name;
  }

  return v;
}

/// @brief return a variable by id - this does not respect the scopes!
Variable* VariableGenerator::getVariable(VariableId id) const {
  auto it = _variables.find(id);

  if (it == _variables.end()) {
    return nullptr;
  }

  return (*it).second.get();
}

/// @brief return the next temporary variable name
std::string VariableGenerator::nextName() {
  // note: if the naming scheme is adjusted, it may be necessary to adjust
  // Variable::isUserDefined, too!
  return std::to_string(nextId());
}

/// @brief export to VelocyPack
void VariableGenerator::toVelocyPack(VPackBuilder& builder) const {
  VPackArrayBuilder guard(&builder);
  for (auto const& it : _variables) {
    it.second->toVelocyPack(builder, Variable::WithConstantValue{});
  }
}

/// @brief import from VelocyPack
void VariableGenerator::fromVelocyPack(VPackSlice const slice) {
  VPackSlice allVariablesList = slice;
  if (slice.isObject()) {
    allVariablesList = slice.get("variables");
  }

  if (!allVariablesList.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "variables needs to be an array");
  }

  auto len = allVariablesList.length();
  _variables.reserve(static_cast<size_t>(len));

  for (auto var : VPackArrayIterator(allVariablesList)) {
    createVariable(var);
  }
}

bool VariableGenerator::isValidName(char const* p, char const* end) noexcept {
  // ($?|_+)
  if (p != end) {
    if (*p == '$') {
      ++p;
    } else {
      while (*p == '_' && p != end) {
        ++p;
      }
    }
  }

  // [a-zA-Z]+
  char const* begin = p;
  while (p != end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
    ++p;
  }

  if (begin == p) {
    return false;
  }

  // [a-zA-Z0-9_]*
  while (p != end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                      (*p >= '0' || *p <= '9') || *p == '_')) {
    ++p;
  }

  return (p == end);
}

VariableId VariableGenerator::nextId() noexcept { return _id++; }
