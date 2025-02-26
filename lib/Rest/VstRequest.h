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
/// @author Achim Brandt
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stddef.h>
#include <cstdint>
#include <memory>

#include <velocypack/Buffer.h>
#include <velocypack/Slice.h>

#include "Endpoint/Endpoint.h"
#include "Rest/GeneralRequest.h"

namespace arangodb {
struct ConnectionInfo;
namespace velocypack {
class Builder;
}  // namespace velocypack

class VstRequest final : public GeneralRequest {
 public:
  VstRequest(ConnectionInfo const& connectionInfo,
             velocypack::Buffer<uint8_t> buffer, size_t payloadOffset,
             uint64_t messageId);

  ~VstRequest();

  size_t contentLength() const noexcept override;
  std::string_view rawPayload() const override;
  velocypack::Slice payload(bool strictValidation = true) override;
  void setPayload(arangodb::velocypack::Buffer<uint8_t> buffer) override;

  void setDefaultContentType() noexcept override {
    _contentType = rest::ContentType::VPACK;
  }

  Endpoint::TransportType transportType() override {
    return Endpoint::TransportType::VST;
  }

 private:
  void setHeader(velocypack::Slice key, velocypack::Slice content);

  void parseHeaderInformation();

  /// message header and request body are in the same body
  size_t _payloadOffset;
  /// @brief was VPack payload validated
  bool _validatedPayload;
};
}  // namespace arangodb
