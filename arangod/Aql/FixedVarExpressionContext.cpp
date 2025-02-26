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

#include "FixedVarExpressionContext.h"
#include "Aql/AqlValue.h"
#include "Aql/Variable.h"
#include "Basics/debugging.h"

#include <velocypack/Builder.h>

using namespace arangodb;
using namespace arangodb::aql;

FixedVarExpressionContext::FixedVarExpressionContext(
    transaction::Methods& trx, QueryContext& context,
    AqlFunctionsInternalCache& cache)
    : QueryExpressionContext(trx, context, cache) {}

AqlValue FixedVarExpressionContext::getVariableValue(Variable const* variable,
                                                     bool doCopy,
                                                     bool& mustDestroy) const {
  return QueryExpressionContext::getVariableValue(
      variable, doCopy, mustDestroy,
      [this](Variable const* variable, bool doCopy, bool& mustDestroy) {
        // now check our own temporary variables
        auto it = _vars.find(variable);
        if (it == _vars.end()) {
          TRI_ASSERT(false);
          return AqlValue(AqlValueHintNull());
        }
        mustDestroy = doCopy;
        if (doCopy) {
          return it->second.clone();
        }
        return it->second;
      });
}

void FixedVarExpressionContext::clearVariableValues() noexcept {
  _vars.clear();
}

void FixedVarExpressionContext::setVariableValue(Variable const* var,
                                                 AqlValue const& value) {
  TRI_ASSERT(!_vars.contains(var));
  // always overwrite current map value if it exists.
  _vars.insert_or_assign(var, value);
}

void FixedVarExpressionContext::clearVariableValue(
    Variable const* var) noexcept {
  _vars.erase(var);
}

void FixedVarExpressionContext::serializeAllVariables(
    velocypack::Options const& opts, velocypack::Builder& builder) const {
  TRI_ASSERT(builder.isOpenArray());
  for (auto const& it : _vars) {
    builder.openArray();
    it.first->toVelocyPack(builder);
    it.second.toVelocyPack(&opts, builder, /*allowUnindexed*/ false);
    builder.close();
  }
}

NoVarExpressionContext::NoVarExpressionContext(transaction::Methods& trx,
                                               QueryContext& context,
                                               AqlFunctionsInternalCache& cache)
    : QueryExpressionContext(trx, context, cache) {}

AqlValue NoVarExpressionContext::getVariableValue(Variable const* /*variable*/,
                                                  bool /*doCopy*/,
                                                  bool& /*mustDestroy*/) const {
  return AqlValue(AqlValueHintNull());
}
