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

#include "GlobalExecutorMethods.h"

using namespace arangodb;

std::string_view GlobalExecutorMethods::name(
    GlobalExecutorMethods::MethodType type) noexcept {
  switch (type) {
    case MethodType::kReloadRouting:
      return "reloadRouting";
    case MethodType::kReloadAql:
      return "reloadAql";
  }
  return "unknown";
}

std::string_view GlobalExecutorMethods::code(
    GlobalExecutorMethods::MethodType type) noexcept {
  switch (type) {
    case MethodType::kReloadRouting:
      return "require(\"@arangodb/actions\").reloadRouting();";
    case MethodType::kReloadAql:
      return "try { require(\"@arangodb/aql\").reload(); } catch (err) {}";
  }
  return "";
}
