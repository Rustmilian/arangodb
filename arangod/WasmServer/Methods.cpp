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
/// @author Julia Volmer
////////////////////////////////////////////////////////////////////////////////

#include "Methods.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Result.h"
#include "Basics/ResultT.h"
#include "Futures/Future.h"
#include "VocBase/vocbase.h"
#include "WasmServer/WasmServerFeature.h"
#include "WasmServer/WasmCommon.h"

using namespace arangodb;
using namespace arangodb::wasm;

struct WasmVmMethodsSingleServer final
    : WasmVmMethods,
      std::enable_shared_from_this<WasmVmMethodsSingleServer> {
  explicit WasmVmMethodsSingleServer(TRI_vocbase_t& vocbase)
      : vocbase(vocbase){};

  auto addModule(Module const& module) const
      -> futures::Future<Result> override {
    return vocbase.server().getFeature<WasmServerFeature>().addModule(module);
  }

  auto removeModule(ModuleName const& name) const
      -> futures::Future<Result> override {
    return vocbase.server().getFeature<WasmServerFeature>().removeModule(name);
  }

  auto allModules() const
      -> futures::Future<ResultT<std::vector<ModuleName>>> override {
    return vocbase.server().getFeature<WasmServerFeature>().allModules();
  }

  auto module(ModuleName const& name) const
      -> futures::Future<ResultT<Module>> override {
    return vocbase.server().getFeature<WasmServerFeature>().module(name);
  }

  auto executeFunction(ModuleName const& moduleName,
                       FunctionName const& functionName,
                       FunctionInput const& parameters) const
      -> futures::Future<ResultT<FunctionOutput>> override {
    return vocbase.server().getFeature<WasmServerFeature>().executeFunction(
        moduleName, functionName, parameters);
  }

  TRI_vocbase_t& vocbase;
};

auto WasmVmMethods::createInstance(TRI_vocbase_t& vocbase)
    -> std::shared_ptr<WasmVmMethods> {
  return std::make_shared<WasmVmMethodsSingleServer>(vocbase);
}
