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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef USE_V8
#error this file is not supposed to be used in builds with -DUSE_V8=Off
#endif

#include "v8-collection.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/FileUtils.h"
#include "Basics/ReadLocker.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/Utf8Helper.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/conversions.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Indexes/Index.h"
#include "RestServer/DatabaseFeature.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Hints.h"
#include "Transaction/V8Context.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/OperationResult.h"
#include "Utils/SingleCollectionTransaction.h"
#include "V8/v8-conv.h"
#include "V8/v8-utils.h"
#include "V8/v8-vpack.h"
#include "V8Server/v8-externals.h"
#include "V8Server/v8-vocbase.h"
#include "V8Server/v8-vocbaseprivate.h"
#include "V8Server/v8-vocindex.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include <absl/strings/escaping.h>

namespace {

////////////////////////////////////////////////////////////////////////////////
/// @brief retrieves a collection from a V8 argument
////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<arangodb::LogicalCollection> GetCollectionFromArgument(
    v8::Isolate* isolate, TRI_vocbase_t& vocbase,
    v8::Handle<v8::Value> const val) {
  if (arangodb::ServerState::instance()->isCoordinator()) {
    return vocbase.server().hasFeature<arangodb::ClusterFeature>()
               ? vocbase.server()
                     .getFeature<arangodb::ClusterFeature>()
                     .clusterInfo()
                     .getCollectionNT(vocbase.name(),
                                      TRI_ObjectToString(isolate, val))
               : nullptr;
  }

  // number
  if (val->IsNumber() || val->IsNumberObject()) {
    arangodb::DataSourceId cid{TRI_ObjectToUInt64(isolate, val, true)};
    return vocbase.lookupCollection(cid);
  }

  return vocbase.lookupCollection(TRI_ObjectToString(isolate, val));
}

void addTransactionHints(arangodb::LogicalCollection& col,
                         arangodb::SingleCollectionTransaction& trx,
                         bool isMultiple, bool isOverwritingInsert) {
  if (arangodb::ServerState::instance()->isCoordinator()) {
    if (col.isSmartEdgeCollection()) {
      // Smart Edge Collections hit multiple shards with dependent requests,
      // they have to be globally managed.
      trx.addHint(arangodb::transaction::Hints::Hint::GLOBAL_MANAGED);
      return;
    }
  }
  // For non multiple operations we can optimize to use SingleOperations.
  if (!isMultiple && !isOverwritingInsert) {
    trx.addHint(arangodb::transaction::Hints::Hint::SINGLE_OPERATION);
  }
}

}  // namespace

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

////////////////////////////////////////////////////////////////////////////////
/// @brief extract a boolean flag from the arguments
/// must specify the argument index starting from 1
////////////////////////////////////////////////////////////////////////////////

static inline bool ExtractBooleanArgument(
    v8::Isolate* isolate, v8::FunctionCallbackInfo<v8::Value> const& args,
    int index, bool defaultVal) {
  TRI_ASSERT(index > 0);

  if (args.Length() >= index) {
    return TRI_ObjectToBoolean(isolate, args[index - 1]);
  } else {
    return defaultVal;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief extracts a string value referencing a documents _id
///        If value is a string it is simply returned.
///        If value is an object and has a string _id attribute, this is
///        returned
///        Otherwise the empty string is returned
////////////////////////////////////////////////////////////////////////////////

static std::string ExtractIdString(v8::Isolate* isolate,
                                   v8::Handle<v8::Value> const val) {
  auto context = TRI_IGETC;
  if (val->IsString()) {
    return TRI_ObjectToString(isolate, val);
  }

  if (val->IsObject()) {
    TRI_GET_GLOBALS();
    v8::Handle<v8::Object> obj = v8::Handle<v8::Object>::Cast(
        val->ToObject(TRI_IGETC).FromMaybe(v8::Local<v8::Value>()));
    TRI_GET_GLOBAL_STRING(_IdKey);
    if (TRI_HasRealNamedProperty(context, isolate, obj, _IdKey)) {
      v8::Handle<v8::Value> idVal =
          obj->Get(context, _IdKey).FromMaybe(v8::Handle<v8::Value>());
      if (idVal->IsString()) {
        return TRI_ObjectToString(isolate, idVal);
      }
    }
  }
  std::string empty;
  return empty;
}

static void getOperationOptionsFromObject(v8::Isolate* isolate,
                                          OperationOptions& options,
                                          v8::Handle<v8::Object>& optionsObject,
                                          bool getUpdateFlags = false) {
  auto context = TRI_IGETC;
  TRI_GET_GLOBALS();
  TRI_GET_GLOBAL_STRING(OverwriteKey);
  if (TRI_HasProperty(context, isolate, optionsObject, OverwriteKey)) {
    options.ignoreRevs =
        TRI_ObjectToBoolean(isolate, optionsObject->Get(context, OverwriteKey)
                                         .FromMaybe(v8::Local<v8::Value>()));
  }
  TRI_GET_GLOBAL_STRING(WaitForSyncKey);
  if (TRI_HasProperty(context, isolate, optionsObject, WaitForSyncKey)) {
    options.waitForSync =
        TRI_ObjectToBoolean(isolate, optionsObject->Get(context, WaitForSyncKey)
                                         .FromMaybe(v8::Local<v8::Value>()));
  }
  TRI_GET_GLOBAL_STRING(ReturnNewKey);
  if (TRI_HasProperty(context, isolate, optionsObject, ReturnNewKey)) {
    options.returnNew =
        TRI_ObjectToBoolean(isolate, optionsObject->Get(context, ReturnNewKey)
                                         .FromMaybe(v8::Local<v8::Value>()));
  }
  TRI_GET_GLOBAL_STRING(ReturnOldKey);
  if (TRI_HasProperty(context, isolate, optionsObject, ReturnOldKey)) {
    options.returnOld =
        TRI_ObjectToBoolean(isolate, optionsObject->Get(context, ReturnOldKey)
                                         .FromMaybe(v8::Local<v8::Value>()));
  }
  TRI_GET_GLOBAL_STRING(SilentKey);
  if (TRI_HasProperty(context, isolate, optionsObject, SilentKey)) {
    options.silent =
        TRI_ObjectToBoolean(isolate, optionsObject->Get(context, SilentKey)
                                         .FromMaybe(v8::Local<v8::Value>()));
  }
  TRI_GET_GLOBAL_STRING(RefillIndexCachesKey);
  // this attribute can have 3 values: default, true and false. only
  // pick it up when it is set to true or false
  if (TRI_HasProperty(context, isolate, optionsObject, RefillIndexCachesKey)) {
    options.refillIndexCaches =
        (TRI_ObjectToBoolean(isolate,
                             optionsObject->Get(context, RefillIndexCachesKey)
                                 .FromMaybe(v8::Local<v8::Value>())))
            ? RefillIndexCaches::kRefill
            : RefillIndexCaches::kDontRefill;
  }
  TRI_GET_GLOBAL_STRING(IsSynchronousReplicationKey);
  if (TRI_HasProperty(context, isolate, optionsObject,
                      IsSynchronousReplicationKey)) {
    options.isSynchronousReplicationFrom = TRI_ObjectToString(
        isolate, optionsObject->Get(context, IsSynchronousReplicationKey)
                     .FromMaybe(v8::Local<v8::Value>()));
  }
  TRI_GET_GLOBAL_STRING(CompactKey);
  if (TRI_HasProperty(context, isolate, optionsObject, CompactKey)) {
    options.truncateCompact =
        TRI_ObjectToBoolean(isolate, optionsObject->Get(context, CompactKey)
                                         .FromMaybe(v8::Local<v8::Value>()));
  }
  TRI_GET_GLOBAL_STRING(SkipDocumentValidationKey);
  if (TRI_HasProperty(context, isolate, optionsObject,
                      SkipDocumentValidationKey)) {
    options.validate = !TRI_ObjectToBoolean(
        isolate, optionsObject->Get(context, SkipDocumentValidationKey)
                     .FromMaybe(v8::Local<v8::Value>()));
  }
  TRI_GET_GLOBAL_STRING(IsRestoreKey);
  if (TRI_HasProperty(context, isolate, optionsObject, IsRestoreKey)) {
    options.isRestore =
        TRI_ObjectToBoolean(isolate, optionsObject->Get(context, IsRestoreKey)
                                         .FromMaybe(v8::Local<v8::Value>()));
  }
  if (getUpdateFlags) {
    // intentionally not called for TRI_VOC_DOCUMENT_OPERATION_REPLACE
    TRI_GET_GLOBAL_STRING(KeepNullKey);
    if (TRI_HasProperty(context, isolate, optionsObject, KeepNullKey)) {
      options.keepNull =
          TRI_ObjectToBoolean(isolate, optionsObject->Get(context, KeepNullKey)
                                           .FromMaybe(v8::Local<v8::Value>()));
    }
    TRI_GET_GLOBAL_STRING(MergeObjectsKey);
    if (TRI_HasProperty(context, isolate, optionsObject, MergeObjectsKey)) {
      options.mergeObjects = TRI_ObjectToBoolean(
          isolate, optionsObject->Get(context, MergeObjectsKey)
                       .FromMaybe(v8::Local<v8::Value>()));
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief parse document or document handle from a v8 value (string | object)
////////////////////////////////////////////////////////////////////////////////

static ErrorCode ParseDocumentOrDocumentHandle(
    v8::Isolate* isolate, CollectionNameResolver const* resolver,
    bool extendedNames,
    std::shared_ptr<arangodb::LogicalCollection>& collection,
    std::string& collectionName, VPackBuilder& builder, bool includeRev,
    v8::Handle<v8::Value> const val) {
  v8::HandleScope scope(isolate);

  // try to extract the collection name, key, and revision from the object
  // passed
  if (!ExtractDocumentHandle(isolate, val, extendedNames, collectionName,
                             builder, includeRev)) {
    return TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD;
  }

  if (collectionName.empty()) {
    // only a document key without collection name was passed
    if (collection == nullptr) {
      // we do not know the collection
      return TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD;
    }
    // we use the current collection's name
    collectionName = resolver->getCollectionNameCluster(collection->id());
  } else {
    // we read a collection name from the document id
    // check cross-collection requests
    if (collection != nullptr && !methods::Collections::hasName(
                                     *resolver, *collection, collectionName)) {
      return TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST;
    }
  }

  TRI_ASSERT(!collectionName.empty());

  if (collection == nullptr) {
    // no collection object was passed, now check the user-supplied collection
    // name
    collection = resolver->getCollection(collectionName);

    if (collection == nullptr) {
      // collection not found
      return TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND;
    }
  }
  TRI_ASSERT(collection != nullptr);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief V8ToVPack without _key and _rev, builder must be open with an
/// object and is left open at the end
////////////////////////////////////////////////////////////////////////////////

static ErrorCode V8ToVPackNoKeyRevId(v8::Isolate* isolate,
                                     VPackBuilder& builder,
                                     v8::Local<v8::Value> const obj) {
  auto context = TRI_IGETC;
  TRI_ASSERT(obj->IsObject() && !obj->IsArray());
  auto o = v8::Local<v8::Object>::Cast(obj);
  v8::Handle<v8::Array> names =
      o->GetOwnPropertyNames(context).FromMaybe(v8::Local<v8::Array>());
  uint32_t const n = names->Length();
  for (uint32_t i = 0; i < n; ++i) {
    v8::Handle<v8::Value> key =
        names->Get(context, i).FromMaybe(v8::Handle<v8::Value>());
    TRI_Utf8ValueNFC str(isolate, key);
    if (*str == nullptr) {
      return TRI_ERROR_OUT_OF_MEMORY;
    }
    if (strcmp(*str, "_key") != 0 && strcmp(*str, "_rev") != 0 &&
        strcmp(*str, "_id") != 0) {
      builder.add(VPackValue(*str));
      TRI_V8ToVPack(isolate, builder,
                    o->Get(context, key).FromMaybe(v8::Local<v8::Value>()),
                    false);
    }
  }
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get all cluster collections cloned, caller needs to cleanupb
////////////////////////////////////////////////////////////////////////////////

std::vector<std::shared_ptr<LogicalCollection>> GetCollections(
    TRI_vocbase_t& vocbase) {
  if (arangodb::ServerState::instance()->isCoordinator()) {
    return vocbase.server().hasFeature<arangodb::ClusterFeature>()
               ? vocbase.server()
                     .getFeature<arangodb::ClusterFeature>()
                     .clusterInfo()
                     .getCollections(vocbase.name())
               : std::vector<std::shared_ptr<LogicalCollection>>();
  }

  return vocbase.collections(false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get all cluster collection names
////////////////////////////////////////////////////////////////////////////////

static std::vector<std::string> GetCollectionNamesCluster(
    TRI_vocbase_t* vocbase) {
  std::vector<std::string> result;

  std::vector<std::shared_ptr<LogicalCollection>> const collections =
      vocbase->server()
          .getFeature<arangodb::ClusterFeature>()
          .clusterInfo()
          .getCollections(vocbase->name());

  for (auto& collection : collections) {
    std::string const& name = collection->name();
    result.emplace_back(name);
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up a document and returns whether it exists
////////////////////////////////////////////////////////////////////////////////

static void ExistsVocbaseVPack(
    bool useCollection, v8::FunctionCallbackInfo<v8::Value> const& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  // first and only argument should be a document identifier
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("exists(<document-id> or <document-key> )");
  }

  TRI_vocbase_t* vocbase;
  arangodb::LogicalCollection* col = nullptr;

  if (useCollection) {
    // called as db.collection.exists()
    col = UnwrapCollection(isolate, args.Holder());

    if (!col) {
      TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
    }

    vocbase = &(col->vocbase());
  } else {
    // called as db._exists()
    vocbase = &GetContextVocBase(isolate);
  }

  bool extendedNames =
      vocbase->server().getFeature<DatabaseFeature>().extendedNames();

  auto origin = transaction::OperationOriginREST{"checking document existence"};
  transaction::V8Context transactionContext(*vocbase, origin, true);
  VPackBuilder builder;
  std::shared_ptr<arangodb::LogicalCollection> collection(
      col, [](arangodb::LogicalCollection*) -> void {});
  std::string collectionName;
  Result res;

  {
    VPackObjectBuilder guard(&builder);

    res = ParseDocumentOrDocumentHandle(
        isolate, &(transactionContext.resolver()), extendedNames, collection,
        collectionName, builder, true, args[0]);
  }

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_ASSERT(collection);
  TRI_ASSERT(!collectionName.empty());
  VPackSlice search = builder.slice();
  TRI_ASSERT(search.isObject());

  SingleCollectionTransaction trx(
      std::shared_ptr<transaction::Context>(
          std::shared_ptr<transaction::Context>(), &transactionContext),
      collectionName, AccessMode::Type::READ);
  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  res = trx.begin();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  OperationOptions options;
  options.silent = false;
  options.ignoreRevs = false;
  OperationResult opResult = trx.document(collectionName, search, options);

  res = trx.finish(opResult.result);

  if (opResult.fail()) {
    if (opResult.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND)) {
      TRI_V8_RETURN_FALSE();
    }
    TRI_V8_THROW_EXCEPTION(opResult.result);
  }

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Value> result = TRI_VPackToV8(
      isolate, opResult.slice(), transactionContext.getVPackOptions());

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up (a) document(s) and returns it/them, collection method
////////////////////////////////////////////////////////////////////////////////

static void DocumentVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  TRI_GET_GLOBALS();

  // first and only argument should be a document handle or key or an object
  if (args.Length() < 1 || args.Length() > 2) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "document(<document-id> or <document-key> or <object> or <array>)");
  }

  OperationOptions options;
  options.ignoreRevs = false;

  if (args.Length() == 2 && args[1]->IsObject()) {
    v8::Local<v8::Object> optsObj = v8::Local<v8::Object>::Cast(args[1]);
    TRI_GET_GLOBAL_STRING(AllowDirtyReadsKey);
    if (TRI_HasProperty(context, isolate, optsObj, AllowDirtyReadsKey)) {
      options.allowDirtyReads =
          TRI_ObjectToBoolean(isolate, optsObj->Get(context, AllowDirtyReadsKey)
                                           .FromMaybe(v8::Local<v8::Value>()));
    }
  }

  // Find collection and vocbase
  auto* col = UnwrapCollection(isolate, args.Holder());

  if (!col) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  bool extendedNames =
      col->vocbase().server().getFeature<DatabaseFeature>().extendedNames();

  auto& collectionName = col->name();
  VPackBuilder searchBuilder;

  auto workOnOneDocument = [&](v8::Local<v8::Value> const searchValue,
                               bool isBabies) {
    std::string collName;
    if (!ExtractDocumentHandle(isolate, searchValue, extendedNames, collName,
                               searchBuilder, true)) {
      if (!isBabies) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD);
      }
    }
    if (!collName.empty() && collName != collectionName) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST);
    }
  };

  if (!args[0]->IsArray()) {
    VPackObjectBuilder guard(&searchBuilder);
    workOnOneDocument(args[0], false);
  } else {
    VPackArrayBuilder guard(&searchBuilder);
    auto searchVals = v8::Local<v8::Array>::Cast(args[0]);
    for (uint32_t i = 0; i < searchVals->Length(); ++i) {
      VPackObjectBuilder guard(&searchBuilder);
      workOnOneDocument(
          searchVals->Get(context, i).FromMaybe(v8::Local<v8::Value>()), true);
    }
  }

  VPackSlice search = searchBuilder.slice();
  auto origin = transaction::OperationOriginREST{"reading document(s)"};
  transaction::V8Context transactionContext(col->vocbase(), origin, true);
  SingleCollectionTransaction trx(
      std::shared_ptr<transaction::Context>(
          std::shared_ptr<transaction::Context>(), &transactionContext),
      collectionName, AccessMode::Type::READ);

  if (!args[0]->IsArray()) {
    trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);
  }

  Result res = trx.begin();
  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  OperationResult opResult = trx.document(collectionName, search, options);

  res = trx.finish(opResult.result);

  if (opResult.fail()) {
    TRI_V8_THROW_EXCEPTION(opResult.result);
  }

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Value> result = TRI_VPackToV8(
      isolate, opResult.slice(), transactionContext.getVPackOptions());

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up a document and returns it, database method
////////////////////////////////////////////////////////////////////////////////

static void DocumentVocbase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  // first and only argument should be a document identifier
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("document(<document-id>)");
  }

  auto& vocbase = GetContextVocBase(isolate);

  if (vocbase.isDropped()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  bool extendedNames =
      vocbase.server().getFeature<DatabaseFeature>().extendedNames();

  auto origin = transaction::OperationOriginREST{"reading document(s)"};
  transaction::V8Context transactionContext(vocbase, origin, true);
  VPackBuilder builder;
  std::shared_ptr<arangodb::LogicalCollection> collection;
  std::string collectionName;

  {
    VPackObjectBuilder guard(&builder);
    auto res = ParseDocumentOrDocumentHandle(
        isolate, &(transactionContext.resolver()), extendedNames, collection,
        collectionName, builder, true, args[0]);

    if (res != TRI_ERROR_NO_ERROR) {
      TRI_V8_THROW_EXCEPTION(res);
    }
  }

  TRI_ASSERT(collection);
  TRI_ASSERT(!collectionName.empty());

  VPackSlice search = builder.slice();
  TRI_ASSERT(search.isObject());

  OperationOptions options;
  options.ignoreRevs = false;

  SingleCollectionTransaction trx(
      std::shared_ptr<transaction::Context>(
          std::shared_ptr<transaction::Context>(), &transactionContext),
      collectionName, AccessMode::Type::READ);
  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  Result res = trx.begin();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  OperationResult opResult = trx.document(collectionName, search, options);

  res = trx.finish(opResult.result);

  if (opResult.fail()) {
    TRI_V8_THROW_EXCEPTION(opResult.result);
  }

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Value> result = TRI_VPackToV8(
      isolate, opResult.slice(), transactionContext.getVPackOptions());

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief deletes (a) document(s), collection method
////////////////////////////////////////////////////////////////////////////////

static void RemoveVocbaseCol(v8::FunctionCallbackInfo<v8::Value> const& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  OperationOptions options;
  options.ignoreRevs = false;

  // check the arguments
  uint32_t const argLength = args.Length();

  if (argLength < 1 || argLength > 3) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "remove(<document>, "
        "{overwrite: booleanValue, waitForSync: booleanValue, returnOld: "
        "booleanValue, silent:booleanValue})");
  }

  if (argLength > 1) {
    if (args[1]->IsObject()) {
      v8::Handle<v8::Object> optionsObject = args[1].As<v8::Object>();
      getOperationOptionsFromObject(isolate, options, optionsObject);
    } else {  // old variant remove(<document>, <overwrite>, <waitForSync>)
      options.ignoreRevs = TRI_ObjectToBoolean(isolate, args[1]);
      if (argLength > 2) {
        options.waitForSync = TRI_ObjectToBoolean(isolate, args[2]);
      }
    }
  }

  // Find collection and vocbase
  auto* col = UnwrapCollection(isolate, args.Holder());

  if (!col) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  bool extendedNames =
      col->vocbase().server().getFeature<DatabaseFeature>().extendedNames();

  auto& collectionName = col->name();
  VPackBuilder searchBuilder;

  auto workOnOneDocument = [&](v8::Local<v8::Value> const searchValue,
                               bool isBabies) {
    std::string collName;
    if (!ExtractDocumentHandle(isolate, searchValue, extendedNames, collName,
                               searchBuilder, true)) {
      if (!isBabies) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD);
      }
      return;
    }
    if (!collName.empty() && collName != collectionName) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST);
    }
  };

  if (!args[0]->IsArray()) {
    VPackObjectBuilder guard(&searchBuilder);
    workOnOneDocument(args[0], false);
  } else {
    VPackArrayBuilder guard(&searchBuilder);
    auto searchVals = v8::Local<v8::Array>::Cast(args[0]);
    for (uint32_t i = 0; i < searchVals->Length(); ++i) {
      VPackObjectBuilder guard(&searchBuilder);
      workOnOneDocument(
          searchVals->Get(context, i).FromMaybe(v8::Local<v8::Value>()), true);
    }
  }

  bool payloadIsArray = args[0]->IsArray();
  transaction::Options trxOpts;
  trxOpts.delaySnapshot = !payloadIsArray;  // for now we only enable this for
                                            // single document operations

  VPackSlice toRemove = searchBuilder.slice();
  auto origin = transaction::OperationOriginREST{"removing document(s)"};
  transaction::V8Context transactionContext(col->vocbase(), origin, true);
  SingleCollectionTransaction trx(
      std::shared_ptr<transaction::V8Context>(
          std::shared_ptr<transaction::Context>(), &transactionContext),
      collectionName, AccessMode::Type::WRITE, trxOpts);

  ::addTransactionHints(*col, trx, payloadIsArray, false);

  Result res = trx.begin();
  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  OperationResult result = trx.remove(collectionName, toRemove, options);

  res = trx.finish(result.result);

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  if (options.silent) {
    // no return value
    TRI_V8_RETURN_TRUE();
  }

  v8::Handle<v8::Value> finalResult = TRI_VPackToV8(
      isolate, result.slice(), transactionContext.getVPackOptions());

  TRI_V8_RETURN(finalResult);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief deletes a document, database method
////////////////////////////////////////////////////////////////////////////////

static void RemoveVocbase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  OperationOptions options;
  options.ignoreRevs = false;

  // check the arguments
  uint32_t const argLength = args.Length();

  if (argLength < 1 || argLength > 3) {
    TRI_V8_THROW_EXCEPTION_USAGE("remove(<document>, <options>)");
  }

  if (argLength > 1) {
    if (args[1]->IsObject()) {
      v8::Handle<v8::Object> optionsObject = args[1].As<v8::Object>();
      getOperationOptionsFromObject(isolate, options, optionsObject);
    } else {  // old variant replace(<document>, <data>, <overwrite>,
              // <waitForSync>)
      options.ignoreRevs = TRI_ObjectToBoolean(isolate, args[1]);
      if (argLength > 2) {
        options.waitForSync = TRI_ObjectToBoolean(isolate, args[2]);
      }
    }
  }

  auto& vocbase = GetContextVocBase(isolate);

  if (vocbase.isDropped()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  bool extendedNames =
      vocbase.server().getFeature<DatabaseFeature>().extendedNames();

  auto origin = transaction::OperationOriginREST{"removing document(s)"};
  ;
  transaction::V8Context transactionContext(vocbase, origin, true);
  VPackBuilder builder;
  std::shared_ptr<arangodb::LogicalCollection> collection;
  std::string collectionName;

  {
    VPackObjectBuilder guard(&builder);
    auto res = ParseDocumentOrDocumentHandle(
        isolate, &(transactionContext.resolver()), extendedNames, collection,
        collectionName, builder, !options.ignoreRevs, args[0]);

    if (res != TRI_ERROR_NO_ERROR) {
      TRI_V8_THROW_EXCEPTION(res);
    }
  }

  TRI_ASSERT(collection);
  TRI_ASSERT(!collectionName.empty());

  VPackSlice toRemove = builder.slice();
  TRI_ASSERT(toRemove.isObject());

  transaction::Options trxOpts;
  trxOpts.delaySnapshot = true;

  SingleCollectionTransaction trx(
      std::shared_ptr<transaction::Context>(
          std::shared_ptr<transaction::Context>(), &transactionContext),
      collectionName, AccessMode::Type::WRITE, trxOpts);
  ::addTransactionHints(*collection, trx, false, false);

  Result res = trx.begin();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  OperationResult result = trx.remove(collectionName, toRemove, options);

  res = trx.finish(result.result);

  if (result.fail()) {
    TRI_V8_THROW_EXCEPTION(result.result);
  }

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  if (options.silent) {
    // no return value
    TRI_V8_RETURN_TRUE();
  }

  v8::Handle<v8::Value> finalResult = TRI_VPackToV8(
      isolate, result.slice(), transactionContext.getVPackOptions());

  TRI_V8_RETURN(finalResult);
}

// db.<collection>.document
static void JS_DocumentVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  DocumentVocbaseCol(args);
  TRI_V8_TRY_CATCH_END
}

// db.<collection>.binaryDocument
static void JS_BinaryDocumentVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // first and only argument should be a document handle or key
  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "binaryDocument(<document-id> or <document-key>, <filename>)");
  }

  OperationOptions options;
  options.ignoreRevs = false;

  // Find collection and vocbase
  auto* col = UnwrapCollection(isolate, args.Holder());

  if (!col) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  bool extendedNames =
      col->vocbase().server().getFeature<DatabaseFeature>().extendedNames();

  VPackBuilder searchBuilder;
  v8::Local<v8::Value> const searchValue = args[0];
  auto& collectionName = col->name();

  {
    VPackObjectBuilder guard(&searchBuilder);

    std::string collName;
    if (!ExtractDocumentHandle(isolate, searchValue, extendedNames, collName,
                               searchBuilder, true)) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD);
    }

    if (!collName.empty() && collName != collectionName) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST);
    }
  }

  VPackSlice search = searchBuilder.slice();
  auto origin = transaction::OperationOriginREST{"storing binary document"};
  transaction::V8Context transactionContext(col->vocbase(), origin, true);
  SingleCollectionTransaction trx(
      std::shared_ptr<transaction::Context>(
          std::shared_ptr<transaction::Context>(), &transactionContext),
      collectionName, AccessMode::Type::READ);

  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  Result res = trx.begin();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  OperationResult opResult = trx.document(collectionName, search, options);

  res = trx.finish(opResult.result);

  if (opResult.fail()) {
    TRI_V8_THROW_EXCEPTION(opResult.result);
  }

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  std::string filename = TRI_ObjectToString(isolate, args[1]);
  auto builder = std::make_shared<VPackBuilder>();

  {
    VPackObjectBuilder meta(builder.get());

    for (auto it : VPackObjectIterator(opResult.slice().resolveExternals())) {
      std::string key = it.key.copyString();

      if (key == StaticStrings::AttachmentString) {
        std::string_view value;

        try {
          value = it.value.stringView();
        } catch (...) {
          TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID,
                                         "'_attachment' must be a string");
        }

        std::string attachment;
        absl::Base64Unescape(value, &attachment);

        try {
          FileUtils::spit(filename, attachment);
        } catch (...) {
          TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), TRI_last_error());
        }
      } else {
        builder->add(key, it.value);
      }
    }
  }

  v8::Handle<v8::Value> result = TRI_VPackToV8(
      isolate, builder->slice(), transactionContext.getVPackOptions());

  TRI_V8_RETURN(result);

  TRI_V8_TRY_CATCH_END
}

static void JS_DropVocbaseCol(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::HandleScope scope(isolate);
  auto& vocbase = GetContextVocBase(isolate);
  if (vocbase.isDangling()) {
    events::DropCollection(vocbase.name(), "",
                           TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    events::DropCollection(vocbase.name(), "", TRI_ERROR_INTERNAL);
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  PREVENT_EMBEDDED_TRANSACTION();

  bool allowDropSystem = false;

  if (args.Length() > 0) {
    // options
    if (args[0]->IsObject()) {
      TRI_GET_GLOBALS();
      v8::Handle<v8::Object> optionsObject = args[0].As<v8::Object>();
      TRI_GET_GLOBAL_STRING(IsSystemKey);
      if (TRI_HasProperty(context, isolate, optionsObject, IsSystemKey)) {
        allowDropSystem = TRI_ObjectToBoolean(
            isolate, optionsObject->Get(context, IsSystemKey)
                         .FromMaybe(v8::Local<v8::Value>()));
      }
    } else {
      allowDropSystem = TRI_ObjectToBoolean(isolate, args[0]);
    }
  }

  try {
    auto res = methods::Collections::drop(*collection, allowDropSystem);
    if (res.fail()) {
      TRI_V8_THROW_EXCEPTION(res);
    }
  } catch (basics::Exception const& ex) {
    events::DropCollection(vocbase.name(), collection->name(), ex.code());
    throw;
  } catch (...) {
    events::DropCollection(vocbase.name(), collection->name(),
                           TRI_ERROR_INTERNAL);
    throw;
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

static void JS_ExistsVocbaseVPack(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  return ExistsVocbaseVPack(true, args);

  // cppcheck-suppress style
  TRI_V8_TRY_CATCH_END
}

static void JS_FiguresVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  bool details = false;
  if (args.Length() != 0) {
    details = TRI_ObjectToBoolean(isolate, args[0]);
  }

  auto origin = transaction::OperationOriginREST{"extracing figures"};
  SingleCollectionTransaction trx(
      transaction::V8Context::create(collection->vocbase(), origin, true),
      *collection, AccessMode::Type::READ);
  Result res = trx.begin();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  OperationOptions options(ExecContext::current());
  auto opRes = collection->figures(details, options).get();

  if (trx.finish(opRes.result).ok()) {
    TRI_V8_RETURN(TRI_VPackToV8(isolate, opRes.slice()));
  } else {
    TRI_V8_RETURN_NULL();
  }

  TRI_V8_TRY_CATCH_END
}

static void JS_GetResponsibleShardVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (!ServerState::instance()->isCoordinator()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_CLUSTER_ONLY_ON_COORDINATOR);
  }

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }
  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("getResponsibleShard(<data>)");
  }

  VPackBuilder builder;
  if (args[0]->IsNumber() || args[0]->IsNumberObject()) {
    builder.openObject();
    builder.add(
        StaticStrings::KeyString,
        VPackValue(std::to_string(TRI_ObjectToInt64(isolate, args[0]))));
    builder.close();
  } else if (args[0]->IsString() || args[0]->IsStringObject()) {
    builder.openObject();
    builder.add(StaticStrings::KeyString,
                VPackValue(TRI_ObjectToString(isolate, args[0])));
    builder.close();
  } else {
    TRI_V8ToVPack(isolate, builder, args[0], false);
  }
  if (!builder.slice().isObject()) {
    TRI_V8_THROW_EXCEPTION_USAGE("getResponsibleShard(<object>)");
  }

  TRI_ASSERT(builder.slice().isObject());
  auto maybeShard = collection->getResponsibleShard(builder.slice(), false);

  if (maybeShard.fail()) {
    TRI_V8_THROW_EXCEPTION(maybeShard.result());
  }

  v8::Handle<v8::Value> result =
      TRI_V8_STD_STRING(isolate, std::string{maybeShard.get()});
  TRI_V8_RETURN(result);

  TRI_V8_TRY_CATCH_END
}

static void JS_LoadVocbaseCol(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto& vocbase = GetContextVocBase(isolate);

  if (vocbase.isDropped()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  // load is a no-op starting with ArangoDB 3.9
  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the name of a collection
////////////////////////////////////////////////////////////////////////////////

static void JS_NameVocbaseCol(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  auto& collectionName = collection->name();

  if (collectionName.empty()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }
  v8::Handle<v8::Value> result = TRI_V8_STD_STRING(isolate, collectionName);
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the path of a collection
////////////////////////////////////////////////////////////////////////////////

static void JS_PathVocbaseCol(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  // always empty
  std::string path;
  v8::Handle<v8::Value> result = TRI_V8_STD_STRING(isolate, path);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection's cluster plan id
////////////////////////////////////////////////////////////////////////////////

static void JS_PlanIdVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (ServerState::instance()->isCoordinator()) {
    TRI_V8_RETURN(TRI_V8UInt64String<DataSourceId::BaseType>(
        isolate, collection->id().id()));
  }

  TRI_V8_RETURN(TRI_V8UInt64String<DataSourceId::BaseType>(
      isolate, collection->planId().id()));
  TRI_V8_TRY_CATCH_END
}

static void JS_PropertiesVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  auto* consoleColl = UnwrapCollection(isolate, args.Holder());

  if (!consoleColl) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  bool const isModification = (args.Length() != 0);

  if (isModification) {
    v8::Handle<v8::Value> par = args[0];

    if (par->IsObject()) {
      VPackBuilder builder;
      TRI_V8ToVPack(isolate, builder, args[0], false);
      TRI_ASSERT(builder.isClosed());
      OperationOptions options(ExecContext::current());

      auto res = methods::Collections::updateProperties(
                     *consoleColl, builder.slice(), options)
                     .get();
      if (res.fail() && ServerState::instance()->isCoordinator()) {
        TRI_V8_THROW_EXCEPTION(res);
      }
    }
  }

  // in the cluster the collection object might contain outdated
  // properties, which will break tests. We need an extra lookup
  VPackBuilder builder;

  std::shared_ptr<LogicalCollection> coll;
  methods::Collections::lookup(consoleColl->vocbase(), consoleColl->name(),
                               coll);
  if (coll) {
    VPackObjectBuilder object(&builder, true);
    methods::Collections::Context ctxt(coll);
    Result res = methods::Collections::properties(ctxt, builder).get();

    if (res.fail()) {
      TRI_V8_THROW_EXCEPTION(res);
    }
  }

  // return the current parameter set
  TRI_V8_RETURN(TRI_VPackToV8(isolate, builder.slice())
                    ->ToObject(context)
                    .FromMaybe(v8::Local<v8::Value>()));
  TRI_V8_TRY_CATCH_END
}

static void JS_RemoveVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  RemoveVocbaseCol(args);
  // cppcheck-suppress style
  TRI_V8_TRY_CATCH_END
}

static void JS_RenameVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("rename(<name>)");
  }

  std::string const name = TRI_ObjectToString(isolate, args[0]);

  // second parameter "override" is to override renaming restrictions, e.g.
  // renaming from a system collection name to a non-system collection name and
  // vice versa. this parameter is not publicly exposed but used internally
  bool doOverride = false;
  if (args.Length() > 1) {
    doOverride = TRI_ObjectToBoolean(isolate, args[1]);
  }

  PREVENT_EMBEDDED_TRANSACTION();

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  auto res = methods::Collections::rename(*collection, name, doOverride);

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }
  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief option parsing for replace and update methods
////////////////////////////////////////////////////////////////////////////////

static void parseReplaceAndUpdateOptions(
    v8::Isolate* isolate, v8::FunctionCallbackInfo<v8::Value> const& args,
    OperationOptions& options, TRI_voc_document_operation_e operation) {
  TRI_ASSERT(args.Length() > 2);
  if (args[2]->IsObject()) {
    v8::Handle<v8::Object> optionsObject = args[2].As<v8::Object>();
    getOperationOptionsFromObject(
        isolate, options, optionsObject,
        operation == TRI_VOC_DOCUMENT_OPERATION_UPDATE);
  } else {
    // old variants
    //   replace(<document>, <data>, <overwrite>, <waitForSync>)
    // and
    //   update(<document>, <data>, <overwrite>, <keepNull>, <waitForSync>
    options.ignoreRevs = TRI_ObjectToBoolean(isolate, args[2]);
    if (args.Length() > 3) {
      if (operation == TRI_VOC_DOCUMENT_OPERATION_REPLACE) {
        options.waitForSync = TRI_ObjectToBoolean(isolate, args[3]);
      } else {  // UPDATE
        options.keepNull = TRI_ObjectToBoolean(isolate, args[3]);
        if (args.Length() > 4) {
          options.waitForSync = TRI_ObjectToBoolean(isolate, args[4]);
        }
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ModifyVocbaseCol
////////////////////////////////////////////////////////////////////////////////

static void ModifyVocbaseCol(TRI_voc_document_operation_e operation,
                             v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = isolate->GetCurrentContext();

  // check the arguments
  uint32_t const argLength = args.Length();

  if (argLength < 2 ||
      argLength >
          (operation == TRI_VOC_DOCUMENT_OPERATION_REPLACE ? 4UL : 5UL)) {
    if (operation == TRI_VOC_DOCUMENT_OPERATION_REPLACE) {
      TRI_V8_THROW_EXCEPTION_USAGE(
          "replace(<document(s)>, <data>, {overwrite: booleanValue,"
          " waitForSync: booleanValue, returnNew: booleanValue,"
          " returnOld: booleanValue, silent: booleanValue})");
    } else {  // UPDATE
      TRI_V8_THROW_EXCEPTION_USAGE(
          "update(<document>, <data>, {overwrite: booleanValue, keepNull: "
          "booleanValue, mergeObjects: booleanValue, waitForSync: "
          "booleanValue, returnNew: booleanValue, returnOld: booleanValue,"
          " silent: booleanValue})");
    }
  }

  // we're only accepting "real" object documents or arrays of such
  if (!args[1]->IsObject()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }
  if (args[0]->IsArray() ^ args[1]->IsArray()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }
  if (args[0]->IsArray()) {  // then both are arrays, check equal length
    auto a = v8::Local<v8::Array>::Cast(args[0]);
    auto b = v8::Local<v8::Array>::Cast(args[1]);
    if (a->Length() != b->Length()) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
    }
  }

  OperationOptions options;
  options.ignoreRevs = false;
  if (args.Length() > 2) {
    parseReplaceAndUpdateOptions(isolate, args, options, operation);
  }
  if (options.isRestore) {
    options.ignoreRevs = true;
  }

  // Find collection and vocbase
  auto* col = UnwrapCollection(isolate, args.Holder());

  if (!col) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  bool extendedNames =
      col->vocbase().server().getFeature<DatabaseFeature>().extendedNames();

  auto& collectionName = col->name();
  VPackBuilder updateBuilder;
  auto workOnOneSearchVal = [&](v8::Local<v8::Value> const searchVal,
                                bool isBabies) {
    std::string collName;

    if (!ExtractDocumentHandle(isolate, searchVal, extendedNames, collName,
                               updateBuilder, !options.isRestore)) {
      // If this is no restore, then we must extract the _rev from the
      // search value. If options.isRestore is set, the _rev value must
      // be taken from the new value, see below in workOnOneDocument!
      if (!isBabies) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD);
      } else {
        return;
      }
    }

    if (!collName.empty() && collName != collectionName) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST);
    }
  };

  auto workOnOneDocument = [&](v8::Local<v8::Value> const newVal) {
    if (!newVal->IsObject() || newVal->IsArray()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
    }

    auto res = V8ToVPackNoKeyRevId(isolate, updateBuilder, newVal);

    if (res != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(res);
    }

    if (options.isRestore) {
      // In this case we have to extract the _rev entry from newVal:
      TRI_GET_GLOBALS();
      v8::Handle<v8::Object> obj =
          newVal->ToObject(context).FromMaybe(v8::Local<v8::Object>());
      TRI_GET_GLOBAL_STRING(_RevKey);
      if (!TRI_HasRealNamedProperty(context, isolate, obj, _RevKey)) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_REV_BAD);
      }
      v8::Handle<v8::Value> revVal =
          obj->Get(context, _RevKey).FromMaybe(v8::Local<v8::Value>());
      if (!revVal->IsString()) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_REV_BAD);
      }
      v8::String::Utf8Value str(isolate, revVal);
      if (*str == nullptr) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_REV_BAD);
      }
      updateBuilder.add(StaticStrings::RevString, VPackValue(*str));
    }
  };

  if (!args[0]->IsArray()) {
    // we deal with the single document case:
    VPackObjectBuilder guard(&updateBuilder);
    workOnOneDocument(args[1]);
    workOnOneSearchVal(args[0], false);
  } else {  // finally, the array case, note that we already know that the two
            // arrays have equal length!
    TRI_ASSERT(args[0]->IsArray() && args[1]->IsArray());
    VPackArrayBuilder guard(&updateBuilder);
    auto searchVals = v8::Local<v8::Array>::Cast(args[0]);
    auto documents = v8::Local<v8::Array>::Cast(args[1]);
    for (uint32_t i = 0; i < searchVals->Length(); ++i) {
      v8::Local<v8::Value> const newVal =
          documents->Get(context, i).FromMaybe(v8::Local<v8::Value>());
      if (!newVal->IsObject() || newVal->IsArray()) {
        // We insert a non-object that should fail later.
        updateBuilder.add(VPackValue(VPackValueType::Null));
        continue;
      }
      VPackObjectBuilder guard(&updateBuilder);
      workOnOneDocument(newVal);
      workOnOneSearchVal(
          searchVals->Get(context, i).FromMaybe(v8::Local<v8::Value>()), true);
    }
  }

  VPackSlice update = updateBuilder.slice();
  auto origin = transaction::OperationOriginREST{"modifying document(s)"};
  transaction::V8Context transactionContext(col->vocbase(), origin, true);

  bool payloadIsArray = args[0]->IsArray();
  transaction::Options trxOpts;
  trxOpts.delaySnapshot = !payloadIsArray;  // for now we only enable this for
                                            // single document operations

  // Now start the transaction:
  SingleCollectionTransaction trx(
      std::shared_ptr<transaction::Context>(
          std::shared_ptr<transaction::Context>(), &transactionContext),
      collectionName, AccessMode::Type::WRITE, trxOpts);

  addTransactionHints(*col, trx, payloadIsArray, false);

  Result res = trx.begin();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  OperationResult opResult = (operation == TRI_VOC_DOCUMENT_OPERATION_REPLACE)
                                 ? trx.replace(collectionName, update, options)
                                 : trx.update(collectionName, update, options);
  res = trx.finish(opResult.result);

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  if (options.silent) {
    // no return value
    TRI_V8_RETURN_TRUE();
  }

  VPackSlice resultSlice = opResult.slice();
  TRI_V8_RETURN(TRI_VPackToV8(isolate, resultSlice,
                              transactionContext.getVPackOptions()));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Replace a document, collection method
////////////////////////////////////////////////////////////////////////////////

static void JS_ReplaceVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  ModifyVocbaseCol(TRI_VOC_DOCUMENT_OPERATION_REPLACE, args);
  TRI_V8_TRY_CATCH_END
}

static void JS_UpdateVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  ModifyVocbaseCol(TRI_VOC_DOCUMENT_OPERATION_UPDATE, args);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ModifyVocbase, database method, only single documents
////////////////////////////////////////////////////////////////////////////////

static void ModifyVocbase(TRI_voc_document_operation_e operation,
                          v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // check the arguments
  uint32_t const argLength = args.Length();

  if (argLength < 2 ||
      argLength >
          (operation == TRI_VOC_DOCUMENT_OPERATION_REPLACE ? 4UL : 5UL)) {
    if (operation == TRI_VOC_DOCUMENT_OPERATION_REPLACE) {
      TRI_V8_THROW_EXCEPTION_USAGE(
          "_replace(<document>, <data>, {overwrite: booleanValue, waitForSync: "
          "booleanValue, returnNew: booleanValue, returnOld: booleanValue,"
          " silent: booleanValue})");
    } else {
      TRI_V8_THROW_EXCEPTION_USAGE(
          "_update(<document>, <data>, {overwrite: booleanValue, keepNull: "
          "booleanValue, mergeObjects: booleanValue, waitForSync: "
          "booleanValue, returnNew: booleanValue, returnOld: booleanValue,"
          " silent: booleanValue})");
    }
  }

  // we're only accepting "real" object documents
  if (!args[1]->IsObject() || args[1]->IsArray()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  OperationOptions options;
  options.ignoreRevs = false;
  if (args.Length() > 2) {
    parseReplaceAndUpdateOptions(isolate, args, options, operation);
  }

  std::shared_ptr<arangodb::LogicalCollection> collection;
  std::string collectionName;
  auto& vocbase = GetContextVocBase(isolate);
  bool extendedNames =
      vocbase.server().getFeature<DatabaseFeature>().extendedNames();
  auto origin = transaction::OperationOriginREST{"modifying document(s)"};
  transaction::V8Context transactionContext(vocbase, origin, true);
  VPackBuilder updateBuilder;

  {
    VPackObjectBuilder guard(&updateBuilder);
    auto res = V8ToVPackNoKeyRevId(isolate, updateBuilder, args[1]);

    if (res != TRI_ERROR_NO_ERROR) {
      TRI_V8_THROW_EXCEPTION(res);
    }

    res = ParseDocumentOrDocumentHandle(
        isolate, &(transactionContext.resolver()), extendedNames, collection,
        collectionName, updateBuilder, !options.ignoreRevs, args[0]);

    if (res != TRI_ERROR_NO_ERROR) {
      TRI_V8_THROW_EXCEPTION(res);
    }
  }

  transaction::Options trxOpts;
  trxOpts.delaySnapshot = true;

  SingleCollectionTransaction trx(
      std::shared_ptr<transaction::Context>(
          std::shared_ptr<transaction::Context>(), &transactionContext),
      collectionName, AccessMode::Type::WRITE, trxOpts);
  addTransactionHints(*collection, trx, false, false);

  Result res = trx.begin();
  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  VPackSlice update = updateBuilder.slice();

  OperationResult opResult = (operation == TRI_VOC_DOCUMENT_OPERATION_REPLACE)
                                 ? trx.replace(collectionName, update, options)
                                 : trx.update(collectionName, update, options);

  res = trx.finish(opResult.result);

  if (opResult.fail()) {
    TRI_V8_THROW_EXCEPTION(opResult.result);
  }

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  if (options.silent) {
    // no return value
    TRI_V8_RETURN_TRUE();
  }

  VPackSlice resultSlice = opResult.slice();
  TRI_V8_RETURN(TRI_VPackToV8(isolate, resultSlice,
                              transactionContext.getVPackOptions()));
  TRI_V8_TRY_CATCH_END
}

static void JS_ReplaceVocbase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  ModifyVocbase(TRI_VOC_DOCUMENT_OPERATION_REPLACE, args);
  TRI_V8_TRY_CATCH_END
}

static void JS_UpdateVocbase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  ModifyVocbase(TRI_VOC_DOCUMENT_OPERATION_UPDATE, args);
  TRI_V8_TRY_CATCH_END
}

static void JS_RevisionVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  struct NonDeleter {
    void operator()(LogicalCollection*) {}
  };

  // we are not responsible for this collection object, but need to wrap it into
  // a shared_ptr here
  std::shared_ptr<LogicalCollection> coll(collection, NonDeleter());
  methods::Collections::Context ctxt(coll);
  OperationOptions options(ExecContext::current());
  auto res = methods::Collections::revisionId(ctxt, options).get();

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res.result);
  }

  RevisionId rid = RevisionId::fromSlice(res.slice());
  std::string ridString = rid.toString();
  TRI_V8_RETURN(TRI_V8_STD_STRING(isolate, ridString));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief inserts a document, using VPack
////////////////////////////////////////////////////////////////////////////////

static void InsertVocbaseCol(v8::Isolate* isolate,
                             v8::FunctionCallbackInfo<v8::Value> const& args,
                             std::string* attachment) {
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  auto isEdgeCollection = (collection->type() == TRI_COL_TYPE_EDGE);
  uint32_t const argLength = args.Length();

  // Position of <data> and <options>
  // They differ for edge (old signature) and document.
  uint32_t docIdx = 0;
  uint32_t optsIdx = (attachment == nullptr) ? 1 : 2;

  TRI_GET_GLOBALS();

  if (argLength < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("insert(<data>, [, <options>])");
  }

  bool oldEdgeSignature = false;

  if (isEdgeCollection && argLength >= 3) {
    oldEdgeSignature = true;
    if (argLength > 4) {
      TRI_V8_THROW_EXCEPTION_USAGE(
          "insert(<from>, <to>, <data> [, <options>])");
    }
    docIdx = 2;
    optsIdx = (attachment == nullptr) ? 3 : 4;
    if (args[2]->IsArray()) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
    }
  } else {
    if (argLength < 1 || argLength > 2) {
      TRI_V8_THROW_EXCEPTION_USAGE("insert(<data> [, <options>])");
    }
  }

  OperationOptions options;
  if (argLength > optsIdx && args[optsIdx]->IsObject()) {
    v8::Handle<v8::Object> optionsObject = args[optsIdx].As<v8::Object>();

    TRI_GET_GLOBAL_STRING(WaitForSyncKey);
    if (TRI_HasProperty(context, isolate, optionsObject, WaitForSyncKey)) {
      options.waitForSync = TRI_ObjectToBoolean(
          isolate, optionsObject->Get(context, WaitForSyncKey)
                       .FromMaybe(v8::Local<v8::Value>()));
    }

    TRI_GET_GLOBAL_STRING(SkipDocumentValidationKey);
    if (TRI_HasProperty(context, isolate, optionsObject,
                        SkipDocumentValidationKey)) {
      options.validate = !TRI_ObjectToBoolean(
          isolate, optionsObject->Get(context, SkipDocumentValidationKey)
                       .FromMaybe(v8::Local<v8::Value>()));
    }

    TRI_GET_GLOBAL_STRING(OverwriteKey);
    if (TRI_HasProperty(context, isolate, optionsObject, OverwriteKey)) {
      bool overwrite =
          TRI_ObjectToBoolean(isolate, optionsObject->Get(context, OverwriteKey)
                                           .FromMaybe(v8::Local<v8::Value>()));
      if (overwrite) {
        // this is the default mode in case only "overwrite" is set.
        TRI_ASSERT(!options.isOverwriteModeSet());
        options.overwriteMode = OperationOptions::OverwriteMode::Replace;
      }
    }

    TRI_GET_GLOBAL_STRING(OverwriteModeKey);
    if (TRI_HasProperty(context, isolate, optionsObject, OverwriteModeKey)) {
      auto mode = TRI_ObjectToString(
          isolate, optionsObject->Get(context, OverwriteModeKey)
                       .FromMaybe(v8::Local<v8::Value>()));

      auto overwriteMode = OperationOptions::determineOverwriteMode(mode);
      if (overwriteMode != OperationOptions::OverwriteMode::Unknown) {
        options.overwriteMode = overwriteMode;

        if (overwriteMode == OperationOptions::OverwriteMode::Update) {
          TRI_GET_GLOBAL_STRING(KeepNullKey);
          if (TRI_HasProperty(context, isolate, optionsObject, KeepNullKey)) {
            options.keepNull = TRI_ObjectToBoolean(
                isolate, optionsObject->Get(context, KeepNullKey)
                             .FromMaybe(v8::Local<v8::Value>()));
          }
          TRI_GET_GLOBAL_STRING(MergeObjectsKey);
          if (TRI_HasProperty(context, isolate, optionsObject,
                              MergeObjectsKey)) {
            options.mergeObjects = TRI_ObjectToBoolean(
                isolate, optionsObject->Get(context, MergeObjectsKey)
                             .FromMaybe(v8::Local<v8::Value>()));
          }
        }
      }
    }

    TRI_GET_GLOBAL_STRING(SilentKey);
    if (TRI_HasProperty(context, isolate, optionsObject, SilentKey)) {
      options.silent =
          TRI_ObjectToBoolean(isolate, optionsObject->Get(context, SilentKey)
                                           .FromMaybe(v8::Local<v8::Value>()));
    }
    TRI_GET_GLOBAL_STRING(ReturnNewKey);
    if (TRI_HasProperty(context, isolate, optionsObject, ReturnNewKey)) {
      options.returnNew =
          TRI_ObjectToBoolean(isolate, optionsObject->Get(context, ReturnNewKey)
                                           .FromMaybe(v8::Local<v8::Value>()));
    }
    TRI_GET_GLOBAL_STRING(ReturnOldKey);
    if (TRI_HasProperty(context, isolate, optionsObject, ReturnOldKey)) {
      options.returnOld =
          TRI_ObjectToBoolean(isolate,
                              optionsObject->Get(context, ReturnOldKey)
                                  .FromMaybe(v8::Local<v8::Value>())) &&
          options.isOverwriteModeUpdateReplace();
    }
    TRI_GET_GLOBAL_STRING(RefillIndexCachesKey);
    // this attribute can have 3 values: default, true and false. only
    // pick it up when it is set to true or false
    if (TRI_HasProperty(context, isolate, optionsObject,
                        RefillIndexCachesKey)) {
      options.refillIndexCaches =
          (TRI_ObjectToBoolean(isolate,
                               optionsObject->Get(context, RefillIndexCachesKey)
                                   .FromMaybe(v8::Local<v8::Value>())))
              ? RefillIndexCaches::kRefill
              : RefillIndexCaches::kDontRefill;
    }
    TRI_GET_GLOBAL_STRING(IsRestoreKey);
    if (TRI_HasProperty(context, isolate, optionsObject, IsRestoreKey)) {
      options.isRestore =
          TRI_ObjectToBoolean(isolate, optionsObject->Get(context, IsRestoreKey)
                                           .FromMaybe(v8::Local<v8::Value>()));
    }
    TRI_GET_GLOBAL_STRING(IsSynchronousReplicationKey);
    if (TRI_HasProperty(context, isolate, optionsObject,
                        IsSynchronousReplicationKey)) {
      options.isSynchronousReplicationFrom = TRI_ObjectToString(
          isolate, optionsObject->Get(context, IsSynchronousReplicationKey)
                       .FromMaybe(v8::Local<v8::Value>()));
    }
  } else {
    options.waitForSync =
        ExtractBooleanArgument(isolate, args, optsIdx + 1, false);
  }

  if (!args[docIdx]->IsObject()) {
    // invalid value type. must be a document
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  // copy default options (and set exclude handler in copy)
  VPackOptions vpackOptions = VPackOptions::Defaults;
  VPackBuilder builder(&vpackOptions);

  auto doOneDocument = [&](v8::Handle<v8::Value> obj) -> void {
    TRI_V8ToVPack(isolate, builder, obj, true);

    if (isEdgeCollection && oldEdgeSignature) {
      // Just insert from and to. Check is done later.
      std::string tmpId(ExtractIdString(isolate, args[0]));
      if (tmpId.empty()) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD);
      }
      builder.add(StaticStrings::FromString, VPackValue(tmpId));

      tmpId = ExtractIdString(isolate, args[1]);
      if (tmpId.empty()) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD);
      }
      builder.add(StaticStrings::ToString, VPackValue(tmpId));
    }

    if (attachment != nullptr) {
      builder.add(StaticStrings::AttachmentString, VPackValue(*attachment));
    }

    builder.close();
  };

  v8::Handle<v8::Value> payload = args[docIdx];
  bool payloadIsArray;
  if (payload->IsArray()) {
    payloadIsArray = true;
    VPackArrayBuilder b(&builder);
    v8::Handle<v8::Array> array = v8::Handle<v8::Array>::Cast(payload);
    uint32_t const n = array->Length();
    for (uint32_t i = 0; i < n; ++i) {
      doOneDocument(array->Get(context, i).FromMaybe(v8::Local<v8::Value>()));
    }
  } else {
    payloadIsArray = false;
    doOneDocument(payload);
  }

  transaction::Options trxOpts;
  trxOpts.delaySnapshot = !payloadIsArray;  // for now we only enable this for
                                            // single document operations

  auto origin = transaction::OperationOriginREST{"inserting document(s)"};
  transaction::V8Context transactionContext(collection->vocbase(), origin,
                                            true);
  SingleCollectionTransaction trx(
      std::shared_ptr<transaction::Context>(
          std::shared_ptr<transaction::Context>(), &transactionContext),
      *collection, AccessMode::Type::WRITE, trxOpts);

  addTransactionHints(*collection, trx, payloadIsArray,
                      options.isOverwriteModeUpdateReplace());

  Result res = trx.begin();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  auto result = trx.insert(collection->name(), builder.slice(), options);

  res = trx.finish(result.result);

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  if (options.silent) {
    // no return value
    TRI_V8_RETURN_TRUE();
  }

  VPackSlice resultSlice = result.slice();

  auto v8Result =
      TRI_VPackToV8(isolate, resultSlice, transactionContext.getVPackOptions());

  TRI_V8_RETURN(v8Result);
}

static void JS_InsertVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  InsertVocbaseCol(isolate, args, nullptr);
  TRI_V8_TRY_CATCH_END
}

static void JS_BinaryInsertVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  uint32_t const argLength = args.Length();

  if (argLength < 2 || argLength > 3) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "binaryInsert(<data>, <filename> [, <options>])");
  }

  std::string filename = TRI_ObjectToString(isolate, args[1]);
  std::string attachment;

  try {
    attachment = FileUtils::slurp(filename);
  } catch (...) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), TRI_last_error());
  }

  attachment = absl::Base64Escape(attachment);

  InsertVocbaseCol(isolate, args, &attachment);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the globally unique id of a collection
////////////////////////////////////////////////////////////////////////////////

static void JS_GloballyUniqueIdVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  auto& uniqueId = collection->guid();

  TRI_V8_RETURN(TRI_V8_ASCII_STD_STRING(isolate, uniqueId));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the status of a collection
////////////////////////////////////////////////////////////////////////////////

static void JS_StatusVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  bool ok = false;
  if (ServerState::instance()->isCoordinator()) {
    auto& databaseName = collection->vocbase().name();

    auto ci = collection->vocbase()
                  .server()
                  .getFeature<arangodb::ClusterFeature>()
                  .clusterInfo()
                  .getCollectionNT(databaseName,
                                   std::to_string(collection->id().id()));
    if (ci != nullptr && !ci->deleted()) {
      ok = true;
    }
  } else if (!collection->deleted()) {
    ok = true;
  }

  if (ok) {
    TRI_V8_RETURN(v8::Number::New(isolate, /*TRI_VOC_COL_STATUS_LOADED*/ 3));
  } else {
    TRI_V8_RETURN(v8::Number::New(isolate, /*TRI_VOC_COL_STATUS_DELETED*/ 5));
  }

  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief truncates a collection
////////////////////////////////////////////////////////////////////////////////

static void JS_TruncateVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  OperationOptions options;
  if ((args.Length() >= 1) && args[0]->IsObject()) {
    v8::Handle<v8::Object> optionsObject = args[0].As<v8::Object>();
    getOperationOptionsFromObject(isolate, options, optionsObject);
  }

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  {
    auto origin = transaction::OperationOriginREST{"truncating collection"};
    auto ctx =
        transaction::V8Context::create(collection->vocbase(), origin, true);
    SingleCollectionTransaction trx(ctx, *collection,
                                    AccessMode::Type::EXCLUSIVE);
    trx.addHint(transaction::Hints::Hint::INTERMEDIATE_COMMITS);
    trx.addHint(transaction::Hints::Hint::ALLOW_RANGE_DELETE);
    Result res = trx.begin();

    if (!res.ok()) {
      TRI_V8_THROW_EXCEPTION(res);
    }

    auto result = trx.truncate(collection->name(), options);

    res = trx.finish(result.result);

    if (!res.ok()) {
      TRI_V8_THROW_EXCEPTION(res);
    }
  }

  if (options.truncateCompact) {
    // wait for the transaction to finish first. only after that compact the
    // data range(s) for the collection
    // we shouldn't run compact() as part of the transaction, because the
    // compact will be useless inside due to the snapshot the transaction has
    // taken
    collection->compact();
  }
  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

static void JS_TypeVocbaseCol(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (ServerState::instance()->isCoordinator()) {
    auto& databaseName = collection->vocbase().name();

    auto ci = collection->vocbase()
                  .server()
                  .getFeature<ClusterFeature>()
                  .clusterInfo()
                  .getCollectionNT(databaseName,
                                   std::to_string(collection->id().id()));
    if (ci != nullptr) {
      TRI_V8_RETURN(v8::Number::New(isolate, (int)ci->type()));
    } else {
      TRI_V8_RETURN(v8::Number::New(isolate, (int)(collection->type())));
    }
  }
  // intentionally falls through

  auto type = collection->type();

  TRI_V8_RETURN(v8::Number::New(isolate, (int)type));
  TRI_V8_TRY_CATCH_END
}

static void JS_UnloadVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  // unload is a no-op starting with ArangoDB 3.9

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the version of a collection
////////////////////////////////////////////////////////////////////////////////

static void JS_VersionVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_V8_RETURN(
      v8::Number::New(isolate, static_cast<uint32_t>(collection->version())));
  TRI_V8_TRY_CATCH_END
}

static void JS_CollectionVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto& vocbase = GetContextVocBase(isolate);

  if (vocbase.isDropped()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  // expecting one argument
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("_collection(<name>|<identifier>)");
  }

  v8::Handle<v8::Value> val = args[0];
  auto collection = GetCollectionFromArgument(isolate, vocbase, val);

  if (collection == nullptr) {
    TRI_V8_RETURN_NULL();
  }

  // check authentication after ensuring the collection exists
  auto const& exec = ExecContext::current();
  if (!exec.canUseCollection(collection->name(), auth::Level::RO)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("No access to collection '") +
                                       TRI_ObjectToString(isolate, val) + "'");
  }

  v8::Handle<v8::Value> result = WrapCollection(isolate, collection);
  if (result.IsEmpty()) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

static void JS_CollectionsVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  auto& vocbase = GetContextVocBase(isolate);

  if (vocbase.isDropped()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  auto colls = methods::Collections::sorted(vocbase);
  bool error = false;

  v8::Handle<v8::Array> result = v8::Array::New(isolate);
  size_t const n = colls.size();
  size_t x = 0;

  auto const& exec = ExecContext::current();
  for (size_t i = 0; i < n; ++i) {
    auto& coll = colls[i];

    if (!exec.canUseCollection(vocbase.name(), coll->name(), auth::Level::RO)) {
      continue;
    }

    v8::Handle<v8::Value> c = WrapCollection(isolate, coll);
    if (c.IsEmpty()) {
      error = true;
      break;
    }

    result->Set(context, static_cast<uint32_t>(x++), c).FromMaybe(false);
  }

  if (error) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns all collection names
////////////////////////////////////////////////////////////////////////////////

static void JS_CompletionsVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  auto& vocbase = GetContextVocBase(isolate);

  if (vocbase.isDropped()) {
    TRI_V8_RETURN(v8::Array::New(isolate));
  }

  std::vector<std::string> names;

  if (ServerState::instance()->isCoordinator()) {
    if (vocbase.server()
            .getFeature<ClusterFeature>()
            .clusterInfo()
            .doesDatabaseExist(vocbase.name())) {
      names = GetCollectionNamesCluster(&vocbase);
    }
  } else {
    names = vocbase.collectionNames();
  }

  uint32_t j = 0;

  v8::Handle<v8::Array> result = v8::Array::New(isolate);
  // add collection names
  for (auto& name : names) {
    result->Set(context, j++, TRI_V8_STD_STRING(isolate, name))
        .FromMaybe(false);
  }

  // add function names. these are hard coded
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_collection()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_collections()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_create()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_createDatabase()"))
      .FromMaybe(false);
  result
      ->Set(context, j++,
            TRI_V8_ASCII_STRING(isolate, "_createDocumentCollection()"))
      .FromMaybe(false);
  result
      ->Set(context, j++,
            TRI_V8_ASCII_STRING(isolate, "_createEdgeCollection()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_createView()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_createStatement()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_document()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_drop()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_dropDatabase()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_dropView()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_engineStats()"))
      .FromMaybe(false);
  result
      ->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_executeTransaction()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_exists()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_explain()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_id"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_isSystem()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_databases()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_engine()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_name()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_path()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_parse()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_pregelStart()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_pregelStatus()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_pregelStop()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_profileQuery()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_query()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_remove()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_replace()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_update()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_useDatabase()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_version()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_view()"))
      .FromMaybe(false);
  result->Set(context, j++, TRI_V8_ASCII_STRING(isolate, "_views()"))
      .FromMaybe(false);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

static void JS_RemoveVocbase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  RemoveVocbase(args);
  // cppcheck-suppress style
  TRI_V8_TRY_CATCH_END
}

static void JS_DocumentVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  DocumentVocbase(args);
  TRI_V8_TRY_CATCH_END
}

static void JS_ExistsVocbase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  return ExistsVocbaseVPack(false, args);

  // cppcheck-suppress style
  TRI_V8_TRY_CATCH_END
}

static void JS_CountVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* col = UnwrapCollection(isolate, args.Holder());

  if (!col) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (args.Length() > 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("count()");
  }

  bool details = false;
  if (args.Length() == 1 && ServerState::instance()->isCoordinator()) {
    details = TRI_ObjectToBoolean(isolate, args[0]);
  }

  auto& collectionName = col->name();
  auto origin = transaction::OperationOriginREST{"counting documents"};
  SingleCollectionTransaction trx(
      transaction::V8Context::create(col->vocbase(), origin, true),
      collectionName, AccessMode::Type::READ);

  Result res = trx.begin();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  OperationOptions options(ExecContext::current());
  OperationResult opResult =
      trx.count(collectionName,
                details ? transaction::CountType::Detailed
                        : transaction::CountType::Normal,
                options);
  res = trx.finish(opResult.result);

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  VPackSlice s = opResult.slice();
  if (details) {
    TRI_ASSERT(s.isObject());
    v8::Handle<v8::Value> result = TRI_VPackToV8(isolate, s);
    TRI_V8_RETURN(result);
  } else {
    TRI_ASSERT(s.isNumber());
    TRI_V8_RETURN(
        v8::Number::New(isolate, static_cast<double>(s.getNumber<double>())));
  }
  TRI_V8_TRY_CATCH_END
}

// .............................................................................
// Warmup Index caches
// .............................................................................

static void JS_WarmupVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  auto res =
      arangodb::methods::Collections::warmup(collection->vocbase(), *collection)
          .get();

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }
  TRI_V8_RETURN_UNDEFINED();

  TRI_V8_TRY_CATCH_END
}

// .............................................................................
// generate the arangodb::LogicalCollection template
// .............................................................................

void TRI_InitV8Collections(v8::Handle<v8::Context> context,
                           TRI_vocbase_t* vocbase, TRI_v8_global_t* v8g,
                           v8::Isolate* isolate,
                           v8::Handle<v8::ObjectTemplate> ArangoDBNS) {
  TRI_AddMethodVocbase(isolate, ArangoDBNS,
                       TRI_V8_ASCII_STRING(isolate, "_collection"),
                       JS_CollectionVocbase);
  TRI_AddMethodVocbase(isolate, ArangoDBNS,
                       TRI_V8_ASCII_STRING(isolate, "_collections"),
                       JS_CollectionsVocbase);
  TRI_AddMethodVocbase(isolate, ArangoDBNS,
                       TRI_V8_ASCII_STRING(isolate, "_COMPLETIONS"),
                       JS_CompletionsVocbase, true);
  TRI_AddMethodVocbase(isolate, ArangoDBNS,
                       TRI_V8_ASCII_STRING(isolate, "_document"),
                       JS_DocumentVocbase);
  TRI_AddMethodVocbase(isolate, ArangoDBNS,
                       TRI_V8_ASCII_STRING(isolate, "_exists"),
                       JS_ExistsVocbase);
  TRI_AddMethodVocbase(isolate, ArangoDBNS,
                       TRI_V8_ASCII_STRING(isolate, "_remove"),
                       JS_RemoveVocbase);
  TRI_AddMethodVocbase(isolate, ArangoDBNS,
                       TRI_V8_ASCII_STRING(isolate, "_replace"),
                       JS_ReplaceVocbase);
  TRI_AddMethodVocbase(isolate, ArangoDBNS,
                       TRI_V8_ASCII_STRING(isolate, "_update"),
                       JS_UpdateVocbase);

  v8::Handle<v8::ObjectTemplate> rt;
  v8::Handle<v8::FunctionTemplate> ft;

  ft = v8::FunctionTemplate::New(isolate);
  ft->SetClassName(TRI_V8_ASCII_STRING(isolate, "ArangoCollection"));

  rt = ft->InstanceTemplate();
  rt->SetInternalFieldCount(2);  // SLOT_CLASS_TYPE + SLOT_CLASS

  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "count"),
                       JS_CountVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "document"),
                       JS_DocumentVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt,
                       TRI_V8_ASCII_STRING(isolate, "_binaryDocument"),
                       JS_BinaryDocumentVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "drop"),
                       JS_DropVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "exists"),
                       JS_ExistsVocbaseVPack);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "figures"),
                       JS_FiguresVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt,
                       TRI_V8_ASCII_STRING(isolate, "getResponsibleShard"),
                       JS_GetResponsibleShardVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "insert"),
                       JS_InsertVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt,
                       TRI_V8_ASCII_STRING(isolate, "_binaryInsert"),
                       JS_BinaryInsertVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt,
                       TRI_V8_ASCII_STRING(isolate, "globallyUniqueId"),
                       JS_GloballyUniqueIdVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "load"),
                       JS_LoadVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "name"),
                       JS_NameVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "path"),
                       JS_PathVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "planId"),
                       JS_PlanIdVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "properties"),
                       JS_PropertiesVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "remove"),
                       JS_RemoveVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "revision"),
                       JS_RevisionVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "rename"),
                       JS_RenameVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "replace"),
                       JS_ReplaceVocbaseCol);
  TRI_AddMethodVocbase(
      isolate, rt, TRI_V8_ASCII_STRING(isolate, "save"),
      JS_InsertVocbaseCol);  // note: save is now an alias for insert
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "status"),
                       JS_StatusVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "truncate"),
                       JS_TruncateVocbaseCol, true);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "type"),
                       JS_TypeVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "unload"),
                       JS_UnloadVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "update"),
                       JS_UpdateVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "version"),
                       JS_VersionVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt,
                       TRI_V8_ASCII_STRING(isolate, "loadIndexesIntoMemory"),
                       JS_WarmupVocbaseCol);

  TRI_InitV8IndexCollection(isolate, rt);

  v8g->VocbaseColTempl.Reset(isolate, rt);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "ArangoCollection"),
      ft->GetFunction(TRI_IGETC).FromMaybe(v8::Local<v8::Function>()));
}
