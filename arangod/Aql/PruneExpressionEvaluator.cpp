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

#include "PruneExpressionEvaluator.h"

#include "Aql/AqlValue.h"
#include "Aql/Expression.h"
#include "Cluster/ServerState.h"
#include "Transaction/Methods.h"
#include "VocBase/vocbase.h"

using namespace arangodb;
using namespace arangodb::aql;

PruneExpressionEvaluator::PruneExpressionEvaluator(
    transaction::Methods& trx, QueryContext& query,
    AqlFunctionsInternalCache& cache, std::vector<Variable const*> vars,
    std::vector<RegisterId> regs, size_t vertexVarIdx, size_t edgeVarIdx,
    size_t pathVarIdx, Expression* expr)
    : _pruneExpression(expr),
      _ctx(trx, query, cache, std::move(vars), std::move(regs), vertexVarIdx,
           edgeVarIdx, pathVarIdx) {
  TRI_ASSERT(_pruneExpression == nullptr ||
             !ServerState::instance()->isRunningInCluster() ||
             _pruneExpression->canRunOnDBServer(_ctx.vocbase().isOneShard()));
}

PruneExpressionEvaluator::~PruneExpressionEvaluator() = default;

bool PruneExpressionEvaluator::evaluate() {
  bool mustDestroy = false;
  TRI_ASSERT(_pruneExpression != nullptr);
  aql::AqlValue res = _pruneExpression->execute(&_ctx, mustDestroy);
  arangodb::aql::AqlValueGuard guard(res, mustDestroy);
  return res.toBoolean();
}
