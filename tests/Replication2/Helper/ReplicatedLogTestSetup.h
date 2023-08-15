////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2023-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Replication2/Mocks/DelayedLogFollower.h"
#include "Replication2/Mocks/FakeAsyncExecutor.h"
#include "Replication2/Mocks/FakeFollowerFactory.h"
#include "Replication2/Mocks/FakeReplicatedState.h"
#include "Replication2/Mocks/FakeStorageEngineMethods.h"
#include "Replication2/Mocks/RebootIdCacheMock.h"
#include "Replication2/Mocks/ReplicatedLogMetricsMock.h"
#include "Replication2/Mocks/ReplicatedStateHandleMock.h"
#include "Replication2/Mocks/ReplicatedStateMetricsMock.h"
#include "Replication2/Mocks/SchedulerMocks.h"

#include "Replication2/ReplicatedLog/ILogInterfaces.h"
#include "Replication2/ReplicatedState/StateCommon.h"

namespace arangodb::replication2::test {

auto inline operator"" _Lx(unsigned long long x) -> LogIndex {
  return LogIndex{x};
}
auto inline operator"" _T(unsigned long long x) -> LogTerm {
  return LogTerm{x};
}

struct LogArguments {
  std::variant<LogRange, std::vector<LogPayload>> initialLogRange = {};
  // set persistedMetadata to std::nullopt to simulate a read failure.
  // `.stateId` will be set by makeLogWithFakes(), but is of no consequence
  // anyway.
  std::optional<storage::PersistedStateInfo> persistedMetadata =
      storage::PersistedStateInfo{
          .snapshot = {.status = replicated_state::SnapshotStatus::kCompleted}};

  std::shared_ptr<ReplicatedLogGlobalSettings> options =
      std::make_shared<ReplicatedLogGlobalSettings>();
};
struct ConfigArguments {
  LogTerm term = LogTerm{1};
  std::size_t writeConcern = 1;
  bool waitForSync = false;
};
struct LogWithFakes;
struct LogConfig {
  std::reference_wrapper<LogWithFakes> leader;
  std::vector<std::reference_wrapper<LogWithFakes>> followers;
  agency::LogPlanTermSpecification termSpec;
  agency::ParticipantsConfig participantsConfig;

  void installConfig(bool establishLeadership = false);
};

struct LogWithFakes : IHasScheduler {
  LogWithFakes(LogId logId, agency::ServerInstanceReference serverId,
               LoggerContext loggerContext,
               std::shared_ptr<replicated_log::ReplicatedLogMetrics> logMetrics,
               LogArguments fakeArguments)
      : loggerContext(std::move(loggerContext)),
        logId(logId),
        serverInstance(std::move(serverId)),
        logMetrics(std::move(logMetrics)),
        storageContext(
            std::make_shared<storage::test::FakeStorageEngineMethodsContext>(
                12, gid.id, storageExecutor, fakeArguments.initialLogRange,
                std::move(fakeArguments.persistedMetadata))),
        options(std::move(fakeArguments.options)) {}

  [[nodiscard]] auto updateConfig(LogConfig conf) {
    if (&conf.leader.get() == this) {
      stateHandleMock->expectLeader();
      for (auto const it : conf.followers) {
        auto& followerContainer = it.get();
        fakeFollowerFactory->followerThunks.try_emplace(
            followerContainer.serverInstance.serverId, [&followerContainer]() {
              // Note: Follower instances must have their config installed
              // already for this to work.
              // TODO catch exception and print an error?
              followerContainer.delayedLogFollower =
                  std::make_shared<DelayedLogFollower>(
                      followerContainer.getAsFollower());
              return followerContainer.delayedLogFollower;
            });
      }
    } else {
      stateHandleMock->expectFollower();
    }
    return log->updateConfig(std::move(conf.termSpec),
                             std::move(conf.participantsConfig),
                             serverInstance);
  }
  [[nodiscard]] auto getAsLeader() {
    auto const leader = std::dynamic_pointer_cast<replicated_log::ILogLeader>(
        log->getParticipant());
    EXPECT_NE(leader, nullptr);
    return leader;
  }
  [[nodiscard]] auto getAsFollower()
      -> std::shared_ptr<replicated_log::ILogFollower> {
    auto const follower =
        std::dynamic_pointer_cast<replicated_log::ILogFollower>(
            log->getParticipant());
    EXPECT_NE(follower, nullptr);
    return follower;
  }

  auto runAll() noexcept -> std::size_t override {
    if (delayedLogFollower == nullptr) {
      return IHasScheduler::runAll(logScheduler, storageExecutor);
    } else {
      return IHasScheduler::runAll(logScheduler, storageExecutor,
                                   delayedLogFollower->scheduler);
    }
  }
  auto hasWork() const noexcept -> bool override {
    if (delayedLogFollower == nullptr) {
      return IHasScheduler::hasWork(logScheduler, storageExecutor);
    } else {
      return IHasScheduler::hasWork(logScheduler, storageExecutor,
                                    delayedLogFollower->scheduler);
    }
  }

  LoggerContext loggerContext;
  LogId const logId;
  agency::ServerInstanceReference const serverInstance;
  std::shared_ptr<replicated_log::ReplicatedLogMetrics> logMetrics;

  GlobalLogIdentifier gid = GlobalLogIdentifier("db", logId);

  std::shared_ptr<storage::rocksdb::test::DelayedExecutor> storageExecutor =
      std::make_shared<storage::rocksdb::test::DelayedExecutor>();
  // Note that this purposefully does not initialize the PersistedStateInfo that
  // is returned by the StorageEngineMethods. readMetadata() will return a
  // document not found error unless you initialize it in your test.
  std::shared_ptr<storage::test::FakeStorageEngineMethodsContext>
      storageContext;
  storage::IStorageEngineMethods* methodsPtr =
      storageContext->getMethods().release();
  std::shared_ptr<ReplicatedLogGlobalSettings> options;

  std::shared_ptr<DelayedScheduler> logScheduler =
      std::make_shared<DelayedScheduler>();
  std::shared_ptr<test::RebootIdCacheMock> rebootIdCache =
      std::make_shared<testing::NiceMock<test::RebootIdCacheMock>>();
  std::shared_ptr<test::FakeFollowerFactory> fakeFollowerFactory =
      std::make_shared<FakeFollowerFactory>();
  std::shared_ptr<replicated_log::DefaultParticipantsFactory>
      participantsFactory =
          std::make_shared<replicated_log::DefaultParticipantsFactory>(
              fakeFollowerFactory, logScheduler, rebootIdCache);

  std::shared_ptr<replicated_log::ReplicatedLog> log =
      std::make_shared<replicated_log::ReplicatedLog>(
          std::unique_ptr<storage::IStorageEngineMethods>{methodsPtr},
          logMetrics, options, participantsFactory, loggerContext,
          serverInstance);

  test::ReplicatedStateHandleMock* stateHandleMock =
      new test::ReplicatedStateHandleMock();
  replicated_log::ReplicatedLogConnection connection = log->connect(
      std::unique_ptr<replicated_log::IReplicatedStateHandle>(stateHandleMock));

  std::shared_ptr<DelayedLogFollower> delayedLogFollower = nullptr;
};

struct ReplicatedLogTest : ::testing::Test {
  testing::TestInfo const* const testInfo =
      testing::UnitTest::GetInstance()->current_test_info();
  constexpr static char gtestStr[] = "gtest";
  LoggerContext loggerContext =
      LoggerContext(Logger::REPLICATION2)
          .with<gtestStr>(fmt::format("{}.{}", testInfo->test_suite_name(),
                                      testInfo->test_case_name()));

  std::shared_ptr<test::ReplicatedLogMetricsMock> logMetricsMock =
      std::make_shared<test::ReplicatedLogMetricsMock>();
  // std::shared_ptr<replication2::tests::ReplicatedStateMetricsMock>
  //     stateMetricsMock =
  //         std::make_shared<replication2::tests::ReplicatedStateMetricsMock>(
  //             "foo");

  std::uint64_t nextId = 1;

  auto makeLogWithFakes(LogArguments fakeArguments) {
    auto const id = nextId++;
    auto logId = LogId{id};
    auto serverId = agency::ServerInstanceReference{fmt::format("dbs{:02}", id),
                                                    RebootId{1}};
    if (fakeArguments.persistedMetadata.has_value()) {
      fakeArguments.persistedMetadata->stateId = logId;
    }
    return LogWithFakes(logId, serverId, loggerContext, logMetricsMock,
                        std::move(fakeArguments));
  }

  auto makeConfig(LogWithFakes& leader,
                  std::vector<std::reference_wrapper<LogWithFakes>> follower,
                  ConfigArguments configArguments) {
    auto logConfig = agency::LogPlanConfig{};
    auto participants = agency::ParticipantsFlagsMap{};
    auto const logToParticipant = [&](LogWithFakes& f) {
      return std::pair(f.serverInstance.serverId, ParticipantFlags{});
    };
    participants.emplace(logToParticipant(leader));
    std::transform(follower.begin(), follower.end(),
                   std::inserter(participants, participants.end()),
                   logToParticipant);
    auto participantsConfig = agency::ParticipantsConfig{
        .participants = participants, .config = logConfig};
    auto logSpec = agency::LogPlanTermSpecification{configArguments.term,
                                                    leader.serverInstance};
    return LogConfig{.leader = leader,
                     .followers = std::move(follower),
                     .termSpec = std::move(logSpec),
                     .participantsConfig = std::move(participantsConfig)};
  }

  template<std::size_t replicationFactor>
  requires requires { replicationFactor >= 1; }
  auto createLogs(LogConfig config)
      -> std::pair<LogWithFakes,
                   std::array<LogWithFakes, replicationFactor - 1>> {
    auto leader = makeLogWithFakes({});
    auto logs = std::array<LogWithFakes, replicationFactor>{};
    for (auto&& [i, log] : enumerate(logs)) {
      auto& follower = config.followers[i];
      log = makeLogWithFakes({});
    }
    return logs;
  }
};

}  // namespace arangodb::replication2::test

// TODO Maybe move the rest of this file into a separate header
namespace arangodb::replication2 {
void PrintTo(LogEntry const& entry, std::ostream* os);
}  // namespace arangodb::replication2

namespace arangodb::replication2::test {
// Allows matching a log entry partially in gtest EXPECT_THAT. Members set to
// std::nullopt are ignored when matching; only the set members are matched.
struct PartialLogEntry {
  std::optional<LogTerm> term{};
  std::optional<LogIndex> index{};
  // Note: Add more (optional) fields to IsMeta/IsPayload as needed;
  // then match them in MatchesMapLogEntry, and print them in PrintTo.
  struct IsMeta {};
  struct IsPayload {
    // Taking a string should suffice for the tests. If you need e.g. velocypack
    // instead, make it a variant (and add a nullopt as well).
    std::optional<std::string> payload{};
  };
  std::variant<std::nullopt_t, IsMeta, IsPayload> payload = std::nullopt;

  friend void PrintTo(PartialLogEntry const& point, std::ostream* os);
};
using PartialLogEntries = std::initializer_list<PartialLogEntry>;

MATCHER_P2(IsTermIndexPair, term, index, "") {
  return arg.term == term and arg.index == index;
}

// Matches a map entry pair (LogIndex, LogEntry) against a PartialLogEntry.
MATCHER(MatchesMapLogEntry,
        fmt::format("{} log entries", negation ? "doesn't match" : "matches")) {
  auto const& logIndex = std::get<0>(arg).first;
  auto const& logEntry = std::get<0>(arg).second;
  auto const& partialLogEntry = std::get<1>(arg);
  return (not partialLogEntry.term.has_value() or
          partialLogEntry.term == logEntry.logTerm()) and
         (not partialLogEntry.index.has_value() or
          (partialLogEntry.index == logIndex and
           partialLogEntry.index == logEntry.logIndex())) and
         (std::visit(
             overload{
                 [](std::nullopt_t) { return true; },
                 [&](PartialLogEntry::IsPayload const& payload) {
                   return logEntry.hasPayload() &&
                          (!payload.payload.has_value() ||
                           (logEntry.logPayload()->slice().isString() &&
                            payload.payload ==
                                logEntry.logPayload()->slice().stringView()));
                 },
                 [&](PartialLogEntry::IsMeta const& payload) {
                   return logEntry.hasMeta();
                 },
             },
             partialLogEntry.payload));
}
}  // namespace arangodb::replication2::test
