////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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

#include "utils/misc.hpp"

#include "gtest/gtest.h"

#include "Mocks/Servers.h"
#include "Mocks/IResearchLinkMock.h"
#include "Mocks/PhysicalCollectionMock.h"

#include "Agency/Store.h"
#include "ApplicationFeatures/CommunicationFeaturePhase.h"
#include "ApplicationFeatures/GreetingsFeaturePhase.h"
#include "Aql/AstNode.h"
#include "Aql/Variable.h"
#include "Basics/files.h"
#include "Basics/GlobalResourceMonitor.h"
#include "Basics/ResourceUsage.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "FeaturePhases/BasicFeaturePhaseServer.h"
#include "FeaturePhases/ClusterFeaturePhase.h"
#include "FeaturePhases/DatabaseFeaturePhase.h"
#include "FeaturePhases/V8FeaturePhase.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchLink.h"
#include "IResearch/IResearchLinkHelper.h"
#include "IResearch/IResearchLinkMeta.h"
#include "IResearch/IResearchView.h"
#include "RestServer/ViewTypesFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "VocBase/LogicalCollection.h"
#include "common.h"
#include <filesystem>
#include "velocypack/Parser.h"

#include "IResearch/MakeViewSnapshot.h"

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

class IResearchViewDBServerTest : public ::testing::Test {
 protected:
  arangodb::tests::mocks::MockDBServer server;
  arangodb::GlobalResourceMonitor global{};
  arangodb::ResourceMonitor resourceMonitor{global};

  IResearchViewDBServerTest() : server("PRMR_0001") {}

  void createTestDatabase(TRI_vocbase_t*& vocbase,
                          std::string const name = "testDatabase") {
    vocbase = server.createDatabase(name);
    ASSERT_NE(nullptr, vocbase);
    ASSERT_EQ(name, vocbase->name());
  }

  void initLink(arangodb::iresearch::IResearchLinkMock& link) {
    auto json = VPackParser::fromJson(R"({ "view": "42" })");
    bool pathExists = false;
    EXPECT_TRUE(link.init(json->slice(), pathExists).ok());
    EXPECT_FALSE(pathExists);
  }

  ~IResearchViewDBServerTest() = default;
};

#ifdef USE_ENTERPRISE
static constexpr size_t kEnterpriseFields = 1;
#else
static constexpr size_t kEnterpriseFields = 0;
#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------
TEST_F(IResearchViewDBServerTest, test_drop) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();
  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
  createTestDatabase(vocbase);

  // drop empty
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE(arangodb::iresearch::IResearchView::factory()
                    .create(view, *vocbase, json->slice(), true)
                    .ok());
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    EXPECT_TRUE(impl->drop().ok());
  }

  // drop non-empty
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection0\" }");
    auto linkJson = arangodb::velocypack::Parser::fromJson(
        "{ \"view\": \"testView0\", \"type\": \"arangosearch\", "
        "\"includeAllFields\": true }");
    auto viewJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView0\", \"type\": \"arangosearch\" }");
    auto const viewId = std::to_string(
        ci.uniqid() +
        1);  // +1 because LogicalView creation will generate a new ID
    auto logicalCollection = vocbase->createCollection(collectionJson->slice());
    ASSERT_FALSE(!logicalCollection);
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE(arangodb::iresearch::IResearchView::factory()
                    .create(view, *vocbase, viewJson->slice(), true)
                    .ok());
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    // ensure we have shard view in vocbase
    bool created;
    auto index =
        logicalCollection->createIndex(linkJson->slice(), created).get();
    ASSERT_FALSE(!index);
    auto link =
        std::dynamic_pointer_cast<arangodb::iresearch::IResearchLinkMock>(
            index);
    ASSERT_FALSE(!link);

    static auto visitor = [](arangodb::DataSourceId,
                             arangodb::LogicalView::Indexes*) { return false; };
    EXPECT_FALSE(impl->visitCollections(visitor));
    EXPECT_TRUE((false == !arangodb::iresearch::IResearchLinkHelper::find(
                              *logicalCollection, *view)));
    EXPECT_TRUE(impl->drop().ok());
    EXPECT_TRUE((true == !arangodb::iresearch::IResearchLinkHelper::find(
                             *logicalCollection, *view)));
    EXPECT_FALSE(impl->visitCollections(
        visitor));  // list of links is not modified after link drop
  }

  // drop non-empty (drop failure)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection1\" }");
    auto linkJson = arangodb::velocypack::Parser::fromJson(
        "{ \"view\": \"testView1\", \"type\": \"arangosearch\", "
        "\"includeAllFields\": true }");
    auto viewJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView1\", \"type\": \"arangosearch\" }");
    auto const viewId = std::to_string(
        ci.uniqid() +
        1);  // +1 because LogicalView creation will generate a new ID
    auto logicalCollection = vocbase->createCollection(collectionJson->slice());
    ASSERT_FALSE(!logicalCollection);
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .create(view, *vocbase, viewJson->slice(), true)
                     .ok()));
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    // ensure we have shard view in vocbase
    bool created;
    auto index =
        logicalCollection->createIndex(linkJson->slice(), created).get();
    ASSERT_FALSE(!index);
    auto link =
        std::dynamic_pointer_cast<arangodb::iresearch::IResearchLinkMock>(
            index);
    ASSERT_FALSE(!link);

    static auto visitor = [](arangodb::DataSourceId,
                             arangodb::LogicalView::Indexes*) { return false; };
    EXPECT_FALSE(impl->visitCollections(visitor));
    EXPECT_TRUE((false == !arangodb::iresearch::IResearchLinkHelper::find(
                              *logicalCollection, *view)));

    auto before = PhysicalCollectionMock::before;
    irs::Finally restore = [&before]() noexcept {
      PhysicalCollectionMock::before = before;
    };
    PhysicalCollectionMock::before = []() -> void { throw std::exception(); };

    EXPECT_FALSE(impl->drop().ok());
    EXPECT_TRUE((false == !arangodb::iresearch::IResearchLinkHelper::find(
                              *logicalCollection, *view)));
    EXPECT_FALSE(impl->visitCollections(visitor));
  }
}

TEST_F(IResearchViewDBServerTest, test_drop_cid) {
  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
  createTestDatabase(vocbase);

  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testCollection\" }");
  auto linkJson = arangodb::velocypack::Parser::fromJson(
      "{ \"view\": \"testView\", \"type\": \"arangosearch\", "
      "\"includeAllFields\": true }");
  auto viewJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
  auto logicalCollection = vocbase->createCollection(collectionJson->slice());
  ASSERT_FALSE(!logicalCollection);
  arangodb::LogicalView::ptr view;
  ASSERT_TRUE(arangodb::iresearch::IResearchView::factory()
                  .create(view, *vocbase, viewJson->slice(), true)
                  .ok());
  EXPECT_FALSE(!view);
  auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
  EXPECT_NE(nullptr, impl);

  // ensure we have shard view in vocbase
  bool created;
  auto index = logicalCollection->createIndex(linkJson->slice(), created).get();
  ASSERT_FALSE(!index);
  auto link =
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchLinkMock>(index);
  ASSERT_FALSE(!link);

  static auto visitor = [](arangodb::DataSourceId,
                           arangodb::LogicalView::Indexes*) { return false; };
  EXPECT_FALSE(impl->visitCollections(visitor));
  EXPECT_TRUE((false == !arangodb::iresearch::IResearchLinkHelper::find(
                            *logicalCollection, *view)));
  EXPECT_TRUE(impl->unlink(logicalCollection->id()).ok());
  EXPECT_TRUE((false == !arangodb::iresearch::IResearchLinkHelper::find(
                            *logicalCollection, *view)));
  EXPECT_TRUE(impl->visitCollections(visitor));
  EXPECT_TRUE(impl->unlink(logicalCollection->id()).ok());
}

TEST_F(IResearchViewDBServerTest, test_drop_database) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testCollection\" }");
  auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"id\": \"42\", \"name\": \"testView\", \"type\": \"arangosearch\" }");
  auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": { \"testCollection\": { \"includeAllFields\": true } } }");

  size_t beforeCount = 0;
  auto before = PhysicalCollectionMock::before;
  irs::Finally restore = [&before]() noexcept {
    PhysicalCollectionMock::before = before;
  };
  PhysicalCollectionMock::before = [&beforeCount]() -> void { ++beforeCount; };

  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
  createTestDatabase(vocbase, "testDatabase" IRS_TO_STRING(__LINE__));

  auto logicalCollection = vocbase->createCollection(collectionJson->slice());
  ASSERT_FALSE(!logicalCollection);
  EXPECT_TRUE(
      (ci.createViewCoordinator(vocbase->name(), "42", viewCreateJson->slice())
           .ok()));
  auto logicalview = ci.getView(
      vocbase->name(), "42");  // link creation requires cluster-view to be in
                               // ClusterInfo instead of TRI_vocbase_t
  ASSERT_FALSE(!logicalview);
  auto* viewImpl =
      dynamic_cast<arangodb::iresearch::IResearchView*>(logicalview.get());
  ASSERT_FALSE(!viewImpl);

  beforeCount = 0;  // reset before call to StorageEngine::createView(...)
  auto res = logicalview->properties(viewUpdateJson->slice(), true, true);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(1, beforeCount);  // +1 for StorageEngineMock::createIndex(...) and
                              // then for various other activities
}

TEST_F(IResearchViewDBServerTest, test_ensure) {
  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
  createTestDatabase(vocbase);

  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testCollection\" }");
  auto linkJson = arangodb::velocypack::Parser::fromJson(
      "{ \"view\": \"testView\", \"type\": \"arangosearch\", "
      "\"includeAllFields\": true }");
  auto viewJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"collections\": "
      "[ 3, 4, 5 ] }");
  auto logicalCollection = vocbase->createCollection(collectionJson->slice());
  ASSERT_FALSE(!logicalCollection);
  arangodb::LogicalView::ptr view;
  ASSERT_TRUE(arangodb::iresearch::IResearchView::factory()
                  .create(view, *vocbase, viewJson->slice(), true)
                  .ok());
  EXPECT_FALSE(!view);
  auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
  EXPECT_NE(nullptr, impl);

  bool created;
  auto index = logicalCollection->createIndex(linkJson->slice(), created).get();
  ASSERT_FALSE(!index);
  auto link =
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchLinkMock>(index);
  ASSERT_FALSE(!link);

  static auto visitor = [](arangodb::DataSourceId,
                           arangodb::LogicalView::Indexes*) { return false; };
  EXPECT_FALSE(view->visitCollections(visitor));  // no collections in view
  EXPECT_TRUE((false == !arangodb::iresearch::IResearchLinkHelper::find(
                            *logicalCollection, *view)));
}

TEST_F(IResearchViewDBServerTest, test_make) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  // make DBServer view
  {
    auto const viewId =
        ci.uniqid() +
        1;  // +1 because LogicalView creation will generate a new ID
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    TRI_vocbase_t vocbase(testDBInfo(server.server()));
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .instantiate(view, vocbase, json->slice(), false)
                     .ok()));
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    EXPECT_EQ(std::string("testView"), view->name());
    EXPECT_FALSE(view->deleted());
    EXPECT_EQ(viewId, view->id().id());
    EXPECT_EQ(impl->id(), view->planId());  // same as view ID
    EXPECT_EQ(arangodb::iresearch::StaticStrings::ViewArangoSearchType,
              view->typeName());
    EXPECT_EQ(arangodb::ViewType::kArangoSearch, view->type());
    EXPECT_EQ(&vocbase, &(view->vocbase()));
  }
}

TEST_F(IResearchViewDBServerTest, test_open) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  // open empty
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    TRI_vocbase_t vocbase(testDBInfo(server.server()));
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .instantiate(view, vocbase, json->slice(), false)
                     .ok()));
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    static auto visitor = [](arangodb::DataSourceId,
                             arangodb::LogicalView::Indexes*) { return false; };
    EXPECT_TRUE(impl->visitCollections(visitor));
    view->open();
  }

  // open non-empty
  {
    auto const viewId = std::to_string(
        ci.uniqid() +
        1);  // +1 because LogicalView creation will generate a new ID
    std::string dataPath =
        (((std::filesystem::path() /= server.testFilesystemPath()) /=
          std::string("databases")) /= std::string("arangosearch-123"))
            .string();
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\" }");
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    TRI_vocbase_t vocbase(testDBInfo(server.server()));
    auto logicalCollection = vocbase.createCollection(collectionJson->slice());
    ASSERT_FALSE(!logicalCollection);
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .instantiate(view, vocbase, json->slice(), false)
                     .ok()));
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    // ensure we have shard view in vocbase
    arangodb::iresearch::IResearchLinkMock link{arangodb::IndexId{42},
                                                *logicalCollection};
    initLink(link);
    auto asyncLinkPtr =
        std::make_shared<arangodb::iresearch::AsyncLinkHandle>(&link);
    auto visitor = [](arangodb::DataSourceId, arangodb::LogicalView::Indexes*) {
      return false;
    };
    EXPECT_TRUE(impl->visitCollections(visitor));
    EXPECT_TRUE(impl->link(asyncLinkPtr).ok());
    EXPECT_FALSE(impl->visitCollections(visitor));
    view->open();
  }
}

TEST_F(IResearchViewDBServerTest, test_query) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ \
    \"id\": \"42\", \
    \"name\": \"testView\", \
    \"type\": \"arangosearch\" \
  }");
  static std::vector<std::string> const EMPTY;
  arangodb::aql::AstNode noop(arangodb::aql::AstNodeType::NODE_TYPE_FILTER);
  arangodb::aql::AstNode noopChild(arangodb::aql::AstNodeValue(true));

  noop.addMember(&noopChild);

  // no filter/order provided, means "RETURN *"
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\" }");
    auto linkJson = arangodb::velocypack::Parser::fromJson(
        "{ \"view\": \"testView\", \"type\": \"arangosearch\", "
        "\"includeAllFields\": true }");
    TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
    createTestDatabase(vocbase, "testDatabase0");
    auto logicalCollection = vocbase->createCollection(collectionJson->slice());
    ASSERT_NE(nullptr, logicalCollection);
    arangodb::LogicalView::ptr logicalview;
    ASSERT_TRUE(arangodb::iresearch::IResearchView::factory()
                    .create(logicalview, *vocbase, createJson->slice(), true)
                    .ok());
    ASSERT_FALSE(!logicalview);
    auto* viewImpl =
        dynamic_cast<arangodb::iresearch::IResearchView*>(logicalview.get());
    ASSERT_FALSE(!viewImpl);

    bool created;
    auto index =
        logicalCollection->createIndex(linkJson->slice(), created).get();
    ASSERT_FALSE(!index);
    auto link =
        std::dynamic_pointer_cast<arangodb::iresearch::IResearchLinkMock>(
            index);
    ASSERT_FALSE(!link);

    arangodb::transaction::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        std::vector<std::string>{logicalCollection->name()}, EMPTY, EMPTY,
        arangodb::transaction::Options());
    EXPECT_TRUE(trx.begin().ok());
    auto const cid = logicalCollection->id();
    arangodb::iresearch::ViewSnapshot::Links links;
    {
      auto guard = viewImpl->linksReadLock();
      links.emplace_back(viewImpl->linkLock(guard, cid));
    }
    auto* snapshot = makeViewSnapshot(
        trx, arangodb::iresearch::ViewSnapshotMode::FindOrCreate,
        std::move(links), viewImpl, viewImpl->name());
    EXPECT_EQ(0, snapshot->docs_count());
    EXPECT_TRUE(trx.commit().ok());
  }

  // ordered iterator
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\" }");
    auto linkJson = arangodb::velocypack::Parser::fromJson(
        "{ \"view\": \"testView\", \"type\": \"arangosearch\", "
        "\"includeAllFields\": true }");
    TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
    createTestDatabase(vocbase, "testDatabase1");
    auto logicalCollection = vocbase->createCollection(collectionJson->slice());
    ASSERT_NE(nullptr, logicalCollection);
    arangodb::LogicalView::ptr logicalview;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .create(logicalview, *vocbase, createJson->slice(), true)
                     .ok()));
    ASSERT_FALSE(!logicalview);
    auto* viewImpl =
        dynamic_cast<arangodb::iresearch::IResearchView*>(logicalview.get());
    ASSERT_FALSE(!viewImpl);

    bool created;
    auto index =
        logicalCollection->createIndex(linkJson->slice(), created).get();
    ASSERT_FALSE(!index);
    auto link =
        std::dynamic_pointer_cast<arangodb::iresearch::IResearchLinkMock>(
            index);
    ASSERT_FALSE(!link);

    // fill with test data
    {
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::iresearch::IResearchLinkMeta meta;
      meta._includeAllFields = true;
      arangodb::transaction::Methods trx(
          arangodb::transaction::StandaloneContext::create(
              *vocbase, arangodb::transaction::OperationOriginTestCase{}),
          EMPTY, std::vector<std::string>{logicalCollection->name()}, EMPTY,
          arangodb::transaction::Options());
      EXPECT_TRUE(trx.begin().ok());

      for (size_t i = 0; i < 12; ++i) {
        EXPECT_TRUE(
            (link->insert(trx, arangodb::LocalDocumentId(i), doc->slice())
                 .ok()));
      }

      EXPECT_TRUE(trx.commit().ok());
      EXPECT_TRUE(link->commit().ok());
    }

    arangodb::transaction::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        std::vector<std::string>{logicalCollection->name()}, EMPTY, EMPTY,
        arangodb::transaction::Options());
    EXPECT_TRUE(trx.begin().ok());
    auto const cid = logicalCollection->id();
    arangodb::iresearch::ViewSnapshot::Links links;
    {
      auto guard = viewImpl->linksReadLock();
      links.emplace_back(viewImpl->linkLock(guard, cid));
    }
    auto* snapshot = makeViewSnapshot(
        trx, arangodb::iresearch::ViewSnapshotMode::FindOrCreate,
        std::move(links), viewImpl, viewImpl->name());
    EXPECT_EQ(12, snapshot->docs_count());
    EXPECT_TRUE(trx.commit().ok());
  }

  // snapshot isolation
  {
    auto links = arangodb::velocypack::Parser::fromJson(
        "{ \
      \"links\": { \"testCollection\": { \"includeAllFields\" : true } } \
    }");
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\", \"id\":442 }");

    TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
    createTestDatabase(vocbase, "testDatabase" IRS_TO_STRING(__LINE__));
    auto logicalCollection = vocbase->createCollection(collectionJson->slice());
    std::vector<std::string> collections{logicalCollection->name()};
    EXPECT_TRUE(
        (ci.createViewCoordinator(vocbase->name(), "42", createJson->slice())
             .ok()));
    auto logicalview = ci.getView(
        vocbase->name(), "42");  // link creation requires cluster-view to be in
                                 // ClusterInfo instead of TRI_vocbase_t
    EXPECT_FALSE(!logicalview);
    auto* viewImpl =
        dynamic_cast<arangodb::iresearch::IResearchView*>(logicalview.get());
    EXPECT_FALSE(!viewImpl);
    arangodb::Result res = logicalview->properties(links->slice(), true, true);
    EXPECT_TRUE(res.ok());
    EXPECT_FALSE(logicalCollection->getIndexes().empty());

    // fill with test data
    {
      arangodb::transaction::Methods trx(
          arangodb::transaction::StandaloneContext::create(
              *vocbase, arangodb::transaction::OperationOriginTestCase{}),
          EMPTY, collections, EMPTY, arangodb::transaction::Options());
      EXPECT_TRUE(trx.begin().ok());

      arangodb::OperationOptions options;
      for (size_t i = 1; i <= 12; ++i) {
        auto doc = arangodb::velocypack::Parser::fromJson(
            std::string("{ \"key\": ") + std::to_string(i) + " }");
        EXPECT_TRUE(
            trx.insert(logicalCollection->name(), doc->slice(), options).ok());
      }

      EXPECT_TRUE(trx.commit().ok());
    }

    arangodb::transaction::Options trxOptions;

    arangodb::transaction::Methods trx0(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        collections, EMPTY, EMPTY, trxOptions);
    EXPECT_TRUE(trx0.begin().ok());
    auto const cid0 = logicalCollection->id();
    arangodb::iresearch::ViewSnapshot::Links links01, links02, links03;
    {
      auto guard = viewImpl->linksReadLock();
      links01.emplace_back(viewImpl->linkLock(guard, cid0));
      links02.emplace_back(viewImpl->linkLock(guard, cid0));
      links03.emplace_back(viewImpl->linkLock(guard, cid0));
    }
    auto* snapshot0 =
        makeViewSnapshot(trx0, arangodb::iresearch::ViewSnapshotMode::Find,
                         std::move(links01), viewImpl, viewImpl->name());
    EXPECT_TRUE(snapshot0 == nullptr);
    snapshot0 = makeViewSnapshot(
        trx0, arangodb::iresearch::ViewSnapshotMode::SyncAndReplace,
        std::move(links02), viewImpl, viewImpl->name());
    ASSERT_TRUE(snapshot0 != nullptr);
    auto* snapshotFind0 =
        makeViewSnapshot(trx0, arangodb::iresearch::ViewSnapshotMode::Find,
                         std::move(links03), viewImpl, viewImpl->name());
    EXPECT_TRUE(snapshotFind0 == snapshot0);
    EXPECT_EQ(12, snapshot0->docs_count());
    EXPECT_TRUE(trx0.commit().ok());

    // add more data
    {
      arangodb::transaction::Methods trx(
          arangodb::transaction::StandaloneContext::create(
              *vocbase, arangodb::transaction::OperationOriginTestCase{}),
          EMPTY, collections, EMPTY, arangodb::transaction::Options());
      EXPECT_TRUE(trx.begin().ok());

      arangodb::OperationOptions options;
      for (size_t i = 13; i <= 24; ++i) {
        auto doc = arangodb::velocypack::Parser::fromJson(
            std::string("{ \"key\": ") + std::to_string(i) + " }");
        EXPECT_TRUE(
            trx.insert(logicalCollection->name(), doc->slice(), options).ok());
      }

      EXPECT_TRUE(trx.commit().ok());
    }

    // old reader sees same data as before
    EXPECT_EQ(12, snapshot0->docs_count());

    // new reader sees new data
    arangodb::transaction::Methods trx1(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        collections, EMPTY, EMPTY, trxOptions);
    EXPECT_TRUE(trx1.begin().ok());
    auto const cid1 = logicalCollection->id();
    arangodb::iresearch::ViewSnapshot::Links links1;
    {
      auto guard = viewImpl->linksReadLock();
      links1.emplace_back(viewImpl->linkLock(guard, cid1));
    }
    auto* snapshot1 = makeViewSnapshot(
        trx1, arangodb::iresearch::ViewSnapshotMode::SyncAndReplace,
        std::move(links1), viewImpl, viewImpl->name());
    ASSERT_TRUE(snapshot1 != nullptr);
    EXPECT_EQ(24, snapshot1->docs_count());
    EXPECT_TRUE(trx1.commit().ok());
  }

  // query while running FlushThread
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\" }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": { \"includeAllFields\": true } } "
        "}");
    TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
    createTestDatabase(vocbase, "testDatabase" IRS_TO_STRING(__LINE__));
    ASSERT_NE(nullptr, vocbase);
    auto logicalCollection = vocbase->createCollection(collectionJson->slice());
    EXPECT_TRUE(
        (ci.createViewCoordinator(vocbase->name(), "42", createJson->slice())
             .ok()));
    auto logicalview = ci.getView(
        vocbase->name(), "42");  // link creation requires cluster-view to be in
                                 // ClusterInfo instead of TRI_vocbase_t
    ASSERT_FALSE(!logicalview);
    auto* viewImpl =
        dynamic_cast<arangodb::iresearch::IResearchView*>(logicalview.get());
    ASSERT_FALSE(!viewImpl);
    arangodb::Result res =
        logicalview->properties(viewUpdateJson->slice(), true, true);
    ASSERT_TRUE(res.ok());

    static std::vector<std::string> const EMPTY;
    arangodb::transaction::Options options;

    arangodb::aql::Variable variable("testVariable", 0, false, resourceMonitor);

    // test insert + query
    for (size_t i = 1; i < 200; ++i) {
      // insert
      {
        auto doc = arangodb::velocypack::Parser::fromJson(
            std::string("{ \"seq\": ") + std::to_string(i) + " }");
        arangodb::transaction::Methods trx(
            arangodb::transaction::StandaloneContext::create(
                *vocbase, arangodb::transaction::OperationOriginTestCase{}),
            EMPTY, std::vector<std::string>{logicalCollection->name()}, EMPTY,
            options);

        EXPECT_TRUE(trx.begin().ok());
        EXPECT_TRUE((trx.insert(logicalCollection->name(), doc->slice(),
                                arangodb::OperationOptions())
                         .ok()));
        EXPECT_TRUE(trx.commit().ok());
      }

      // query
      {
        arangodb::transaction::Methods trx(
            arangodb::transaction::StandaloneContext::create(
                *vocbase, arangodb::transaction::OperationOriginTestCase{}),
            std::vector<std::string>{logicalCollection->name()}, EMPTY, EMPTY,
            arangodb::transaction::Options{});
        EXPECT_TRUE(trx.begin().ok());
        auto const cid = logicalCollection->id();
        arangodb::iresearch::ViewSnapshot::Links links;
        {
          auto guard = viewImpl->linksReadLock();
          links.emplace_back(viewImpl->linkLock(guard, cid));
        }
        auto* snapshot = makeViewSnapshot(
            trx, arangodb::iresearch::ViewSnapshotMode::SyncAndReplace,
            std::move(links), viewImpl, viewImpl->name());
        ASSERT_TRUE(snapshot != nullptr);
        EXPECT_EQ(i, snapshot->docs_count());
        EXPECT_TRUE(trx.commit().ok());
      }
    }
  }
}

TEST_F(IResearchViewDBServerTest, test_rename) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  // rename empty
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\" }");
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    TRI_vocbase_t vocbase(testDBInfo(server.server()));
    auto logicalCollection = vocbase.createCollection(collectionJson->slice());
    ASSERT_FALSE(!logicalCollection);
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .instantiate(view, vocbase, json->slice(), false)
                     .ok()));
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    EXPECT_EQ(std::string("testView"), view->name());

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->properties(builder,
                       arangodb::LogicalDataSource::Serialization::List);
      builder.close();
      EXPECT_TRUE(builder.slice().hasKey("name"));
      EXPECT_EQ(std::string("testView"),
                builder.slice().get("name").copyString());
    }

    EXPECT_EQ(TRI_ERROR_CLUSTER_UNSUPPORTED,
              view->rename("newName").errorNumber());

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->properties(builder,
                       arangodb::LogicalDataSource::Serialization::List);
      builder.close();
      EXPECT_TRUE(builder.slice().hasKey("name"));
      EXPECT_EQ(std::string("testView"),
                builder.slice().get("name").copyString());
    }

    arangodb::iresearch::IResearchLinkMock link{arangodb::IndexId{42},
                                                *logicalCollection};
    initLink(link);
    auto asyncLinkPtr = std::make_shared<
        arangodb::iresearch::IResearchLink::AsyncLinkPtr::element_type>(&link);
    EXPECT_TRUE(impl->link(asyncLinkPtr).ok());
  }

  // rename non-empty
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\" }");
    auto const viewId = std::to_string(
        ci.uniqid() +
        1);  // +1 because LogicalView creation will generate a new ID
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    TRI_vocbase_t vocbase(testDBInfo(server.server()));
    auto logicalCollection = vocbase.createCollection(collectionJson->slice());
    ASSERT_FALSE(!logicalCollection);
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .instantiate(view, vocbase, json->slice(), false)
                     .ok()));
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    // ensure we have shard view in vocbase
    arangodb::iresearch::IResearchLinkMock link{arangodb::IndexId{42},
                                                *logicalCollection};
    initLink(link);
    auto asyncLinkPtr = std::make_shared<
        arangodb::iresearch::IResearchLink::AsyncLinkPtr::element_type>(&link);
    EXPECT_TRUE(impl->link(asyncLinkPtr).ok());

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->properties(builder,
                       arangodb::LogicalDataSource::Serialization::List);
      builder.close();
      EXPECT_TRUE(builder.slice().hasKey("name"));
      EXPECT_EQ(std::string("testView"),
                builder.slice().get("name").copyString());
    }

    EXPECT_EQ(TRI_ERROR_CLUSTER_UNSUPPORTED,
              view->rename("newName").errorNumber());

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->properties(builder,
                       arangodb::LogicalDataSource::Serialization::List);
      builder.close();
      EXPECT_TRUE(builder.slice().hasKey("name"));
      EXPECT_EQ(std::string("testView"),
                builder.slice().get("name").copyString());
    }

    view->rename("testView");  // rename back or vocbase will be out of sync
  }
}

TEST_F(IResearchViewDBServerTest, test_toVelocyPack) {
  // base
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"unusedKey\": "
        "\"unusedValue\" }");
    TRI_vocbase_t vocbase(testDBInfo(server.server()));
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .instantiate(view, vocbase, json->slice(), false)
                     .ok()));
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->properties(builder, arangodb::LogicalDataSource::Serialization::List);
    builder.close();
    auto slice = builder.slice();
    EXPECT_EQ(4U, slice.length());
    EXPECT_TRUE((slice.hasKey("globallyUniqueId") &&
                 slice.get("globallyUniqueId").isString() &&
                 false == slice.get("globallyUniqueId").copyString().empty()));
    EXPECT_TRUE((slice.hasKey("id") && slice.get("id").isString() &&
                 std::string("1") == slice.get("id").copyString()));
    EXPECT_TRUE((slice.hasKey("name") && slice.get("name").isString() &&
                 std::string("testView") == slice.get("name").copyString()));
    EXPECT_TRUE((slice.hasKey("type") && slice.get("type").isString() &&
                 arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                     slice.get("type").copyString()));
  }

  // properties
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"unusedKey\": "
        "\"unusedValue\", \"storedValues\":[[], [\"\"], [\"\"], { "
        "\"fields\":[\"test.t\"], \"compression\":\"none\"}, [\"a.a\", "
        "\"b.b\"]] }");
    TRI_vocbase_t vocbase(testDBInfo(server.server()));
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .instantiate(view, vocbase, json->slice(), false)
                     .ok()));
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    arangodb::velocypack::Builder builder;

    builder.openObject();
    EXPECT_TRUE(
        view->properties(builder,
                         arangodb::LogicalDataSource::Serialization::Properties)
            .ok());
    builder.close();
    auto slice = builder.slice();
    EXPECT_EQ(15 + kEnterpriseFields, slice.length());
    EXPECT_TRUE((slice.hasKey("globallyUniqueId") &&
                 slice.get("globallyUniqueId").isString() &&
                 false == slice.get("globallyUniqueId").copyString().empty()));
    EXPECT_TRUE((slice.hasKey("id") && slice.get("id").isString() &&
                 std::string("2") == slice.get("id").copyString()));
    EXPECT_TRUE((slice.hasKey("name") && slice.get("name").isString() &&
                 std::string("testView") == slice.get("name").copyString()));
    EXPECT_TRUE((slice.hasKey("type") && slice.get("type").isString() &&
                 arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                     slice.get("type").copyString()));
    auto expectedStoredValue = arangodb::velocypack::Parser::fromJson(
        "[{ \"fields\":[\"test.t\"], \"compression\":\"none\"}, "
        "{\"fields\":[\"a.a\", \"b.b\"], \"compression\":\"lz4\"}]");
    EXPECT_TRUE(arangodb::basics::VelocyPackHelper::equal(
        expectedStoredValue->slice(), slice.get("storedValues"), true));
  }

  // persistence
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\", \"unusedKey\": "
        "\"unusedValue\", \"storedValues\":[[], [\"\"], [\"\"], { "
        "\"fields\":[\"test.t\"], \"compression\":\"none\"}, [\"a.a\", "
        "\"b.b\"]] }");
    TRI_vocbase_t vocbase(testDBInfo(server.server()));
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .instantiate(view, vocbase, json->slice(), false)
                     .ok()));
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    arangodb::velocypack::Builder builder;

    builder.openObject();
    EXPECT_TRUE(view->properties(
                        builder,
                        arangodb::LogicalDataSource::Serialization::Persistence)
                    .ok());
    builder.close();
    auto slice = builder.slice();
    EXPECT_EQ(19 + kEnterpriseFields, slice.length());
    EXPECT_TRUE(slice.hasKey("deleted") && slice.get("deleted").isBoolean() &&
                false == slice.get("deleted").getBoolean());
    EXPECT_TRUE(slice.hasKey("globallyUniqueId") &&
                slice.get("globallyUniqueId").isString() &&
                false == slice.get("globallyUniqueId").copyString().empty());
    EXPECT_TRUE(slice.hasKey("id") && slice.get("id").isString() &&
                std::string("3") == slice.get("id").copyString());
    EXPECT_TRUE(slice.hasKey("isSystem") && slice.get("isSystem").isBoolean() &&
                false == slice.get("isSystem").getBoolean());
    EXPECT_TRUE(slice.hasKey("name") && slice.get("name").isString() &&
                std::string("testView") == slice.get("name").copyString());
    EXPECT_TRUE(slice.hasKey("planId") && slice.get("planId").isString() &&
                std::string("3") == slice.get("planId").copyString());
    EXPECT_TRUE(slice.hasKey("type") && slice.get("type").isString() &&
                arangodb::iresearch::StaticStrings::ViewArangoSearchType ==
                    slice.get("type").copyString());
    EXPECT_TRUE(slice.hasKey("cleanupIntervalStep") &&
                slice.get("cleanupIntervalStep").isNumber());
    EXPECT_TRUE(slice.hasKey("commitIntervalMsec") &&
                slice.get("commitIntervalMsec").isNumber());
    EXPECT_TRUE(slice.hasKey("consolidationIntervalMsec") &&
                slice.get("consolidationIntervalMsec").isNumber());
    EXPECT_TRUE(slice.hasKey("version") && slice.get("version").isNumber());
    EXPECT_TRUE(slice.hasKey("consolidationPolicy") &&
                slice.get("consolidationPolicy").isObject());
    EXPECT_TRUE(slice.hasKey("primarySort") &&
                slice.get("primarySort").isArray());
    auto tmpSlice = slice.get("primarySortCompression");
    EXPECT_TRUE(tmpSlice.isString());
    EXPECT_EQ(std::string("lz4"), tmpSlice.copyString());
    EXPECT_TRUE(slice.hasKey("writebufferActive") &&
                slice.get("writebufferActive").isNumber());
    EXPECT_TRUE(slice.hasKey("writebufferIdle") &&
                slice.get("writebufferIdle").isNumber());
    EXPECT_TRUE(slice.hasKey("writebufferSizeMax") &&
                slice.get("writebufferSizeMax").isNumber());
    EXPECT_TRUE(slice.hasKey("collections") &&
                slice.get("collections").isArray());
    EXPECT_TRUE(slice.hasKey("storedValues") &&
                2 == slice.get("storedValues").length());
    auto expectedStoredValue = arangodb::velocypack::Parser::fromJson(
        "[{ \"fields\":[\"test.t\"], \"compression\":\"none\"}, "
        "{\"fields\":[\"a.a\", \"b.b\"], \"compression\":\"lz4\"}]");
    EXPECT_TRUE(arangodb::basics::VelocyPackHelper::equal(
        expectedStoredValue->slice(), slice.get("storedValues"), true));
  }
}

TEST_F(IResearchViewDBServerTest, test_transaction_snapshot) {
  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
  createTestDatabase(vocbase);

  static std::vector<std::string> const EMPTY;
  auto viewJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testView\", \"type\": \"arangosearch\", "
      "\"consolidationIntervalMsec\": 0 }");
  auto collectionJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"testCollection\" }");
  auto linkJson = arangodb::velocypack::Parser::fromJson(
      "{ \"view\": \"testView\", \"type\": \"arangosearch\", "
      "\"includeAllFields\": true }");
  auto logicalCollection = vocbase->createCollection(collectionJson->slice());
  ASSERT_NE(nullptr, logicalCollection);
  arangodb::LogicalView::ptr logicalview;
  ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                   .create(logicalview, *vocbase, viewJson->slice(), true)
                   .ok()));
  ASSERT_FALSE(!logicalview);
  auto* viewImpl =
      dynamic_cast<arangodb::iresearch::IResearchView*>(logicalview.get());
  ASSERT_NE(nullptr, viewImpl);

  bool created;
  auto index = logicalCollection->createIndex(linkJson->slice(), created).get();
  ASSERT_FALSE(!index);
  auto link =
      std::dynamic_pointer_cast<arangodb::iresearch::IResearchLinkMock>(index);
  ASSERT_FALSE(!link);

  // add a single document to view (do not sync)
  {
    auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
    arangodb::iresearch::IResearchLinkMeta meta;
    meta._includeAllFields = true;
    arangodb::transaction::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        EMPTY, std::vector<std::string>{logicalCollection->name()}, EMPTY,
        arangodb::transaction::Options());
    EXPECT_TRUE(trx.begin().ok());
    EXPECT_TRUE(
        (link->insert(trx, arangodb::LocalDocumentId(0), doc->slice()).ok()));
    EXPECT_TRUE(trx.commit().ok());
  }

  // no snapshot in TransactionState (force == false, waitForSync = false)
  {
    arangodb::transaction::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        std::vector<std::string>{logicalCollection->name()}, EMPTY, EMPTY,
        arangodb::transaction::Options());
    EXPECT_TRUE(trx.begin().ok());
    auto const cid = logicalCollection->id();
    arangodb::iresearch::ViewSnapshot::Links links;
    {
      auto guard = viewImpl->linksReadLock();
      links.emplace_back(viewImpl->linkLock(guard, cid));
    }
    auto* snapshot =
        makeViewSnapshot(trx, arangodb::iresearch::ViewSnapshotMode::Find,
                         std::move(links), viewImpl, viewImpl->name());
    EXPECT_TRUE(snapshot == nullptr);
    EXPECT_TRUE(trx.commit().ok());
  }

  // no snapshot in TransactionState (force == true, waitForSync = false)
  {
    arangodb::transaction::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        std::vector<std::string>{logicalCollection->name()}, EMPTY, EMPTY,
        arangodb::transaction::Options());
    EXPECT_TRUE(trx.begin().ok());
    auto const cid = logicalCollection->id();
    arangodb::iresearch::ViewSnapshot::Links links1, links2;
    {
      auto guard = viewImpl->linksReadLock();
      links1.emplace_back(viewImpl->linkLock(guard, cid));
      links2.emplace_back(viewImpl->linkLock(guard, cid));
    }
    auto* snapshot =
        makeViewSnapshot(trx, arangodb::iresearch::ViewSnapshotMode::Find,
                         std::move(links1), viewImpl, viewImpl->name());
    EXPECT_TRUE(snapshot == nullptr);
    snapshot = makeViewSnapshot(
        trx, arangodb::iresearch::ViewSnapshotMode::FindOrCreate,
        std::move(links2), viewImpl, viewImpl->name());
    ASSERT_TRUE(snapshot != nullptr);
    EXPECT_EQ(snapshot->live_docs_count(), 0);
    EXPECT_TRUE(trx.commit().ok());
  }

  // no snapshot in TransactionState (force == false, waitForSync = true)
  {
    arangodb::transaction::Options opts;
    opts.waitForSync = true;
    arangodb::transaction::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        std::vector<std::string>{logicalCollection->name()}, EMPTY, EMPTY,
        opts);
    EXPECT_TRUE(trx.begin().ok());
    auto const cid = logicalCollection->id();
    arangodb::iresearch::ViewSnapshot::Links links;
    {
      auto guard = viewImpl->linksReadLock();
      links.emplace_back(viewImpl->linkLock(guard, cid));
    }
    auto* snapshot =
        makeViewSnapshot(trx, arangodb::iresearch::ViewSnapshotMode::Find,
                         std::move(links), viewImpl, viewImpl->name());
    EXPECT_TRUE(snapshot == nullptr);
    EXPECT_TRUE(trx.commit().ok());
  }

  // no snapshot in TransactionState (force == true, waitForSync = true)
  {
    arangodb::transaction::Options opts;
    arangodb::transaction::Methods trx(
        arangodb::transaction::StandaloneContext::create(
            *vocbase, arangodb::transaction::OperationOriginTestCase{}),
        std::vector<std::string>{logicalCollection->name()}, EMPTY, EMPTY,
        opts);
    EXPECT_TRUE(trx.begin().ok());
    auto const cid = logicalCollection->id();
    arangodb::iresearch::ViewSnapshot::Links links1, links2, links3, links4;
    {
      auto guard = viewImpl->linksReadLock();
      links1.emplace_back(viewImpl->linkLock(guard, cid));
      links2.emplace_back(viewImpl->linkLock(guard, cid));
      links3.emplace_back(viewImpl->linkLock(guard, cid));
      links4.emplace_back(viewImpl->linkLock(guard, cid));
    }
    auto* snapshot =
        makeViewSnapshot(trx, arangodb::iresearch::ViewSnapshotMode::Find,
                         std::move(links1), viewImpl, viewImpl->name());
    EXPECT_TRUE(snapshot == nullptr);
    snapshot = makeViewSnapshot(
        trx, arangodb::iresearch::ViewSnapshotMode::SyncAndReplace,
        std::move(links2), viewImpl, viewImpl->name());
    ASSERT_TRUE(snapshot != nullptr);
    auto* snapshotFind =
        makeViewSnapshot(trx, arangodb::iresearch::ViewSnapshotMode::Find,
                         std::move(links3), viewImpl, viewImpl->name());
    EXPECT_TRUE(snapshotFind == snapshot);
    auto* snapshotCreate = makeViewSnapshot(
        trx, arangodb::iresearch::ViewSnapshotMode::FindOrCreate,
        std::move(links4), viewImpl, viewImpl->name());
    EXPECT_TRUE(snapshotCreate == snapshot);
    EXPECT_EQ(snapshot->live_docs_count(), 1);
    EXPECT_TRUE(trx.commit().ok());
  }
}

TEST_F(IResearchViewDBServerTest, test_updateProperties) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  // update empty (partial)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\" }");
    auto viewJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"42\", \"name\": \"testView\", \"type\": \"arangosearch\", "
        "\"collections\": [ 3, 4, 5 ], \"cleanupIntervalStep\": 24, "
        "\"consolidationIntervalMsec\": 42 }");
    TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
    createTestDatabase(vocbase, "testDatabase" IRS_TO_STRING(__LINE__));
    auto logicalCollection = vocbase->createCollection(collectionJson->slice());
    EXPECT_NE(nullptr, logicalCollection);
    EXPECT_TRUE(
        (ci.createViewCoordinator(vocbase->name(), "42", viewJson->slice())
             .ok()));
    auto view = ci.getView(vocbase->name(),
                           "42");  // link creation requires cluster-view to be
                                   // in ClusterInfo instead of TRI_vocbase_t
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   24 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           42 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   0 == slice.get("links").length()));
    }

    {
      auto update = arangodb::velocypack::Parser::fromJson(
          "{ \"collections\": [ 6, 7, 8, 9 ], \"consolidationIntervalMsec\": "
          "52, \"links\": { \"testCollection\": {} } }");
      EXPECT_TRUE(view->properties(update->slice(), true, true).ok());
    }

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   24 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   1 == slice.get("links").length()));
    }

    EXPECT_TRUE((false == !arangodb::iresearch::IResearchLinkHelper::find(
                              *logicalCollection, *view)));
    static auto visitor = [](arangodb::DataSourceId,
                             arangodb::LogicalView::Indexes*) { return false; };
    EXPECT_FALSE(view->visitCollections(visitor));  // no collections in view

    // not for persistence
    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   24 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   1 == slice.get("links").length()));
    }

    // for persistence
    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Persistence)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(19 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("collections") &&
                   slice.get("collections").isArray() &&
                   1 == slice.get("collections").length()));
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   24 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_FALSE(slice.hasKey("links"));
    }
  }

  // update empty (full)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\" }");
    auto viewJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"42\", \"name\": \"testView\", \"type\": \"arangosearch\", "
        "\"collections\": [ 3, 4, 5 ], \"cleanupIntervalStep\": 24, "
        "\"consolidationIntervalMsec\": 42 }");
    TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
    createTestDatabase(vocbase, "testDatabase" IRS_TO_STRING(__LINE__));
    ASSERT_NE(nullptr, vocbase);
    auto logicalCollection = vocbase->createCollection(collectionJson->slice());
    EXPECT_NE(nullptr, logicalCollection);
    EXPECT_TRUE(
        (ci.createViewCoordinator(vocbase->name(), "42", viewJson->slice())
             .ok()));
    auto view = ci.getView(vocbase->name(),
                           "42");  // link creation requires cluster-view to be
                                   // in ClusterInfo instead of TRI_vocbase_t
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   24 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           42 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   0 == slice.get("links").length()));
    }

    {
      auto update = arangodb::velocypack::Parser::fromJson(
          "{ \"collections\": [ 6, 7, 8, 9 ], \"links\": { \"testCollection\": "
          "{} }, \"consolidationIntervalMsec\": 52 }");
      EXPECT_TRUE(view->properties(update->slice(), true, false).ok());
    }

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   2 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   1 == slice.get("links").length()));
    }

    EXPECT_TRUE((false == !arangodb::iresearch::IResearchLinkHelper::find(
                              *logicalCollection, *view)));
    static auto visitor = [](arangodb::DataSourceId,
                             arangodb::LogicalView::Indexes*) { return false; };
    EXPECT_FALSE(view->visitCollections(visitor));  // no collections in view

    // not for persistence
    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   2 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   1 == slice.get("links").length()));
    }

    // for persistence
    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Persistence)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(19 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("collections") &&
                   slice.get("collections").isArray() &&
                   1 == slice.get("collections").length()));
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   2 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_FALSE(slice.hasKey("links"));
    }
  }

  // update non-empty (partial)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\" }");
    auto linkJson = arangodb::velocypack::Parser::fromJson(
        "{ \"view\": \"testView\", \"type\": \"arangosearch\", "
        "\"includeAllFields\": true }");
    auto viewJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"42\", \"name\": \"testView\", \"type\": \"arangosearch\", "
        "\"collections\": [ 3, 4, 5 ], \"cleanupIntervalStep\": 24, "
        "\"consolidationIntervalMsec\": 42 }");
    TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
    createTestDatabase(vocbase, "testDatabase" IRS_TO_STRING(__LINE__));
    ASSERT_NE(nullptr, vocbase);
    auto logicalCollection = vocbase->createCollection(collectionJson->slice());
    EXPECT_NE(nullptr, logicalCollection);
    EXPECT_TRUE(
        (ci.createViewCoordinator(vocbase->name(), "42", viewJson->slice())
             .ok()));
    auto view = ci.getView(vocbase->name(),
                           "42");  // link creation requires cluster-view to be
                                   // in ClusterInfo instead of TRI_vocbase_t
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    bool created;
    auto index =
        logicalCollection->createIndex(linkJson->slice(), created).get();
    ASSERT_FALSE(!index);
    auto link =
        std::dynamic_pointer_cast<arangodb::iresearch::IResearchLinkMock>(
            index);
    ASSERT_FALSE(!link);
    static auto visitor = [](arangodb::DataSourceId,
                             arangodb::LogicalView::Indexes*) { return false; };
    EXPECT_FALSE(view->visitCollections(visitor));  // 1 collection in view

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   24 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           42 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   1 == slice.get("links").length()));
    }

    {
      auto update = arangodb::velocypack::Parser::fromJson(
          "{ \"collections\": [ 6, 7, 8 ], \"links\": { \"testCollection\": {} "
          "}, \"consolidationIntervalMsec\": 52 }");
      EXPECT_TRUE(view->properties(update->slice(), true, true).ok());
    }

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   24 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   1 == slice.get("links").length()));
    }

    // not for persistence
    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   24 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   1 == slice.get("links").length()));
    }

    // for persistence
    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Persistence)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(19 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("collections") &&
                   slice.get("collections").isArray() &&
                   1 == slice.get("collections").length()));
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   24 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_FALSE(slice.hasKey("links"));
    }
  }

  // update non-empty (full)
  {
    auto collection0Json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\" }");
    auto collection1Json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection1\", \"id\": \"123\" }");
    auto linkJson = arangodb::velocypack::Parser::fromJson(
        "{ \"view\": \"testView\", \"type\": \"arangosearch\", "
        "\"includeAllFields\": true }");
    auto viewJson = arangodb::velocypack::Parser::fromJson(
        "{ \"id\": \"42\", \"name\": \"testView\", \"type\": \"arangosearch\", "
        "\"collections\": [ 3, 4, 5 ], \"cleanupIntervalStep\": 24, "
        "\"consolidationIntervalMsec\": 42 }");
    TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
    createTestDatabase(vocbase, "testDatabase" IRS_TO_STRING(__LINE__));
    ASSERT_NE(nullptr, vocbase);
    auto logicalCollection0 =
        vocbase->createCollection(collection0Json->slice());
    EXPECT_NE(nullptr, logicalCollection0);
    auto logicalCollection1 =
        vocbase->createCollection(collection1Json->slice());
    EXPECT_NE(nullptr, logicalCollection1);
    EXPECT_TRUE(
        (ci.createViewCoordinator(vocbase->name(), "42", viewJson->slice())
             .ok()));
    auto view = ci.getView(vocbase->name(),
                           "42");  // link creation requires cluster-view to be
                                   // in ClusterInfo instead of TRI_vocbase_t
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    bool created;
    auto index =
        logicalCollection1->createIndex(linkJson->slice(), created).get();
    ASSERT_FALSE(!index);
    auto link =
        std::dynamic_pointer_cast<arangodb::iresearch::IResearchLinkMock>(
            index);
    ASSERT_FALSE(!link);
    static auto visitor = [](arangodb::DataSourceId,
                             arangodb::LogicalView::Indexes*) { return false; };
    EXPECT_FALSE(view->visitCollections(visitor));  // 1 collection in view

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   24 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           42 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   1 == slice.get("links").length()));
    }

    {
      auto update = arangodb::velocypack::Parser::fromJson(
          "{ \"collections\": [ 6, 7, 8 ], \"links\": { \"testCollection\": {} "
          "}, \"consolidationIntervalMsec\": 52 }");
      EXPECT_TRUE(view->properties(update->slice(), true, false).ok());
    }

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   2 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   1 == slice.get("links").length()));
    }

    // not for persistence
    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Properties)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(15 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   2 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_TRUE((slice.hasKey("links") && slice.get("links").isObject() &&
                   1 == slice.get("links").length()));
    }

    // for persistence
    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      EXPECT_TRUE(
          view->properties(
                  builder,
                  arangodb::LogicalDataSource::Serialization::Persistence)
              .ok());
      builder.close();

      auto slice = builder.slice();
      EXPECT_TRUE(slice.isObject());
      EXPECT_EQ(19 + kEnterpriseFields, slice.length());
      EXPECT_TRUE((
          slice.hasKey("collections") && slice.get("collections").isArray() &&
          2 == slice.get("collections").length()));  // list of links is not
                                                     // modified after link drop
      EXPECT_TRUE((slice.hasKey("cleanupIntervalStep") &&
                   slice.get("cleanupIntervalStep").isNumber<size_t>() &&
                   2 == slice.get("cleanupIntervalStep").getNumber<size_t>()));
      EXPECT_TRUE(
          (slice.hasKey("consolidationIntervalMsec") &&
           slice.get("consolidationIntervalMsec").isNumber<size_t>() &&
           52 == slice.get("consolidationIntervalMsec").getNumber<size_t>()));
      EXPECT_FALSE(slice.hasKey("links"));
    }
  }
}

TEST_F(IResearchViewDBServerTest, test_visitCollections) {
  auto& ci = server.getFeature<arangodb::ClusterFeature>().clusterInfo();

  // visit empty
  {
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    TRI_vocbase_t vocbase(testDBInfo(server.server()));
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .instantiate(view, vocbase, json->slice(), false)
                     .ok()));
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    static auto visitor = [](arangodb::DataSourceId,
                             arangodb::LogicalView::Indexes*) { return false; };
    EXPECT_TRUE(view->visitCollections(visitor));  // no collections in view
  }

  // visit non-empty
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testCollection\" }");
    auto linkJson = arangodb::velocypack::Parser::fromJson(
        "{ \"view\": \"testView\", \"includeAllFields\": true }");
    auto const viewId = std::to_string(
        ci.uniqid() +
        1);  // +1 because LogicalView creation will generate a new ID
    auto json = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    TRI_vocbase_t vocbase(testDBInfo(server.server()));
    auto logicalCollection = vocbase.createCollection(collectionJson->slice());
    ASSERT_FALSE(!logicalCollection);
    arangodb::LogicalView::ptr view;
    ASSERT_TRUE((arangodb::iresearch::IResearchView::factory()
                     .instantiate(view, vocbase, json->slice(), false)
                     .ok()));
    EXPECT_FALSE(!view);
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
    EXPECT_NE(nullptr, impl);

    // ensure we have shard view in vocbase
    arangodb::iresearch::IResearchLinkMock link{arangodb::IndexId{42},
                                                *logicalCollection};
    initLink(link);
    auto asyncLinkPtr = std::make_shared<
        arangodb::iresearch::IResearchLink::AsyncLinkPtr::element_type>(&link);
    EXPECT_TRUE(impl->link(asyncLinkPtr).ok());

    std::set<arangodb::DataSourceId> cids = {logicalCollection->id()};
    static auto visitor = [&cids](arangodb::DataSourceId cid,
                                  arangodb::LogicalView::Indexes*) {
      return 1 == cids.erase(cid);
    };
    EXPECT_TRUE(view->visitCollections(visitor));  // all collections expected
    EXPECT_TRUE(cids.empty());
    EXPECT_TRUE(impl->unlink(logicalCollection->id()).ok());
    EXPECT_TRUE(view->visitCollections(visitor));  // no collections in view
  }
}
