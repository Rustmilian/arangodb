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

#include "TraverserCache.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/AqlValue.h"
#include "Aql/Query.h"
#include "Basics/StringHeap.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ServerState.h"
#include "Graph/BaseOptions.h"
#include "Graph/EdgeDocumentToken.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "RestServer/QueryRegistryFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Methods.h"
#include "Transaction/Options.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Builder.h>
#include <velocypack/HashedStringRef.h>
#include <velocypack/Slice.h>

using namespace arangodb;
using namespace arangodb::graph;

namespace {
constexpr size_t costPerPersistedString =
    sizeof(void*) + sizeof(arangodb::velocypack::HashedStringRef);
}  // namespace

TraverserCache::TraverserCache(aql::QueryContext& query, BaseOptions* opts)
    : _query(query),
      _trx(opts->trx()),
      _insertedDocuments(0),
      _filtered(0),
      _cursorsCreated(0),
      _cursorsRearmed(0),
      _cacheHits(0),
      _cacheMisses(0),
      _stringHeap(
          query.resourceMonitor(),
          4096), /* arbitrary block-size, may be adjusted for performance */
      _baseOptions(opts),
      _allowImplicitCollections(ServerState::instance()->isSingleServer() &&
                                !_query.vocbase()
                                     .server()
                                     .getFeature<QueryRegistryFeature>()
                                     .requireWith()) {}

TraverserCache::~TraverserCache() { clear(); }

void TraverserCache::clear() {
  _query.resourceMonitor().decreaseMemoryUsage(_persistedStrings.size() *
                                               ::costPerPersistedString);

  _persistedStrings.clear();
  _docBuilder.clear();
  _stringHeap.clear();
}

VPackSlice TraverserCache::lookupToken(EdgeDocumentToken const& idToken) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  auto col = _trx->vocbase().lookupCollection(idToken.cid());

  if (col == nullptr) {
    // collection gone... should not happen
    LOG_TOPIC("3b2ba", ERR, arangodb::Logger::GRAPHS)
        << "Could not extract indexed edge document. collection not found";
    TRI_ASSERT(col != nullptr);  // for maintainer mode
    return arangodb::velocypack::Slice::nullSlice();
  }

  _docBuilder.clear();
  auto cb = IndexIterator::makeDocumentCallback(_docBuilder);
  if (col->getPhysical()
          ->lookup(_trx, idToken.localDocumentId(), cb, {})
          .fail()) {
    // We already had this token, inconsistent state. Return NULL in Production
    LOG_TOPIC("3acb3", ERR, arangodb::Logger::GRAPHS)
        << "Could not extract indexed edge document, return 'null' instead. "
        << "This is most likely a caching issue. Try: 'db." << col->name()
        << ".unload(); db." << col->name()
        << ".load()' in arangosh to fix this.";
    TRI_ASSERT(false);  // for maintainer mode
    return arangodb::velocypack::Slice::nullSlice();
  }

  return _docBuilder.slice();
}

void TraverserCache::insertEdgeIntoResult(EdgeDocumentToken const& idToken,
                                          VPackBuilder& builder) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  builder.add(lookupToken(idToken));
}

aql::AqlValue TraverserCache::fetchEdgeAqlResult(
    EdgeDocumentToken const& idToken) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  return aql::AqlValue(lookupToken(idToken));
}

std::string_view TraverserCache::persistString(std::string_view idString) {
  return persistString(
             arangodb::velocypack::HashedStringRef(
                 idString.data(), static_cast<uint32_t>(idString.size())))
      .stringView();
}

arangodb::velocypack::HashedStringRef TraverserCache::persistString(
    arangodb::velocypack::HashedStringRef idString) {
  auto it = _persistedStrings.find(idString);
  if (it != _persistedStrings.end()) {
    return *it;
  }
  auto res = _stringHeap.registerString(idString);
  {
    ResourceUsageScope guard(_query.resourceMonitor(),
                             ::costPerPersistedString);

    _persistedStrings.emplace(res);

    // now make the TraverserCache responsible for memory tracking
    guard.steal();
  }
  return res;
}
