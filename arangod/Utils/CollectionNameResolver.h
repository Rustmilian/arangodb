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

#pragma once

#include "Cluster/ServerState.h"
#include "Containers/FlatHashMap.h"
#include "VocBase/Identifiers/DataSourceId.h"
#include "VocBase/voc-types.h"

#include <shared_mutex>

namespace arangodb {

class LogicalCollection;
class LogicalDataSource;
class LogicalView;

////////////////////////////////////////////////////////////////////////////////
/// @brief data-source id/name resolver and cache (single-server and cluster)
/// @note not thread-safe
////////////////////////////////////////////////////////////////////////////////
class CollectionNameResolver {
 public:
  explicit CollectionNameResolver(TRI_vocbase_t& vocbase);
  ~CollectionNameResolver() = default;

  // copy an existing resolver
  CollectionNameResolver(CollectionNameResolver const& other);

  // every other copy/move operation is disallowed
  CollectionNameResolver& operator=(CollectionNameResolver const& other) =
      delete;
  CollectionNameResolver(CollectionNameResolver&& other) = delete;
  CollectionNameResolver& operator=(CollectionNameResolver&& other) = delete;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief look up a collection struct for a collection id
  /// @return the local collection on dbserver / standalone
  ///         the cluster collection on coordinator
  //////////////////////////////////////////////////////////////////////////////
  std::shared_ptr<LogicalCollection> getCollection(DataSourceId id) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief look up a collection struct for a
  ///        collection name, stringified id (or uuid for dbserver / standalone)
  /// @return the local collection on dbserver / standalone
  ///         the cluster collection on coordinator
  //////////////////////////////////////////////////////////////////////////////
  std::shared_ptr<LogicalCollection> getCollection(
      std::string_view nameOrId) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief look up a collection id for a collection name (local case),
  /// use this if you know you are on a single server or on a DBserver
  /// and need to look up a local collection name (or shard name).
  //////////////////////////////////////////////////////////////////////////////
  DataSourceId getCollectionIdLocal(std::string_view name) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief look up a cluster collection id for a cluster collection name,
  /// only use this is in cluster mode on a coordinator or DBserver, in both
  /// cases the name is resolved as a cluster wide collection name and the
  /// cluster wide collection id is returned.
  //////////////////////////////////////////////////////////////////////////////
  DataSourceId getCollectionIdCluster(std::string_view name) const;

  std::shared_ptr<LogicalCollection> getCollectionStructCluster(
      std::string_view name) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief look up a collection id for a collection name, this is the
  /// default one to use, which will usually do the right thing. On a
  /// single server or DBserver it will use the local lookup and on a
  /// coordinator it will use the cluster wide lookup.
  //////////////////////////////////////////////////////////////////////////////
  DataSourceId getCollectionId(std::string_view name) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief look up a collection name for a collection id, this implements
  /// some magic in the cluster case: a DBserver in a cluster will automatically
  /// translate the local collection ID into a cluster wide collection name.
  //////////////////////////////////////////////////////////////////////////////
  std::string getCollectionName(DataSourceId cid) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief look up a cluster-wide collection name for a cluster-wide
  /// collection id
  //////////////////////////////////////////////////////////////////////////////
  std::string getCollectionNameCluster(DataSourceId cid) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return collection name if given string is either the name or
  /// a string with the (numerical) collection id, this returns the cluster
  /// wide collection name in the DBserver case
  //////////////////////////////////////////////////////////////////////////////
  std::string getCollectionName(std::string_view nameOrId) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief look up a data-source struct for a data-source id
  /// @return the local data-source on dbserver / standalone
  ///         the cluster data-source on coordinator
  //////////////////////////////////////////////////////////////////////////////
  std::shared_ptr<LogicalDataSource> getDataSource(DataSourceId id) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief look up a data-source struct for a
  ///        data-source name, stringified id (or uuid for dbserver/standalone)
  /// @return the local data-source on dbserver / standalone
  ///         the cluster data-source on coordinator
  //////////////////////////////////////////////////////////////////////////////
  std::shared_ptr<LogicalDataSource> getDataSource(
      std::string_view nameOrId) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief look up a view struct for a view id
  /// @return the local view on dbserver / standalone
  ///         the cluster view on coordinator
  //////////////////////////////////////////////////////////////////////////////
  std::shared_ptr<LogicalView> getView(DataSourceId id) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief look up a view struct for a
  ///        view name, stringified id (or uuid for dbserver / standalone)
  /// @return the local view on dbserver / standalone
  ///         the cluster view on coordinator
  //////////////////////////////////////////////////////////////////////////////
  std::shared_ptr<LogicalView> getView(std::string_view nameOrId) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the vocbase instance this resolver instance uses
  //////////////////////////////////////////////////////////////////////////////
  TRI_vocbase_t& vocbase() const noexcept { return _vocbase; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief invoke visitor on all collections that map to the specified 'id'
  /// @return visitation was successful
  //////////////////////////////////////////////////////////////////////////////
  bool visitCollections(std::function<bool(LogicalCollection&)> const& visitor,
                        DataSourceId id) const;

 private:
  std::string lookupName(DataSourceId cid) const;

  /// @brief vocbase base pointer
  TRI_vocbase_t& _vocbase;

  /// @brief role of server in cluster
  ServerState::RoleEnum const _serverRole;

  /// @brief lock protecting caches
  mutable std::shared_mutex _lock;

  /// @brief collection id => collection name map
  mutable containers::FlatHashMap<DataSourceId, std::string> _resolvedIds;

  mutable containers::FlatHashMap<DataSourceId,
                                  std::shared_ptr<LogicalDataSource>>
      _dataSourceById;  // cached data-source by id
  mutable containers::FlatHashMap<std::string,
                                  std::shared_ptr<LogicalDataSource>>
      _dataSourceByName;  // cached data-source by name
};

}  // namespace arangodb
