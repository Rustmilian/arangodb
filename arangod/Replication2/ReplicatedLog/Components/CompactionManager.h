////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/LogStatus.h"
#include "Basics/Guarded.h"
#include "ExclusiveBool.h"
#include "Replication2/DeferredExecution.h"

namespace arangodb {
template<typename T>
class ResultT;
namespace futures {
template<typename T>
class Future;
template<typename T>
class Promise;
struct Unit;
}  // namespace futures

namespace replication2::replicated_log {
inline namespace comp {

struct IStorageManager;

struct ISchedulerInterface {
  using clock = std::chrono::steady_clock;

  virtual ~ISchedulerInterface() = default;
  // virtual auto delay(clock::duration) -> futures::Future<futures::Unit> = 0;
  //  virtual auto post()
};

struct ICompactionManager {
  struct CompactResult {
    std::optional<result::Error> error;
    CompactionStopReason stopReason;
    LogRange compactedRange;
  };

  virtual ~ICompactionManager() = default;
  virtual void updateReleaseIndex(LogIndex) noexcept = 0;
  virtual void updateLargestIndexToKeep(LogIndex) noexcept = 0;
  virtual auto compact() noexcept -> futures::Future<CompactResult> = 0;
  [[nodiscard]] virtual auto getCompactionStatus() const noexcept
      -> CompactionStatus = 0;
};

template<typename T>
struct ResolveAggregator {
  auto waitFor() -> futures::Future<T> {
    return promises.emplace_back().getFuture();
  }

  auto resolveAll(futures::Try<T> result) -> DeferredAction {
    struct ResolveData {
      ContainerType promises;
      futures::Try<T> result;
      ResolveData(ContainerType promises, futures::Try<T> result)
          : promises(std::move(promises)), result(std::move(result)) {}
    };
    return DeferredAction{
        [data = std::make_unique<ResolveData>(
             std::move(promises), std::move(result))]() mutable noexcept {
          for (auto& p : data->promises) {
            auto copy = data->result;
            p.setTry(std::move(copy));
          }
        }};
  }

  auto empty() const noexcept { return promises.empty(); }
  auto size() const noexcept { return promises.size(); }

  ResolveAggregator() noexcept = default;
  ResolveAggregator(ResolveAggregator const&) = delete;
  ResolveAggregator(ResolveAggregator&&) noexcept = default;
  auto operator=(ResolveAggregator const&) -> ResolveAggregator& = delete;
  auto operator=(ResolveAggregator&&) noexcept -> ResolveAggregator& = default;

 private:
  using ContainerType = std::vector<futures::Promise<T>>;
  ContainerType promises;
};

struct CompactionManager : ICompactionManager,
                           std::enable_shared_from_this<CompactionManager> {
  explicit CompactionManager(
      IStorageManager& storage, ISchedulerInterface& scheduler,
      std::shared_ptr<ReplicatedLogGlobalSettings const> options);

  CompactionManager(CompactionManager const&) = delete;
  CompactionManager(CompactionManager&&) noexcept = delete;
  auto operator=(CompactionManager const&) -> CompactionManager& = delete;
  auto operator=(CompactionManager&&) noexcept -> CompactionManager& = delete;

  void updateReleaseIndex(LogIndex index) noexcept override;
  void updateLargestIndexToKeep(LogIndex index) noexcept override;
  auto compact() noexcept -> futures::Future<CompactResult> override;

  auto getCompactionStatus() const noexcept -> CompactionStatus override;

 private:
  struct GuardedData {
    explicit GuardedData(IStorageManager& storage,
                         ISchedulerInterface& scheduler);

    [[nodiscard]] auto isCompactionInProgress() const noexcept -> bool;

    ResolveAggregator<CompactResult> compactAggregator;

    bool _compactionInProgress{false};
    bool _fullCompactionNextRound{false};
    LogIndex releaseIndex;
    LogIndex largestIndexToKeep;
    CompactionStatus status;
    IStorageManager& storage;
    ISchedulerInterface& scheduler;
  };
  Guarded<GuardedData> guarded;

  std::shared_ptr<ReplicatedLogGlobalSettings const> const options;

  void triggerAsyncCompaction(Guarded<GuardedData>::mutex_guard_type guard,
                              bool ignoreThreshold);
  void checkCompaction(Guarded<GuardedData>::mutex_guard_type);
  void scheduleCompaction(Guarded<GuardedData>::mutex_guard_type guard,
                          bool ignoreThreshold);
  [[nodiscard]] auto calculateCompactionIndex(GuardedData const& data,
                                              LogRange bounds,
                                              bool ignoreThreshold) const
      -> std::tuple<LogIndex, CompactionStopReason>;
};
}  // namespace comp
}  // namespace replication2::replicated_log
}  // namespace arangodb
