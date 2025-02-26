#include "CreateWorkersState.h"

#include "Pregel/Conductor/ExecutionStates/LoadingState.h"
#include "Pregel/Conductor/ExecutionStates/FatalErrorState.h"
#include "Pregel/Conductor/State.h"
#include "CanceledState.h"

using namespace arangodb;
using namespace arangodb::pregel;
using namespace arangodb::pregel::conductor;

CreateWorkers::CreateWorkers(ConductorState& conductor)
    : conductor{conductor} {}

auto workerSpecification(ExecutionSpecifications const& specifications)
    -> std::unordered_map<ServerID, worker::message::CreateWorker> {
  auto createWorkers =
      std::unordered_map<ServerID, worker::message::CreateWorker>{};
  for (auto server : specifications.graphSerdeConfig.responsibleServerSet()) {
    createWorkers.emplace(
        server, worker::message::CreateWorker{
                    .executionNumber = specifications.executionNumber,
                    .algorithm = std::string{specifications.algorithm},
                    .userParameters = specifications.userParameters,
                    .coordinatorId = "",
                    .parallelism = specifications.parallelism,
                    .graphSerdeConfig = specifications.graphSerdeConfig});
  }
  return createWorkers;
}

auto CreateWorkers::messagesToServers()
    -> std::unordered_map<ServerID, worker::message::CreateWorker> {
  auto workerSpecifications = workerSpecification(conductor.specifications);

  auto servers = std::vector<ServerID>{};
  for (auto const& [server, _] : workerSpecifications) {
    servers.emplace_back(server);
    sentServers.emplace(server);
  }

  return workerSpecifications;
}

auto CreateWorkers::cancel(actor::DistributedActorPID sender,
                           message::ConductorMessages message)
    -> std::optional<StateChange> {
  auto newState = std::make_unique<Canceled>(conductor);
  auto stateName = newState->name();

  return StateChange{
      .statusMessage = pregel::message::Canceled{.state = stateName},
      .metricsMessage = pregel::metrics::message::ConductorFinished{},
      .newState = std::move(newState)};
}

auto CreateWorkers::receive(actor::DistributedActorPID sender,
                            message::ConductorMessages message)
    -> std::optional<StateChange> {
  if (not sentServers.contains(sender.server) or
      not std::holds_alternative<ResultT<message::WorkerCreated>>(message)) {
    auto newState = std::make_unique<FatalError>(conductor);
    auto stateName = newState->name();
    return StateChange{
        .statusMessage =
            pregel::message::InFatalError{
                .state = stateName,
                .errorMessage = fmt::format(
                    "In {}: Received unexpected message {} from {}", name(),
                    inspection::json(message), inspection::json(sender))},
        .metricsMessage = pregel::metrics::message::ConductorFinished{},
        .newState = std::move(newState)};
  }
  auto workerCreated = std::get<ResultT<message::WorkerCreated>>(message);
  if (not workerCreated.ok()) {
    auto newState = std::make_unique<FatalError>(conductor);
    auto stateName = newState->name();
    return StateChange{
        .statusMessage =
            pregel::message::InFatalError{
                .state = stateName,
                .errorMessage =
                    fmt::format("In {}: Received error {} from {}", name(),
                                inspection::json(workerCreated.errorMessage()),
                                inspection::json(sender))},
        .metricsMessage = pregel::metrics::message::ConductorFinished{},
        .newState = std::move(newState)};
  }
  conductor.workers.emplace(sender);

  _updateResponsibleActorPerShard(sender);

  respondedServers.emplace(sender.server);
  responseCount++;

  if (responseCount == sentServers.size() and respondedServers == sentServers) {
    auto newState =
        std::make_unique<Loading>(conductor, std::move(actorForShard));
    auto stateName = newState->name();
    return StateChange{
        .statusMessage = pregel::message::LoadingStarted{.state = stateName},
        .metricsMessage = pregel::metrics::message::ConductorLoadingStarted{},
        .newState = std::move(newState)};
  }
  return std::nullopt;
};

auto CreateWorkers::_updateResponsibleActorPerShard(
    actor::DistributedActorPID actor) -> void {
  auto vertexShards =
      conductor.specifications.graphSerdeConfig.localShardIDs(actor.server);

  for (auto shard : vertexShards) {
    actorForShard.emplace(shard, actor);
  }
}
