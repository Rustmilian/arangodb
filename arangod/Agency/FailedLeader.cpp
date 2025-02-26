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
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "FailedLeader.h"

#include "Agency/Agent.h"
#include "Agency/Job.h"
#include "Agency/JobContext.h"
#include "Agency/Node.h"
#include "Basics/StaticStrings.h"
#include "Basics/TimeString.h"
#include "Logger/LogMacros.h"

#include <algorithm>
#include <vector>

using namespace arangodb::consensus;
using namespace arangodb::velocypack;

FailedLeader::FailedLeader(Node const& snapshot, AgentInterface* agent,
                           std::string const& jobId, std::string const& creator,
                           std::string const& database,
                           std::string const& collection,
                           std::string const& shard, std::string const& from,
                           bool addsFollower)
    : Job(NOTFOUND, snapshot, agent, jobId, creator),
      _database(database),
      _collection(collection),
      _shard(shard),
      _from(from),
      _addsFollower(addsFollower) {}

FailedLeader::FailedLeader(Node const& snapshot, AgentInterface* agent,
                           JOB_STATUS status, std::string const& jobId)
    : Job(status, snapshot, agent, jobId) {
  // Get job details from agency:
  std::string path = pos[status] + _jobId + "/";
  auto tmp_database = _snapshot.hasAsString(path + "database");
  auto tmp_collection = _snapshot.hasAsString(path + "collection");
  auto tmp_from = _snapshot.hasAsString(path + "fromServer");
  auto tmp_addsFollower = _snapshot.hasAsBool(path + "addsFollower");

  // set only if already started (test to prevent warning)
  if (_snapshot.has(path + "toServer")) {
    auto tmp_to = _snapshot.hasAsString(path + "toServer");
    _to = tmp_to.value();
  }
  if (tmp_addsFollower) {
    _addsFollower = tmp_addsFollower.value();
  }

  auto tmp_shard = _snapshot.hasAsString(path + "shard");
  auto tmp_creator = _snapshot.hasAsString(path + "creator");
  auto tmp_created = _snapshot.hasAsString(path + "timeCreated");

  if (tmp_database && tmp_collection && tmp_from && tmp_shard && tmp_creator &&
      tmp_created) {
    _database = tmp_database.value();
    _collection = tmp_collection.value();
    _from = tmp_from.value();
    // _to conditionally set above
    _shard = tmp_shard.value();
    _creator = tmp_creator.value();
    _created = stringToTimepoint(tmp_created.value());
  } else {
    std::stringstream err;
    err << "Failed to find job " << _jobId << " in agency";
    LOG_TOPIC("cae01", ERR, Logger::SUPERVISION) << err.str();
    finish("", _shard, false, err.str());
    _status = FAILED;
  }
}

FailedLeader::~FailedLeader() = default;

void FailedLeader::run(bool& aborts) { runHelper("", _shard, aborts); }

void FailedLeader::rollback() {
  // Create new plan servers (exchange _to and _from)
  std::string planPath =
      planColPrefix + _database + "/" + _collection + "/shards/" + _shard;
  auto plannedBuilder = _snapshot.hasAsBuilder(planPath).value();
  auto planned = plannedBuilder.slice();
  std::shared_ptr<Builder> payload = nullptr;

  if (_status == PENDING) {  // Only payload if pending. Otherwise just fail.
    VPackBuilder rb;
    if (!_to.empty()) {
      {
        VPackArrayBuilder r(&rb);
        for (auto const i : VPackArrayIterator(planned)) {
          TRI_ASSERT(i.isString());
          auto istr = i.stringView();
          if (istr == _from) {
            rb.add(VPackValue(_to));
          } else if (istr == _to) {
            rb.add(VPackValue(_from));
          } else {
            rb.add(i);
          }
        }
      }
    } else {
      rb.add(planned);
    }
    auto cs = clones(_snapshot, _database, _collection, _shard);

    // Transactions
    payload = std::make_shared<Builder>();
    {
      VPackArrayBuilder a(payload.get());
      {  // opers
        VPackObjectBuilder b(payload.get());
        for (auto const& c : cs) {
          payload->add(planColPrefix + _database + "/" + c.collection +
                           "/shards/" + c.shard,
                       rb.slice());
        }
      }
      {
        VPackObjectBuilder p(payload.get());
        addPreconditionCollectionStillThere(*payload.get(), _database,
                                            _collection);
      }
    }
  }

  finish("", _shard, false, "Timed out.", payload);
}

bool FailedLeader::create(std::shared_ptr<VPackBuilder> b) {
  using namespace std::chrono;
  LOG_TOPIC("46046", INFO, Logger::SUPERVISION)
      << "Create failedLeader for " << _shard << " from " << _from;

  if (b == nullptr) {
    _jb = std::make_shared<Builder>();
    _jb->openArray();
    _jb->openObject();
  } else {
    _jb = b;
  }

  _jb->add(VPackValue(toDoPrefix + _jobId));
  {
    VPackObjectBuilder todo(_jb.get());
    _jb->add("creator", VPackValue(_creator));
    _jb->add("type", VPackValue("failedLeader"));
    _jb->add("database", VPackValue(_database));
    _jb->add("collection", VPackValue(_collection));
    _jb->add("shard", VPackValue(_shard));
    _jb->add("fromServer", VPackValue(_from));
    _jb->add("jobId", VPackValue(_jobId));
    _jb->add("timeCreated", VPackValue(timepointToString(system_clock::now())));
    _jb->add("addsFollower", VPackValue(_addsFollower));
  }

  if (b == nullptr) {
    _jb->close();  // object
    _jb->close();  // array
    write_ret_t res = singleWriteTransaction(_agent, *_jb, false);
    return (res.accepted && res.indices.size() == 1 && res.indices[0]);
  }

  return true;
}

bool FailedLeader::start(bool& aborts) {
  std::vector<std::string> existing =
      _snapshot.exists(planColPrefix + _database + "/" + _collection + "/" +
                       "distributeShardsLike");

  // Fail if got distributeShardsLike
  if (existing.size() == 5) {
    finish("", _shard, false, "Collection has distributeShardsLike");
    return false;
  }
  // Fail if collection gone
  else if (existing.size() < 4) {
    finish("", _shard, true, "Collection " + _collection + " gone");
    return false;
  }

  // Get healthy in Sync follower common to all prototype + clones
  auto commonHealthyInSync = findNonblockedCommonHealthyInSyncFollower(
      _snapshot, _database, _collection, _shard, _from);
  if (commonHealthyInSync.empty()) {
    return false;
  } else {
    _to = commonHealthyInSync;
  }

  LOG_TOPIC("0ced0", INFO, Logger::SUPERVISION)
      << "Start failedLeader for " << _shard << " from " << _from << " to "
      << _to;

  using namespace std::chrono;

  // Current servers vector
  std::string curPath =
      curColPrefix + _database + "/" + _collection + "/" + _shard;
  auto currentBuilder = _snapshot.get(curPath + "/servers")->toBuilder();
  auto current = currentBuilder.slice();

  // Planned servers vector
  std::string planPath =
      planColPrefix + _database + "/" + _collection + "/shards/" + _shard;
  auto plannedBuilder = _snapshot.get(planPath)->toBuilder();
  auto planned = plannedBuilder.slice();

  // Get todo entry
  Builder todo;
  {
    VPackArrayBuilder t(&todo);
    if (_jb == nullptr) {
      auto const& jobIdNode = _snapshot.get(toDoPrefix + _jobId);
      if (jobIdNode) {
        jobIdNode->toBuilder(todo);
      } else {
        LOG_TOPIC("96395", INFO, Logger::SUPERVISION)
            << "Failed to get key " << toDoPrefix << _jobId
            << " from agency snapshot";
        return false;
      }
    } else {
      todo.add(_jb->slice()[0].get(toDoPrefix + _jobId));
    }
  }

  // New plan vector excluding _to and _from
  std::vector<std::string> planv;
  for (auto const& i : VPackArrayIterator(planned)) {
    auto s = i.copyString();
    // _from and _to are added as first and last entries
    // we can keep all others
    // for security we will not use empty strings (empty servers should never
    // happen) also we will not use any resigend servers in the list (this
    // should never happen as well, but if it happens, this situation will
    // repair itself by diverging replicationFactor.
    if (s != _from && s != _to && !s.empty() && s[0] != '_') {
      planv.push_back(std::move(s));
    }
  }

  // Exclude servers in failoverCandidates for some clone and those in Plan:
  auto shardsLikeMe = clones(_snapshot, _database, _collection, _shard);
  auto failoverCands =
      Job::findAllFailoverCandidates(_snapshot, _database, shardsLikeMe);
  std::vector<std::string> excludes;
  for (const auto& s : VPackArrayIterator(planned)) {
    if (s.isString()) {
      std::string id = s.copyString();
      if (failoverCands.find(id) == failoverCands.end()) {
        excludes.push_back(std::move(id));
      }
    }
  }
  for (auto const& id : failoverCands) {
    excludes.push_back(id);
  }

  // Additional follower, if applicable
  if (_addsFollower) {
    auto additionalFollower = randomIdleAvailableServer(_snapshot, excludes);
    if (!additionalFollower.empty()) {
      planv.push_back(additionalFollower);
    }
  }

  // Transactions
  Builder pending;

  {
    VPackArrayBuilder transactions(&pending);
    {
      VPackArrayBuilder transaction(&pending);

      // Operations ----------------------------------------------------------
      {
        VPackObjectBuilder operations(&pending);
        // Add pending entry
        pending.add(VPackValue(pendingPrefix + _jobId));
        {
          VPackObjectBuilder ts(&pending);
          pending.add("timeStarted",  // start
                      VPackValue(timepointToString(system_clock::now())));
          pending.add("toServer", VPackValue(_to));  // toServer
          for (auto const& obj : VPackObjectIterator(todo.slice()[0])) {
            pending.add(obj.key.stringView(), obj.value);
          }
        }
        addRemoveJobFromSomewhere(pending, "ToDo", _jobId);
        // DB server vector -------
        Builder ns;
        {
          VPackArrayBuilder servers(&ns);
          ns.add(VPackValue(_to));
          // We prefer servers in sync and want to put them early in the new
          // Plan (behind the leader). This helps so that RemoveFollower prefers
          // others to remove.
          for (auto const& i : VPackArrayIterator(current)) {
            std::string s = i.copyString();
            if (s.starts_with('_')) {
              s = s.substr(1);
            }
            // We need to make sure to only pick servers from the plan as
            // followers. There is a chance, that a server is removed from the
            // plan, but it is not yet taken out from the in-sync followers in
            // current. e.g. User Reduces the ReplicationFactor => Follower F1
            // will be taken from the Plan Now F1 drops the shard. For some
            // Reason Leader cannot report out-of-sync, and dies.
            // => F1 will be readded in shard, as an early follower, and is
            // considered to be in sync, until New Leader has updated sync
            // information.
            if (s != _from && s != _to &&
                std::find(planv.begin(), planv.end(), s) != planv.end()) {
              ns.add(i);
              planv.erase(std::remove(planv.begin(), planv.end(), s),
                          planv.end());
            }
          }
          ns.add(VPackValue(_from));
          for (auto const& i : planv) {
            ns.add(VPackValue(i));
          }
        }
        for (auto const& clone : shardsLikeMe) {
          pending.add(planColPrefix + _database + "/" + clone.collection +
                          "/shards/" + clone.shard,
                      ns.slice());
        }
        addBlockShard(pending, _shard, _jobId);
        addIncreasePlanVersion(pending);
      }
      // Preconditions -------------------------------------------------------
      {
        VPackObjectBuilder preconditions(&pending);
        // Failed condition persists
        addPreconditionServerHealth(pending, _from,
                                    Supervision::HEALTH_STATUS_FAILED);
        // Destination server still in good condition
        addPreconditionServerHealth(pending, _to,
                                    Supervision::HEALTH_STATUS_GOOD);
        // Server list in plan still as before
        addPreconditionUnchanged(pending, planPath, planned);
        // All clones still exist
        addPreconditionClonesStillExist(pending, _database, shardsLikeMe);
        // Check that Current/servers and failoverCandidates are still as
        // we inspected them:
        doForAllShards(
            _snapshot, _database, shardsLikeMe,
            [this, &pending](Slice plan, Slice current, std::string& planPath,
                             std::string& curPath) {
              addPreconditionUnchanged(pending, curPath, current);
              // take off "servers" from curPath and add
              // "failoverCandidates":
              std::string foCandsPath = curPath.substr(0, curPath.size() - 7);
              foCandsPath += StaticStrings::FailoverCandidates;
              auto foCands = this->_snapshot.hasAsBuilder(foCandsPath);
              if (foCands) {
                addPreconditionUnchanged(pending, foCandsPath,
                                         foCands->slice());
              }
            });
        // Destination server should not be blocked by another job
        addPreconditionServerNotBlocked(pending, _to);
        // Shard to be handled is block by another job
        addPreconditionShardNotBlocked(pending, _shard);
      }  // Preconditions -----------------------------------------------------
    }
  }

  // Abort job blocking server if abortable
  //  (likely to not exist, avoid warning message by testing first)
  if (_snapshot.has(blockedShardsPrefix + _shard)) {
    auto jobId = _snapshot.hasAsString(blockedShardsPrefix + _shard);
    if (jobId && !abortable(_snapshot, *jobId)) {
      return false;
    } else if (jobId) {
      aborts = true;
      JobContext(PENDING, *jobId, _snapshot, _agent)
          .abort("failed leader requests abort");
      return false;
    }
  }

  LOG_TOPIC("b1a8c", DEBUG, Logger::SUPERVISION)
      << "FailedLeader transaction: " << pending.toJson();

  trans_ret_t res = generalTransaction(_agent, pending);

  if (!res.accepted) {  // lost leadership
    LOG_TOPIC("1f01f", INFO, Logger::SUPERVISION)
        << "Leadership lost! Job " << _jobId << " handed off.";
    return false;
  }

  LOG_TOPIC("c7421", DEBUG, Logger::SUPERVISION)
      << "FailedLeader result: " << res.result->toJson();

  // Something went south. Let's see
  auto result = res.result->slice()[0];

  if (result.isNumber()) {
    return true;
  }

  TRI_ASSERT(result.isObject());

  if (result.isObject()) {
    // Still failing _from?
    auto slice = result.get(std::vector<std::string>(
        {agencyPrefix, "Supervision", "Health", _from, "Status"}));
    if (slice.isString() &&
        slice.stringView() == Supervision::HEALTH_STATUS_GOOD) {
      finish("", _shard, false, "Server " + _from + " no longer failing.");
      return false;
    }

    // Still healthy _to?
    slice = result.get(std::vector<std::string>(
        {agencyPrefix, "Supervision", "Health", _to, "Status"}));
    if (slice.isString() &&
        slice.stringView() != Supervision::HEALTH_STATUS_GOOD) {
      LOG_TOPIC("7e2ef", INFO, Logger::SUPERVISION)
          << "Will not failover from " << _from << " to " << _to
          << " as target server is no longer in good condition. Will retry.";
      return false;
    }

    // Snapshot and plan still in sync with respect to server list?
    slice = result.get(
        std::vector<std::string>({agencyPrefix, "Plan", "Collections",
                                  _database, _collection, "shards", _shard}));
    if (!slice.isNone()) {
      LOG_TOPIC("eedcf", INFO, Logger::SUPERVISION)
          << "Plan no longer holds the expected server list. Will retry.";
    }

    // To server blocked by other job?
    slice = result.get(std::vector<std::string>(
        {agencyPrefix, "Supervision", "DBServers", _to}));
    if (!slice.isNone()) {
      LOG_TOPIC("aff11", INFO, Logger::SUPERVISION)
          << "Destination server " << _to << " meanwhile is blocked by job "
          << slice.stringView();
    }

    // This shard blocked by other job?
    slice = result.get(std::vector<std::string>(
        {agencyPrefix, "Supervision", "Shards", _shard}));
    if (!slice.isNone()) {
      LOG_TOPIC("71bb2", INFO, Logger::SUPERVISION)
          << "Shard  " << _shard << " meanwhile is blocked by job "
          << slice.stringView();
    }
  }

  return false;
}

JOB_STATUS FailedLeader::status() {
  if (!_snapshot.has(planColPrefix + _database + "/" + _collection)) {
    finish("", _shard, true, "Collection " + _collection + " gone");
    return FINISHED;
  }

  // Timedout after 77 minutes
  if (std::chrono::system_clock::now() - _created >
      std::chrono::seconds(4620)) {
    finish("", (_status != PENDING) ? "" : _shard, false, "Timed out.");
    return FAILED;
  }

  if (_status != PENDING) {
    return _status;
  }

  std::string toServerHealth = checkServerHealth(_snapshot, _to);
  if (toServerHealth == Supervision::HEALTH_STATUS_FAILED ||
      toServerHealth == Supervision::HEALTH_STATUS_UNCLEAR) {
    finish("", _shard, false, "_to server not health");
    return FAILED;
  }

  std::string database, shard;
  auto const& job = _snapshot.get(pendingPrefix + _jobId);
  if (job) {
    auto tmp_database = job->hasAsString("database");
    auto tmp_shard = job->hasAsString("shard");
    if (tmp_database && tmp_shard) {
      database = tmp_database.value();
      shard = tmp_shard.value();
    } else {
      return _status;
    }  // else
  } else {
    return _status;
  }  // else

  bool done = false;
  for (auto const& clone : clones(_snapshot, _database, _collection, _shard)) {
    auto sub = database + "/" + clone.collection;
    auto plan_slice =
        _snapshot.hasAsBuilder(planColPrefix + sub + "/shards/" + clone.shard);
    auto cur_slice = _snapshot.hasAsBuilder(curColPrefix + sub + "/" +
                                            clone.shard + "/servers");
    if (plan_slice && cur_slice &&
        !basics::VelocyPackHelper::equal(plan_slice->slice()[0],
                                         cur_slice->slice()[0], false)) {
      LOG_TOPIC("0d8ca", DEBUG, Logger::SUPERVISION)
          << "FailedLeader waiting for " << sub << "/" << shard;
      break;
    }
    done = true;
  }

  if (done) {
    if (finish("", shard)) {
      LOG_TOPIC("1ead6", INFO, Logger::SUPERVISION)
          << "Finished failedLeader for " << _shard << " from " << _from
          << " to " << _to;
      return FINISHED;
    }
  }

  return _status;
}

arangodb::Result FailedLeader::abort(std::string const& reason) {
  // job is only abortable when it is in ToDo
  if (_status != TODO) {
    return Result(TRI_ERROR_SUPERVISION_GENERAL_FAILURE,
                  "Failed aborting failedFollower job beyond todo stage");
  } else {
    finish("", "", false, "job aborted: " + reason);
    return Result();
  }
}
