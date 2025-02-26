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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#include "ModificationNodes.h"
#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionBlockImpl.tpp"
#include "Aql/ExecutionPlan.h"
#include "Aql/ModificationExecutor.h"
#include "Aql/ModificationExecutorHelpers.h"
#include "Aql/Query.h"
#include "Aql/SimpleModifier.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/UpsertModifier.h"
#include "Aql/VariableGenerator.h"

using namespace arangodb::aql;

namespace arangodb::aql {

ModificationNode::ModificationNode(ExecutionPlan* plan,
                                   arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      CollectionAccessingNode(plan, base),
      _options(base),
      _outVariableOld(
          Variable::varFromVPack(plan->getAst(), base, "outVariableOld", true)),
      _outVariableNew(
          Variable::varFromVPack(plan->getAst(), base, "outVariableNew", true)),
      _countStats(base.get("countStats").getBool()),
      _producesResults(base.hasKey("producesResults")
                           ? base.get("producesResults").getBool()
                           : true) {}

/// @brief toVelocyPack
void ModificationNode::doToVelocyPack(VPackBuilder& builder,
                                      unsigned flags) const {
  // add collection information
  CollectionAccessingNode::toVelocyPack(builder, flags);
  CollectionAccessingNode::toVelocyPackHelperPrimaryIndex(builder);

  // Now put info about vocbase and cid in there
  builder.add("countStats", VPackValue(_countStats));

  builder.add("producesResults", VPackValue(_producesResults));

  // add out variables
  if (_outVariableOld != nullptr) {
    builder.add(VPackValue("outVariableOld"));
    _outVariableOld->toVelocyPack(builder);
  }
  if (_outVariableNew != nullptr) {
    builder.add(VPackValue("outVariableNew"));
    _outVariableNew->toVelocyPack(builder);
  }
  builder.add(VPackValue("modificationFlags"));

  _options.toVelocyPack(builder);
}

/// @brief estimateCost
/// Note that all the modifying nodes use this estimateCost method which is
/// why we can make it final here.
CostEstimate ModificationNode::estimateCost() const {
  CostEstimate estimate = _dependencies.at(0)->getCost();
  estimate.estimatedCost += estimate.estimatedNrItems;
  if (_outVariableOld == nullptr && _outVariableNew == nullptr &&
      !_producesResults) {
    // node produces no output
    estimate.estimatedNrItems = 0;
  }
  return estimate;
}

AsyncPrefetchEligibility ModificationNode::canUseAsyncPrefetching()
    const noexcept {
  return AsyncPrefetchEligibility::kDisableGlobally;
}

void ModificationNode::cloneCommon(ModificationNode* c) const {
  if (!_countStats) {
    c->disableStatistics();
  }
  c->producesResults(_producesResults);
  CollectionAccessingNode::cloneInto(*c);
}

///////////////////////////////////////////////////////////////////////////////
/// REMOVE
///
using SingleRowRemoveExecutionBlock = ExecutionBlockImpl<ModificationExecutor<
    SingleRowFetcher<BlockPassthrough::Disable>, RemoveModifier>>;

RemoveNode::RemoveNode(ExecutionPlan* plan,
                       arangodb::velocypack::Slice const& base)
    : ModificationNode(plan, base),
      _inVariable(Variable::varFromVPack(plan->getAst(), base, "inVariable")) {}

void RemoveNode::doToVelocyPack(VPackBuilder& nodes, unsigned flags) const {
  ModificationNode::doToVelocyPack(nodes, flags);
  nodes.add(VPackValue("inVariable"));
  _inVariable->toVelocyPack(nodes);
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> RemoveNode::createBlock(
    ExecutionEngine& engine) const {
  ExecutionNode const* previousNode = getFirstDependency();

  TRI_ASSERT(previousNode != nullptr);

  RegisterId inDocRegister = variableToRegisterId(_inVariable);
  RegisterId outputNew = variableToRegisterOptionalId(_outVariableNew);
  RegisterId outputOld = variableToRegisterOptionalId(_outVariableOld);

  OperationOptions options = ModificationExecutorHelpers::convertOptions(
      _options, _outVariableNew, _outVariableOld);

  auto readableInputRegisters = RegIdSet{inDocRegister};
  auto writableOutputRegisters = RegIdSet{};
  if (outputNew.isValid()) {
    writableOutputRegisters.emplace(outputNew);
  }
  if (outputOld.isValid()) {
    writableOutputRegisters.emplace(outputOld);
  }
  auto registerInfos = createRegisterInfos(std::move(readableInputRegisters),
                                           std::move(writableOutputRegisters));

  auto executorInfos = ModificationExecutorInfos(
      &engine, inDocRegister, RegisterPlan::MaxRegisterId,
      RegisterPlan::MaxRegisterId, outputNew, outputOld,
      RegisterPlan::MaxRegisterId /*output*/, _plan->getAst()->query(),
      std::move(options), collection(), ExecutionBlock::DefaultBatchSize,
      ProducesResults(producesResults()),
      ConsultAqlWriteFilter(_options.consultAqlWriteFilter),
      IgnoreErrors(_options.ignoreErrors), DoCount(countStats()),
      IsReplace(false) /*(needed by upsert)*/,
      IgnoreDocumentNotFound(_options.ignoreDocumentNotFound));
  return std::make_unique<SingleRowRemoveExecutionBlock>(
      &engine, this, std::move(registerInfos), std::move(executorInfos));
}

/// @brief clone ExecutionNode recursively
ExecutionNode* RemoveNode::clone(ExecutionPlan* plan,
                                 bool withDependencies) const {
  auto c = std::make_unique<RemoveNode>(plan, _id, collection(), _options,
                                        _inVariable, _outVariableOld);
  ModificationNode::cloneCommon(c.get());

  return cloneHelper(std::move(c), withDependencies);
}

///////////////////////////////////////////////////////////////////////////////
/// INSERT
///
using SingleRowInsertExecutionBlock = ExecutionBlockImpl<ModificationExecutor<
    SingleRowFetcher<BlockPassthrough::Disable>, InsertModifier>>;

InsertNode::InsertNode(ExecutionPlan* plan,
                       arangodb::velocypack::Slice const& base)
    : ModificationNode(plan, base),
      _inVariable(Variable::varFromVPack(plan->getAst(), base, "inVariable")) {}

/// @brief toVelocyPack
void InsertNode::doToVelocyPack(VPackBuilder& nodes, unsigned flags) const {
  ModificationNode::doToVelocyPack(nodes, flags);  // call base class method

  // Now put info about vocbase and cid in there
  nodes.add(VPackValue("inVariable"));
  _inVariable->toVelocyPack(nodes);
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> InsertNode::createBlock(
    ExecutionEngine& engine) const {
  using namespace arangodb::aql;

  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId inputRegister = variableToRegisterId(_inVariable);

  RegisterId outputNew = variableToRegisterOptionalId(_outVariableNew);
  RegisterId outputOld = variableToRegisterOptionalId(_outVariableOld);

  OperationOptions options = ModificationExecutorHelpers::convertOptions(
      _options, _outVariableNew, _outVariableOld);

  auto readableInputRegisters = RegIdSet{inputRegister};
  auto writableOutputRegisters = RegIdSet{};
  if (outputNew.isValid()) {
    writableOutputRegisters.emplace(outputNew);
  }
  if (outputOld.isValid()) {
    writableOutputRegisters.emplace(outputOld);
  }
  auto registerInfos = createRegisterInfos(std::move(readableInputRegisters),
                                           std::move(writableOutputRegisters));

  ModificationExecutorInfos infos(
      &engine, inputRegister, RegisterPlan::MaxRegisterId,
      RegisterPlan::MaxRegisterId, outputNew, outputOld,
      RegisterPlan::MaxRegisterId /*output*/, _plan->getAst()->query(),
      std::move(options), collection(), ExecutionBlock::DefaultBatchSize,
      ProducesResults(producesResults()),
      ConsultAqlWriteFilter(_options.consultAqlWriteFilter),
      IgnoreErrors(_options.ignoreErrors), DoCount(countStats()),
      IsReplace(false) /*(needed by upsert)*/,
      IgnoreDocumentNotFound(_options.ignoreDocumentNotFound));
  return std::make_unique<SingleRowInsertExecutionBlock>(
      &engine, this, std::move(registerInfos), std::move(infos));
}

/// @brief clone ExecutionNode recursively
ExecutionNode* InsertNode::clone(ExecutionPlan* plan,
                                 bool withDependencies) const {
  auto c = std::make_unique<InsertNode>(plan, _id, collection(), _options,
                                        _inVariable, _outVariableOld,
                                        _outVariableNew);
  ModificationNode::cloneCommon(c.get());

  return cloneHelper(std::move(c), withDependencies);
}

void InsertNode::replaceVariables(
    std::unordered_map<VariableId, Variable const*> const& replacements) {
  _inVariable = Variable::replace(_inVariable, replacements);
}

void InsertNode::replaceAttributeAccess(ExecutionNode const* self,
                                        Variable const* searchVariable,
                                        std::span<std::string_view> attribute,
                                        Variable const* replaceVariable,
                                        size_t /*index*/) {
  if (_inVariable != nullptr && searchVariable == _inVariable &&
      attribute.size() == 1 && attribute[0] == StaticStrings::KeyString) {
    _inVariable = replaceVariable;
  }
}

size_t InsertNode::getMemoryUsedBytes() const { return sizeof(*this); }

///////////////////////////////////////////////////////////////////////////////
/// REMOVE
///
using SingleRowUpdateReplaceExecutionBlock =
    ExecutionBlockImpl<ModificationExecutor<
        SingleRowFetcher<BlockPassthrough::Disable>, UpdateReplaceModifier>>;

UpdateReplaceNode::UpdateReplaceNode(ExecutionPlan* plan,
                                     arangodb::velocypack::Slice const& base)
    : ModificationNode(plan, base),
      _inDocVariable(
          Variable::varFromVPack(plan->getAst(), base, "inDocVariable")),
      _inKeyVariable(Variable::varFromVPack(plan->getAst(), base,
                                            "inKeyVariable", true)) {}

void UpdateReplaceNode::doToVelocyPack(VPackBuilder& nodes,
                                       unsigned flags) const {
  ModificationNode::doToVelocyPack(nodes, flags);
  nodes.add(VPackValue("inDocVariable"));
  _inDocVariable->toVelocyPack(nodes);

  // inKeyVariable might be empty
  if (_inKeyVariable != nullptr) {
    nodes.add(VPackValue("inKeyVariable"));
    _inKeyVariable->toVelocyPack(nodes);
  }
}

UpdateNode::UpdateNode(ExecutionPlan* plan,
                       arangodb::velocypack::Slice const& base)
    : UpdateReplaceNode(plan, base) {}

void UpdateReplaceNode::replaceVariables(
    std::unordered_map<VariableId, Variable const*> const& replacements) {
  if (_inDocVariable != nullptr) {
    _inDocVariable = Variable::replace(_inDocVariable, replacements);
  }
  if (_inKeyVariable != nullptr) {
    _inKeyVariable = Variable::replace(_inKeyVariable, replacements);
  }
}

void UpdateReplaceNode::replaceAttributeAccess(
    ExecutionNode const* self, Variable const* searchVariable,
    std::span<std::string_view> attribute, Variable const* replaceVariable,
    size_t /*index*/) {
  auto replace = [&](Variable const*& variable) {
    if (variable != nullptr && searchVariable == variable &&
        attribute.size() == 1 && attribute[0] == StaticStrings::KeyString) {
      // replace the following patterns:
      // FOR doc IN collection LET #x = doc._key (projection)
      //   UPDATE|REPLACE doc._key WITH ... INTO collection
      // with
      //   UPDATE|REPLACE #x WITH ... INTO collection
      // doc._id does not need to be supported for the lookup value here,
      // as using `_id` for the lookup value is not supported.
      variable = replaceVariable;
    }
  };

  replace(_inKeyVariable);
  replace(_inDocVariable);
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> UpdateNode::createBlock(
    ExecutionEngine& engine) const {
  using namespace arangodb::aql;

  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId inDocRegister = variableToRegisterId(_inDocVariable);

  RegisterId inKeyRegister = variableToRegisterOptionalId(_inKeyVariable);
  RegisterId outputNew = variableToRegisterOptionalId(_outVariableNew);
  RegisterId outputOld = variableToRegisterOptionalId(_outVariableOld);

  auto readableInputRegisters = RegIdSet{inDocRegister};
  if (inKeyRegister.isValid()) {
    readableInputRegisters.emplace(inKeyRegister);
  }
  auto writableOutputRegisters = RegIdSet{};
  if (outputNew.isValid()) {
    writableOutputRegisters.emplace(outputNew);
  }
  if (outputOld.isValid()) {
    writableOutputRegisters.emplace(outputOld);
  }
  auto registerInfos = createRegisterInfos(std::move(readableInputRegisters),
                                           std::move(writableOutputRegisters));

  OperationOptions options = ModificationExecutorHelpers::convertOptions(
      _options, _outVariableNew, _outVariableOld);

  auto executorInfos = ModificationExecutorInfos(
      &engine, inDocRegister, inKeyRegister, RegisterPlan::MaxRegisterId,
      outputNew, outputOld, RegisterPlan::MaxRegisterId /*output*/,
      _plan->getAst()->query(), std::move(options), collection(),
      ExecutionBlock::DefaultBatchSize, ProducesResults(producesResults()),
      ConsultAqlWriteFilter(_options.consultAqlWriteFilter),
      IgnoreErrors(_options.ignoreErrors), DoCount(countStats()),
      IsReplace(false) /*(needed by upsert)*/,
      IgnoreDocumentNotFound(_options.ignoreDocumentNotFound));
  return std::make_unique<SingleRowUpdateReplaceExecutionBlock>(
      &engine, this, std::move(registerInfos), std::move(executorInfos));
}

void RemoveNode::replaceVariables(
    std::unordered_map<VariableId, Variable const*> const& replacements) {
  _inVariable = Variable::replace(_inVariable, replacements);
}

void RemoveNode::replaceAttributeAccess(ExecutionNode const* self,
                                        Variable const* searchVariable,
                                        std::span<std::string_view> attribute,
                                        Variable const* replaceVariable,
                                        size_t /*index*/) {
  if (_inVariable != nullptr && searchVariable == _inVariable &&
      attribute.size() == 1 && attribute[0] == StaticStrings::KeyString) {
    // replace the following patterns:
    // FOR doc IN collection LET #x = doc._key (projection)
    //   REMOVE doc._key WITH ... INTO collection
    // with
    //   REMOVE #x WITH ... INTO collection
    // doc._id does not need to be supported for the lookup value here,
    // as using `_id` for the lookup value is not supported.
    _inVariable = replaceVariable;
  }
}

size_t RemoveNode::getMemoryUsedBytes() const { return sizeof(*this); }

/// @brief clone ExecutionNode recursively
ExecutionNode* UpdateNode::clone(ExecutionPlan* plan,
                                 bool withDependencies) const {
  auto c = std::make_unique<UpdateNode>(plan, _id, collection(), _options,
                                        _inDocVariable, _inKeyVariable,
                                        _outVariableOld, _outVariableNew);
  ModificationNode::cloneCommon(c.get());

  return cloneHelper(std::move(c), withDependencies);
}

size_t UpdateNode::getMemoryUsedBytes() const { return sizeof(*this); }

ReplaceNode::ReplaceNode(ExecutionPlan* plan,
                         arangodb::velocypack::Slice const& base)
    : UpdateReplaceNode(plan, base) {}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> ReplaceNode::createBlock(
    ExecutionEngine& engine) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId inDocRegister = variableToRegisterId(_inDocVariable);

  RegisterId inKeyRegister = variableToRegisterOptionalId(_inKeyVariable);

  RegisterId outputNew = variableToRegisterOptionalId(_outVariableNew);

  RegisterId outputOld = variableToRegisterOptionalId(_outVariableOld);

  auto readableInputRegisters = RegIdSet{inDocRegister};
  if (inKeyRegister.isValid()) {
    readableInputRegisters.emplace(inKeyRegister);
  }
  auto writableOutputRegisters = RegIdSet{};
  if (outputNew.isValid()) {
    writableOutputRegisters.emplace(outputNew);
  }
  if (outputOld.isValid()) {
    writableOutputRegisters.emplace(outputOld);
  }
  auto registerInfos = createRegisterInfos(std::move(readableInputRegisters),
                                           std::move(writableOutputRegisters));

  OperationOptions options = ModificationExecutorHelpers::convertOptions(
      _options, _outVariableNew, _outVariableOld);

  auto executorInfos = ModificationExecutorInfos(
      &engine, inDocRegister, inKeyRegister, RegisterPlan::MaxRegisterId,
      outputNew, outputOld, RegisterPlan::MaxRegisterId /*output*/,
      _plan->getAst()->query(), std::move(options), collection(),
      ExecutionBlock::DefaultBatchSize, ProducesResults(producesResults()),
      ConsultAqlWriteFilter(_options.consultAqlWriteFilter),
      IgnoreErrors(_options.ignoreErrors), DoCount(countStats()),
      IsReplace(true), IgnoreDocumentNotFound(_options.ignoreDocumentNotFound));
  return std::make_unique<SingleRowUpdateReplaceExecutionBlock>(
      &engine, this, std::move(registerInfos), std::move(executorInfos));
}

/// @brief clone ExecutionNode recursively
ExecutionNode* ReplaceNode::clone(ExecutionPlan* plan,
                                  bool withDependencies) const {
  auto c = std::make_unique<ReplaceNode>(plan, _id, collection(), _options,
                                         _inDocVariable, _inKeyVariable,
                                         _outVariableOld, _outVariableNew);
  ModificationNode::cloneCommon(c.get());

  return cloneHelper(std::move(c), withDependencies);
}

size_t ReplaceNode::getMemoryUsedBytes() const { return sizeof(*this); }

///////////////////////////////////////////////////////////////////////////////
/// UPSERT
///
using SingleRowUpsertExecutionBlock = ExecutionBlockImpl<ModificationExecutor<
    SingleRowFetcher<BlockPassthrough::Disable>, UpsertModifier>>;

UpsertNode::UpsertNode(
    ExecutionPlan* plan, ExecutionNodeId id, Collection const* collection,
    ModificationOptions const& options, Variable const* inDocVariable,
    Variable const* insertVariable, Variable const* updateVariable,
    Variable const* outVariableNew, bool isReplace, bool canReadOwnWrites)
    : ModificationNode(plan, id, collection, options, nullptr, outVariableNew),
      _inDocVariable(inDocVariable),
      _insertVariable(insertVariable),
      _updateVariable(updateVariable),
      _isReplace(isReplace),
      _canReadOwnWrites(canReadOwnWrites) {
  TRI_ASSERT(_inDocVariable != nullptr);
  TRI_ASSERT(_insertVariable != nullptr);
  TRI_ASSERT(_updateVariable != nullptr);

  TRI_ASSERT(_outVariableOld == nullptr);
}

UpsertNode::UpsertNode(ExecutionPlan* plan,
                       arangodb::velocypack::Slice const& base)
    : ModificationNode(plan, base),
      _inDocVariable(
          Variable::varFromVPack(plan->getAst(), base, "inDocVariable")),
      _insertVariable(
          Variable::varFromVPack(plan->getAst(), base, "insertVariable")),
      _updateVariable(
          Variable::varFromVPack(plan->getAst(), base, "updateVariable")),
      _isReplace(base.get("isReplace").isTrue()),
      _canReadOwnWrites(true) {
  if (auto s = base.get(StaticStrings::ReadOwnWrites); !s.isNone()) {
    // "readOwnWrites" attribute was introduced in 3.12.
    // older coordinators will not send it. thus we make it default
    // to true.
    _canReadOwnWrites = s.isTrue();
  }
}

/// @brief toVelocyPack
void UpsertNode::doToVelocyPack(VPackBuilder& nodes, unsigned flags) const {
  ModificationNode::doToVelocyPack(nodes, flags);

  nodes.add(VPackValue("inDocVariable"));
  _inDocVariable->toVelocyPack(nodes);
  nodes.add(VPackValue("insertVariable"));
  _insertVariable->toVelocyPack(nodes);
  nodes.add(VPackValue("updateVariable"));
  _updateVariable->toVelocyPack(nodes);
  nodes.add("isReplace", VPackValue(_isReplace));
  nodes.add(StaticStrings::ReadOwnWrites, VPackValue(_canReadOwnWrites));
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> UpsertNode::createBlock(
    ExecutionEngine& engine) const {
  using namespace arangodb::aql;

  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId inDoc = variableToRegisterId(_inDocVariable);
  RegisterId insert = variableToRegisterId(_insertVariable);
  RegisterId update = variableToRegisterId(_updateVariable);

  RegisterId outputNew = variableToRegisterOptionalId(_outVariableNew);

  RegisterId outputOld = variableToRegisterOptionalId(_outVariableOld);

  auto readableInputRegisters = RegIdSet{inDoc, insert, update};
  auto writableOutputRegisters = RegIdSet{};
  if (outputNew.isValid()) {
    writableOutputRegisters.emplace(outputNew);
  }
  if (outputOld.isValid()) {
    writableOutputRegisters.emplace(outputOld);
  }
  auto registerInfos = createRegisterInfos(std::move(readableInputRegisters),
                                           std::move(writableOutputRegisters));

  OperationOptions options = ModificationExecutorHelpers::convertOptions(
      _options, _outVariableNew, _outVariableOld);
  // We must not disable indexing for UPSERTs because the subquery might rely on
  // a non-unique secondary index
  options.canDisableIndexing = false;

  // if we do not need to observe our own writes, we can turn on batching
  // for the UpsertNode. otherwise, the Upsert will execute with a batch
  // size of just 1.
  size_t batchSize = 1;
  if (!_canReadOwnWrites) {
    batchSize = ExecutionBlock::DefaultBatchSize;
  }

  auto executorInfos = ModificationExecutorInfos(
      &engine, inDoc, insert, update, outputNew, outputOld,
      RegisterPlan::MaxRegisterId /*output*/, _plan->getAst()->query(),
      std::move(options), collection(), batchSize,
      ProducesResults(producesResults()),
      ConsultAqlWriteFilter(_options.consultAqlWriteFilter),
      IgnoreErrors(_options.ignoreErrors), DoCount(countStats()),
      IsReplace(_isReplace) /*(needed by upsert)*/,
      IgnoreDocumentNotFound(_options.ignoreDocumentNotFound));
  return std::make_unique<SingleRowUpsertExecutionBlock>(
      &engine, this, std::move(registerInfos), std::move(executorInfos));
}

/// @brief clone ExecutionNode recursively
ExecutionNode* UpsertNode::clone(ExecutionPlan* plan,
                                 bool withDependencies) const {
  auto c = std::make_unique<UpsertNode>(
      plan, _id, collection(), _options, _inDocVariable, _insertVariable,
      _updateVariable, _outVariableNew, _isReplace, _canReadOwnWrites);
  ModificationNode::cloneCommon(c.get());

  return cloneHelper(std::move(c), withDependencies);
}

void UpsertNode::replaceVariables(
    std::unordered_map<VariableId, Variable const*> const& replacements) {
  if (_inDocVariable != nullptr) {
    _inDocVariable = Variable::replace(_inDocVariable, replacements);
  }
  if (_insertVariable != nullptr) {
    _insertVariable = Variable::replace(_insertVariable, replacements);
  }
  if (_updateVariable != nullptr) {
    _updateVariable = Variable::replace(_updateVariable, replacements);
  }
}

void UpsertNode::replaceAttributeAccess(ExecutionNode const* self,
                                        Variable const* searchVariable,
                                        std::span<std::string_view> attribute,
                                        Variable const* replaceVariable,
                                        size_t /*index*/) {
  auto replace = [&](Variable const*& variable) {
    if (variable != nullptr && searchVariable == variable &&
        attribute.size() == 1 && attribute[0] == StaticStrings::KeyString) {
      variable = replaceVariable;
    }
  };

  replace(_inDocVariable);
  replace(_insertVariable);
  replace(_updateVariable);
}

size_t UpsertNode::getMemoryUsedBytes() const { return sizeof(*this); }

}  // namespace arangodb::aql
