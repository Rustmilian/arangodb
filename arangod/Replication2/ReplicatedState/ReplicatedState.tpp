////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ReplicatedState.h"

#include <string>
#include <unordered_map>
#include <utility>

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include "Logger/LogContextKeys.h"
#include "Metrics/Gauge.h"
#include "Replication2/Exceptions/ParticipantResignedException.h"
#include "Replication2/MetricsHelper.h"
#include "Replication2/ReplicatedLog/InMemoryLog.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"
#include "Replication2/ReplicatedState/LazyDeserializingIterator.h"
#include "Replication2/ReplicatedState/PersistedStateInfo.h"
#include "Replication2/ReplicatedState/ReplicatedStateMetrics.h"
#include "Replication2/ReplicatedState/StateInterfaces.h"
#include "Replication2/Streams/StreamSpecification.h"
#include "Replication2/Streams/Streams.h"
#include "Scheduler/SchedulerFeature.h"

namespace arangodb::replication2::replicated_state {

template<typename S>
ReplicatedStateManager<S>::ReplicatedStateManager(
    LoggerContext loggerContext,
    std::shared_ptr<ReplicatedStateMetrics> metrics,
    std::unique_ptr<CoreType> logCore, std::shared_ptr<Factory> factory)
    : _loggerContext(std::move(loggerContext)),
      _metrics(std::move(metrics)),
      _factory(std::move(factory)),
      _guarded{std::make_shared<UnconfiguredStateManager<S>>(
          _loggerContext.with<logContextKeyStateRole>(
              static_strings::StringUnconfigured),
          std::move(logCore))} {}

template<typename S>
void ReplicatedStateManager<S>::acquireSnapshot(ServerID leader,
                                                LogIndex commitIndex) {
  auto guard = _guarded.getLockedGuard();

  std::visit(overload{
                 [&](std::shared_ptr<FollowerStateManager<S>>& manager) {
                   LOG_CTX("52a11", DEBUG, _loggerContext)
                       << "try to acquire a new snapshot, starting at "
                       << commitIndex;
                   manager->acquireSnapshot(leader, commitIndex);
                 },
                 [](auto&) {
                   ADB_PROD_ASSERT(false)
                       << "State is not a follower (or uninitialized), but "
                          "acquireSnapshot is called";
                 },
             },
             guard->_currentManager);
}

template<typename S>
void ReplicatedStateManager<S>::updateCommitIndex(LogIndex index) {
  auto guard = _guarded.getLockedGuard();

  std::visit(overload{
                 [index](auto& manager) {
                   // temporary hack: post on the scheduler to avoid deadlocks
                   // with the log
                   auto& scheduler = *SchedulerFeature::SCHEDULER;
                   scheduler.queue(
                       RequestLane::CLUSTER_INTERNAL,
                       [weak = manager->weak_from_this(), index]() mutable {
                         if (auto manager = weak.lock(); manager != nullptr) {
                           manager->updateCommitIndex(index);
                         }
                       });
                 },
                 [](std::shared_ptr<UnconfiguredStateManager<S>>& manager) {
                   ADB_PROD_ASSERT(false) << "update commit index called on "
                                             "an unconfigured state manager";
                 },
             },
             guard->_currentManager);
}

template<typename S>
auto ReplicatedStateManager<S>::resignCurrentState() noexcept
    -> std::unique_ptr<replicated_log::IReplicatedLogMethodsBase> {
  auto guard = _guarded.getLockedGuard();
  auto&& [core, methods] =
      std::visit([](auto& manager) { return std::move(*manager).resign(); },
                 guard->_currentManager);
  // TODO Is it allowed to happen that resign() is called on an unconfigured
  //      state?
  ADB_PROD_ASSERT(
      std::holds_alternative<std::shared_ptr<UnconfiguredStateManager<S>>>(
          guard->_currentManager) == (methods == nullptr));
  guard->_currentManager
      .template emplace<std::shared_ptr<UnconfiguredStateManager<S>>>(
          std::make_shared<UnconfiguredStateManager<S>>(
              _loggerContext.template with<logContextKeyStateRole>(
                  static_strings::StringUnconfigured),
              std::move(core)));
  return std::move(methods);
}

template<typename S>
void ReplicatedStateManager<S>::leadershipEstablished(
    std::unique_ptr<replicated_log::IReplicatedLogLeaderMethods> methods) {
  auto guard = _guarded.getLockedGuard();
  ADB_PROD_ASSERT(
      std::holds_alternative<std::shared_ptr<UnconfiguredStateManager<S>>>(
          guard->_currentManager));
  auto&& [core, oldMethods] =
      std::move(*std::get<std::shared_ptr<UnconfiguredStateManager<S>>>(
                    guard->_currentManager))
          .resign();
  ADB_PROD_ASSERT(oldMethods == nullptr);
  auto stream = std::make_shared<
      ProducerStreamProxy<EntryType, Deserializer, Serializer>>(
      std::move(methods));
  auto leaderState = _factory->constructLeader(std::move(core));
  // TODO Pass the stream during construction already, and delete the
  //      "setStream" method; after that, the leader state implementation can
  //      also really rely on the stream being there.
  leaderState->setStream(stream);
  auto& manager =
      guard->_currentManager
          .template emplace<std::shared_ptr<LeaderStateManager<S>>>(
              std::make_shared<LeaderStateManager<S>>(
                  _loggerContext.template with<logContextKeyStateRole>(
                      static_strings::StringLeader),
                  _metrics, std::move(leaderState), std::move(stream)));

  // temporary hack: post on the scheduler to avoid deadlocks with the log
  auto& scheduler = *SchedulerFeature::SCHEDULER;
  scheduler.queue(RequestLane::CLUSTER_INTERNAL,
                  [weak = manager->weak_from_this()]() mutable {
                    if (auto manager = weak.lock(); manager != nullptr) {
                      manager->recoverEntries();
                    }
                  });
}

template<typename S>
void ReplicatedStateManager<S>::becomeFollower(
    std::unique_ptr<replicated_log::IReplicatedLogFollowerMethods> methods) {
  auto guard = _guarded.getLockedGuard();
  ADB_PROD_ASSERT(
      std::holds_alternative<std::shared_ptr<UnconfiguredStateManager<S>>>(
          guard->_currentManager));
  auto&& [core, oldMethods] =
      std::move(*std::get<std::shared_ptr<UnconfiguredStateManager<S>>>(
                    guard->_currentManager))
          .resign();
  ADB_PROD_ASSERT(oldMethods == nullptr);

  auto followerState = _factory->constructFollower(std::move(core));
  auto stream = std::make_shared<StreamProxy<EntryType, Deserializer>>(
      std::move(methods));
  followerState->setStream(stream);
  auto stateManager = std::make_shared<FollowerStateManager<S>>(
      _loggerContext.template with<logContextKeyStateRole>(
          static_strings::StringFollower),
      _metrics, followerState, std::move(stream));
  followerState->setStateManager(stateManager);
  guard->_currentManager
      .template emplace<std::shared_ptr<FollowerStateManager<S>>>(
          std::move(stateManager));
}

template<typename S>
void ReplicatedStateManager<S>::dropEntries() {
  ADB_PROD_ASSERT(false);
}

template<typename S>
auto ReplicatedStateManager<S>::getStatus() const
    -> std::optional<StateStatus> {
  auto guard = _guarded.getLockedGuard();
  auto status = std::visit(
      overload{[](auto const& manager) { return manager->getStatus(); }},
      guard->_currentManager);

  return status;
}

template<typename S>
auto ReplicatedStateManager<S>::getFollower() const
    -> std::shared_ptr<IReplicatedFollowerStateBase> {
  auto guard = _guarded.getLockedGuard();
  return std::visit(
      overload{
          [](std::shared_ptr<FollowerStateManager<S>> const& manager) {
            return std::dynamic_pointer_cast<IReplicatedFollowerStateBase>(
                manager->getStateMachine());
          },
          [](auto const&) -> std::shared_ptr<IReplicatedFollowerStateBase> {
            return nullptr;
          }},
      guard->_currentManager);
}

template<typename S>
auto ReplicatedStateManager<S>::getLeader() const
    -> std::shared_ptr<IReplicatedLeaderStateBase> {
  auto guard = _guarded.getLockedGuard();
  return std::visit(
      overload{[](std::shared_ptr<LeaderStateManager<S>> const& manager) {
                 return std::dynamic_pointer_cast<IReplicatedLeaderStateBase>(
                     manager->getStateMachine());
               },
               [](auto const&) -> std::shared_ptr<IReplicatedLeaderStateBase> {
                 return nullptr;
               }},
      guard->_currentManager);
}

template<typename S>
auto ReplicatedStateManager<S>::resign() && -> std::unique_ptr<CoreType> {
  auto guard = _guarded.getLockedGuard();
  auto&& [core, methods] =
      std::visit([](auto&& mgr) { return std::move(*mgr).resign(); },
                 guard->_currentManager);
  // we should be unconfigured already
  TRI_ASSERT(methods == nullptr);
  return std::move(core);
}

template<typename EntryType, typename Deserializer,
         template<typename> typename Interface, typename ILogMethodsT>
auto StreamProxy<EntryType, Deserializer, Interface,
                 ILogMethodsT>::waitForIterator(LogIndex index)
    -> futures::Future<std::unique_ptr<Iterator>> {
  // TODO As far as I can tell right now, we can get rid of this, but for the
  //      PrototypeState (currently). So:
  //      Delete this, also in streams::Stream.
  return _logMethods->waitForIterator(index).thenValue([](auto&& logIter) {
    std::unique_ptr<Iterator> deserializedIter =
        std::make_unique<LazyDeserializingIterator<EntryType, Deserializer>>(
            std::move(logIter));
    return deserializedIter;
  });
}

template<typename S>
void LeaderStateManager<S>::recoverEntries() {
  auto future = _guardedData.getLockedGuard()->recoverEntries();
  std::move(future).thenFinal(
      [weak = this->weak_from_this()](futures::Try<Result>&& tryResult) {
        // TODO error handling
        ADB_PROD_ASSERT(tryResult.hasValue());
        ADB_PROD_ASSERT(tryResult.get().ok());
        if (auto self = weak.lock(); self != nullptr) {
          auto guard = self->_guardedData.getLockedGuard();
          guard->_leaderState->onRecoveryCompleted();
        }
      });
}

template<typename S>
auto LeaderStateManager<S>::GuardedData::recoverEntries() {
  auto logSnapshot = _stream->methods().getLogSnapshot();
  auto logIter = logSnapshot.getRangeIteratorFrom(LogIndex{0});
  auto deserializedIter =
      std::make_unique<LazyDeserializingIterator<EntryType, Deserializer>>(
          std::move(logIter));
  MeasureTimeGuard timeGuard(_metrics.replicatedStateRecoverEntriesRtt);
  auto fut = _leaderState->recoverEntries(std::move(deserializedIter))
                 .then([guard = std::move(timeGuard)](auto&& res) mutable {
                   guard.fire();
                   return std::move(res.get());
                 });
  return fut;
}

template<typename S>
LeaderStateManager<S>::LeaderStateManager(
    LoggerContext loggerContext,
    std::shared_ptr<ReplicatedStateMetrics> metrics,
    std::shared_ptr<IReplicatedLeaderState<S>> leaderState,
    std::shared_ptr<ProducerStreamProxy<EntryType, Deserializer, Serializer>>
        stream)
    : _loggerContext(std::move(loggerContext)),
      _metrics(std::move(metrics)),
      _guardedData{_loggerContext, *_metrics, std::move(leaderState),
                   std::move(stream)} {}

template<typename S>
auto LeaderStateManager<S>::resign() && noexcept
    -> std::pair<std::unique_ptr<CoreType>,
                 std::unique_ptr<replicated_log::IReplicatedLogMethodsBase>> {
  return std::move(_guardedData.getLockedGuard().get()).resign();
}

template<typename S>
auto LeaderStateManager<S>::getStatus() const -> StateStatus {
  LeaderStatus status;
  // TODO remove
  return StateStatus{.variant = std::move(status)};
}

template<typename S>
auto LeaderStateManager<S>::getStateMachine() const
    -> std::shared_ptr<IReplicatedLeaderState<S>> {
  return _guardedData.getLockedGuard()->_leaderState;
}

template<typename S>
auto LeaderStateManager<S>::GuardedData::resign() && noexcept
    -> std::pair<std::unique_ptr<CoreType>,
                 std::unique_ptr<replicated_log::IReplicatedLogMethodsBase>> {
  auto core = std::move(*_leaderState).resign();
  // resign the stream after the state, so the state won't try to use the
  // resigned stream.
  auto methods = std::move(*_stream).resign();
  return {std::move(core), std::move(methods)};
}

template<typename S>
void FollowerStateManager<S>::updateCommitIndex(LogIndex commitIndex) {
  auto maybeFuture =
      _guardedData.getLockedGuard()->updateCommitIndex(commitIndex);
  // note that we release the lock before calling "then"

  // we get a future iff applyEntries was called
  if (maybeFuture.has_value()) {
    auto& future = *maybeFuture;
    std::move(future).thenFinal(
        [weak = this->weak_from_this()](auto&& tryResult) {
          if (auto self = weak.lock(); self != nullptr) {
            auto res = basics::catchToResult([&] {
              return std::forward<decltype(tryResult)>(tryResult).get();
            });
            self->handleApplyEntriesResult(res);
          }
        });
  }
}

template<typename S>
void FollowerStateManager<S>::handleApplyEntriesResult(arangodb::Result res) {
  auto maybeFuture = [&]() -> std::optional<futures::Future<Result>> {
    auto guard = _guardedData.getLockedGuard();
    if (res.ok()) {
      ADB_PROD_ASSERT(guard->_applyEntriesIndexInFlight.has_value());
      auto const index = guard->_lastAppliedIndex =
          *guard->_applyEntriesIndexInFlight;

      auto queue = guard->getResolvablePromises(index);
      auto& scheduler = *SchedulerFeature::SCHEDULER;
      queue.resolveAllWith(futures::Try(index), [&scheduler]<typename F>(
                                                    F&& f) noexcept {
        static_assert(noexcept(std::decay_t<decltype(f)>(std::forward<F>(f))));
        scheduler.queue(RequestLane::CLUSTER_INTERNAL,
                        [f = std::forward<F>(f)]() mutable noexcept {
                          static_assert(noexcept(std::forward<F>(f)()));
                          std::forward<F>(f)();
                        });
      });
    }
    guard->_applyEntriesIndexInFlight = std::nullopt;

    if (res.fail()) {
      switch (static_cast<int>(res.errorNumber())) {
        case static_cast<int>(
            TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED): {
          // Log follower has resigned, we'll be resigned as well. We just stop
          // working.
          return std::nullopt;
        }
        case static_cast<int>(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND): {
          // TODO this is a temporary fix, see CINFRA-588
          return std::nullopt;
        }
      }
    }

    ADB_PROD_ASSERT(!res.fail())
        << _loggerContext
        << " Unexpected error returned by apply entries: " << res;

    if (res.fail() || guard->_commitIndex > guard->_lastAppliedIndex) {
      return guard->maybeScheduleApplyEntries();
    }
    return std::nullopt;
  }();
  if (maybeFuture) {
    std::move(*maybeFuture)
        .thenFinal([weak = this->weak_from_this()](auto&& tryResult) {
          if (auto self = weak.lock(); self != nullptr) {
            auto res = basics::catchToResult([&] {
              return std::forward<decltype(tryResult)>(tryResult).get();
            });
            self->handleApplyEntriesResult(res);
          }
        });
  }
}

template<typename S>
auto FollowerStateManager<S>::GuardedData::updateCommitIndex(
    LogIndex commitIndex) -> std::optional<futures::Future<Result>> {
  LOG_DEVEL_IF(false) << "updating commit index from " << _commitIndex << " to "
                      << commitIndex;
  if (_stream == nullptr) {
    return std::nullopt;
  }
  _commitIndex = std::max(_commitIndex, commitIndex);
  return maybeScheduleApplyEntries();
}

template<typename S>
auto FollowerStateManager<S>::GuardedData::maybeScheduleApplyEntries()
    -> std::optional<futures::Future<Result>> {
  if (_stream == nullptr) {
    return std::nullopt;
  }
  if (_commitIndex > _lastAppliedIndex and
      not _applyEntriesIndexInFlight.has_value()) {
    auto log = _stream->methods().getLogSnapshot();
    _applyEntriesIndexInFlight = _commitIndex;
    // get an iterator for the range [last_applied + 1, commitIndex + 1)
    auto logIter = log.getIteratorRange(_lastAppliedIndex + 1,
                                        *_applyEntriesIndexInFlight + 1);
    auto deserializedIter = std::make_unique<
        LazyDeserializingIterator<EntryType const&, Deserializer>>(
        std::move(logIter));
    return _followerState->applyEntries(std::move(deserializedIter));
  } else {
    return std::nullopt;
  }
}

template<typename S>
auto FollowerStateManager<S>::GuardedData::getResolvablePromises(
    LogIndex index) noexcept -> WaitForQueue {
  return _waitQueue.splitLowerThan(index + 1);
}

template<typename S>
auto FollowerStateManager<S>::GuardedData::waitForApplied(LogIndex index)
    -> WaitForQueue::WaitForFuture {
  if (index <= _lastAppliedIndex) {
    // Resolve the promise immediately before returning the future
    auto promise = WaitForQueue::WaitForPromise();
    promise.setTry(futures::Try(_lastAppliedIndex));
    return promise.getFuture();
  }
  return _waitQueue.waitFor(index);
}

template<typename S>
FollowerStateManager<S>::FollowerStateManager(
    LoggerContext loggerContext,
    std::shared_ptr<ReplicatedStateMetrics> metrics,
    std::shared_ptr<IReplicatedFollowerState<S>> followerState,
    std::shared_ptr<StreamProxy<EntryType, Deserializer>> stream)
    : _loggerContext(std::move(loggerContext)),
      _metrics(std::move(metrics)),
      _guardedData{std::move(followerState), std::move(stream)} {}

template<typename S>
void FollowerStateManager<S>::acquireSnapshot(ServerID leader, LogIndex index) {
  LOG_CTX("c4d6b", DEBUG, _loggerContext) << "calling acquiring snapshot";
  MeasureTimeGuard rttGuard(_metrics->replicatedStateAcquireSnapshotRtt);
  GaugeScopedCounter snapshotCounter(
      _metrics->replicatedStateNumberWaitingForSnapshot);
  auto fut = _guardedData.doUnderLock([&](auto& self) {
    return self._followerState->acquireSnapshot(leader, index);
  });
  // note that we release the lock before calling "then" to avoid deadlocks

  // temporary hack: post on the scheduler to avoid deadlocks with the log
  auto& scheduler = *SchedulerFeature::SCHEDULER;
  scheduler.queue(RequestLane::CLUSTER_INTERNAL, [weak = this->weak_from_this(),
                                                  fut = std::move(fut),
                                                  rttGuard =
                                                      std::move(rttGuard),
                                                  snapshotCounter = std::move(
                                                      snapshotCounter),
                                                  leader, index]() mutable {
    std::move(fut).thenFinal(
        [weak = std::move(weak), rttGuard = std::move(rttGuard),
         snapshotCounter = std::move(snapshotCounter), leader,
         index](futures::Try<Result>&& tryResult) mutable noexcept {
          rttGuard.fire();
          snapshotCounter.fire();
          if (auto self = weak.lock(); self != nullptr) {
            LOG_CTX("13f07", DEBUG, self->_loggerContext)
                << "acquireSnapshot returned";
            auto result =
                basics::catchToResult([&] { return tryResult.get(); });
            if (result.ok()) {
              LOG_CTX("44d58", DEBUG, self->_loggerContext)
                  << "snapshot transfer successfully completed, informing "
                     "replicated log";
              auto guard = self->_guardedData.getLockedGuard();
              auto res =
                  static_cast<replicated_log::IReplicatedLogFollowerMethods&>(
                      guard->_stream->methods())
                      .snapshotCompleted();
              // TODO (How) can we handle this more gracefully?
              ADB_PROD_ASSERT(res.ok());
            } else {
              LOG_CTX("9a68a", ERR, self->_loggerContext)
                  << "failed to transfer snapshot: " << result.errorMessage()
                  << " - retry scheduled";
              // TODO implement a more graceful retry loop with a back-off
              self->acquireSnapshot(leader, index);
            }
          }
        });
  });
}

template<typename S>
auto FollowerStateManager<S>::resign() && noexcept
    -> std::pair<std::unique_ptr<CoreType>,
                 std::unique_ptr<replicated_log::IReplicatedLogMethodsBase>> {
  auto guard = _guardedData.getLockedGuard();
  auto core = std::move(*guard->_followerState).resign();
  auto methods = std::move(*guard->_stream).resign();
  guard->_stream.reset();
  auto tryResult = futures::Try<LogIndex>(
      std::make_exception_ptr(replicated_log::ParticipantResignedException(
          TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED, ADB_HERE)));
  auto& scheduler = *SchedulerFeature::SCHEDULER;
  guard->_waitQueue.resolveAllWith(
      std::move(tryResult), [&scheduler]<typename F>(F&& f) {
        static_assert(noexcept(std::decay_t<decltype(f)>(std::forward<F>(f))));
        scheduler.queue(RequestLane::CLUSTER_INTERNAL,
                        [f = std::forward<F>(f)]() mutable noexcept {
                          static_assert(noexcept(std::forward<F>(f)()));
                          std::forward<F>(f)();
                        });
      });

  return {std::move(core), std::move(methods)};
}

template<typename S>
auto FollowerStateManager<S>::getStatus() const -> StateStatus {
  auto followerStatus = FollowerStatus();
  // TODO remove
  return StateStatus{.variant = std::move(followerStatus)};
}

template<typename S>
auto FollowerStateManager<S>::getStateMachine() const
    -> std::shared_ptr<IReplicatedFollowerState<S>> {
  return _guardedData.getLockedGuard()->_followerState;
}

template<typename S>
auto FollowerStateManager<S>::waitForApplied(LogIndex index)
    -> WaitForQueue::WaitForFuture {
  return _guardedData.getLockedGuard()->waitForApplied(index);
}

template<typename S>
UnconfiguredStateManager<S>::UnconfiguredStateManager(
    LoggerContext loggerContext, std::unique_ptr<CoreType> core) noexcept
    : _loggerContext(std::move(loggerContext)),
      _guardedData{GuardedData{._core = std::move(core)}} {}

template<typename S>
auto UnconfiguredStateManager<S>::resign() && noexcept
    -> std::pair<std::unique_ptr<CoreType>,
                 std::unique_ptr<replicated_log::IReplicatedLogMethodsBase>> {
  auto guard = _guardedData.getLockedGuard();
  return {std::move(guard.get()).resign(), nullptr};
}

template<typename S>
auto UnconfiguredStateManager<S>::getStatus() const -> StateStatus {
  auto unconfiguredStatus = UnconfiguredStatus();
  // TODO remove
  return StateStatus{.variant = std::move(unconfiguredStatus)};
}

template<typename S>
auto UnconfiguredStateManager<S>::GuardedData::resign() && noexcept
    -> std::unique_ptr<CoreType> {
  return std::move(_core);
}

template<typename S>
auto IReplicatedLeaderState<S>::getStream() const noexcept
    -> std::shared_ptr<Stream> const& {
  ADB_PROD_ASSERT(_stream != nullptr)
      << "Replicated leader state: stream accessed before service was "
         "started.";

  return _stream;
}

template<typename S>
auto IReplicatedFollowerState<S>::getStream() const noexcept
    -> std::shared_ptr<Stream> const& {
  ADB_PROD_ASSERT(_stream != nullptr) << "Replicated follower state: stream "
                                         "accessed before service was started.";

  return _stream;
}

template<typename S>
void IReplicatedFollowerState<S>::setStateManager(
    std::shared_ptr<FollowerStateManager<S>> manager) noexcept {
  _manager = manager;
}

template<typename S>
auto IReplicatedFollowerState<S>::waitForApplied(LogIndex index)
    -> futures::Future<futures::Unit> {
  if (auto manager = _manager.lock(); manager != nullptr) {
    return manager->waitForApplied(index).thenValue(
        [](LogIndex) { return futures::Unit(); });
  } else {
    WaitForAppliedFuture future(
        std::make_exception_ptr(replicated_log::ParticipantResignedException(
            TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED, ADB_HERE)));
    return future;
  }
}

template<typename S>
ReplicatedState<S>::ReplicatedState(
    GlobalLogIdentifier gid, std::shared_ptr<replicated_log::ReplicatedLog> log,
    std::shared_ptr<Factory> factory, LoggerContext loggerContext,
    std::shared_ptr<ReplicatedStateMetrics> metrics)
    : factory(std::move(factory)),
      gid(std::move(gid)),
      log(std::move(log)),
      guardedData(*this),
      loggerContext(std::move(loggerContext)),
      metrics(std::move(metrics)) {
  TRI_ASSERT(this->metrics != nullptr);
  this->metrics->replicatedStateNumber->fetch_add(1);
}

template<typename S>
auto ReplicatedState<S>::getFollower() const -> std::shared_ptr<FollowerType> {
  auto followerState = log->getFollowerState();
  return std::dynamic_pointer_cast<FollowerType>(followerState);
}

template<typename S>
auto ReplicatedState<S>::getLeader() const -> std::shared_ptr<LeaderType> {
  auto leaderState = log->getLeaderState();
  return std::dynamic_pointer_cast<LeaderType>(leaderState);
}

template<typename S>
auto ReplicatedState<S>::getStatus() -> std::optional<StateStatus> {
  return log->getStateStatus();
}

template<typename S>
ReplicatedState<S>::~ReplicatedState() {
  metrics->replicatedStateNumber->fetch_sub(1);
}

template<typename S>
auto ReplicatedState<S>::buildCore(
    std::optional<velocypack::SharedSlice> const& coreParameter) {
  if constexpr (std::is_void_v<typename S::CoreParameterType>) {
    return factory->constructCore(gid);
  } else {
    if (!coreParameter.has_value()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_BAD_PARAMETER,
          fmt::format("Cannot find core parameter for replicated state with "
                      "ID {}, created in database {}, for {} state",
                      gid.id, gid.database, S::NAME));
    }
    auto params = velocypack::deserialize<typename S::CoreParameterType>(
        coreParameter->slice());
    PersistedStateInfo info;
    info.stateId = log->getId();
    info.specification.type = S::NAME;
    info.specification.parameters = *coreParameter;
    return factory->constructCore(gid, std::move(params));
  }
}

template<typename S>
void ReplicatedState<S>::drop(
    std::shared_ptr<replicated_log::IReplicatedStateHandle> stateHandle) && {
  ADB_PROD_ASSERT(stateHandle != nullptr);
  // opportunistic check that we're the only user.
  // note that there might be false positives (due to memory_order_relaxed),
  // and races where a weak_ptr is promoted to a shared_ptr again.
  // so: TODO remove this later!
  TRI_ASSERT(stateHandle.use_count() == 1);

  auto deferred = guardedData.doUnderLock([&](GuardedData& data) {
    std::unique_ptr<CoreType> core;
    DeferredAction action;

    // TODO remove data.oldCore and data.currentManager
    if (data.currentManager == nullptr) {
      core = std::move(data.oldCore);
    } else {
      std::tie(core, std::ignore, action) =
          std::move(*data.currentManager).resign();
    }
    ADB_PROD_ASSERT(core == nullptr);
    auto stateManager =
        std::dynamic_pointer_cast<ReplicatedStateManager<S>>(stateHandle);
    ADB_PROD_ASSERT(stateManager != nullptr);
    core = std::move(*stateManager).resign();
    ADB_PROD_ASSERT(core != nullptr);

    using CleanupHandler =
        typename ReplicatedStateTraits<S>::CleanupHandlerType;
    if constexpr (not std::is_void_v<CleanupHandler>) {
      static_assert(
          std::is_invocable_r_v<std::shared_ptr<CleanupHandler>,
                                decltype(&Factory::constructCleanupHandler),
                                Factory>);
      std::shared_ptr<CleanupHandler> cleanupHandler =
          factory->constructCleanupHandler();
      cleanupHandler->drop(std::move(core));
    }
    return action;
  });
  deferred.fire();
}

template<typename S>
auto ReplicatedState<S>::createStateHandle(
    std::optional<velocypack::SharedSlice> const& coreParameter)
    -> std::unique_ptr<replicated_log::IReplicatedStateHandle> {
  // TODO Should we make sure not to build the core twice?
  auto core = buildCore(coreParameter);
  auto handle = std::make_unique<ReplicatedStateManager<S>>(
      loggerContext, metrics, std::move(core), factory);

  return handle;
}

}  // namespace arangodb::replication2::replicated_state
