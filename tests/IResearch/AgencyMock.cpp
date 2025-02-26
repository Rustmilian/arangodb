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

#include "AgencyMock.h"

#include <fuerte/requests.h>

#include "Agency/Store.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/NumberUtils.h"
#include "Basics/StringBuffer.h"
#include "Cluster/AgencyCallback.h"
#include "Cluster/AgencyCallbackRegistry.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"

#include <utility>

using namespace arangodb;
using namespace arangodb::network;

AsyncAgencyStorePoolConnection::AsyncAgencyStorePoolConnection(
    AgencyCache& cache, std::string endpoint)
    : fuerte::Connection(fuerte::detail::ConnectionConfiguration()),
      _cache(cache),
      _endpoint(std::move(endpoint)) {}

fuerte::Connection::State AsyncAgencyStorePoolConnection::state() const {
  return fuerte::Connection::State::Connected;
}

auto AsyncAgencyStorePoolConnection::handleRead(VPackSlice body)
    -> std::unique_ptr<fuerte::Response> {
  fuerte::ResponseHeader header;

  VPackBuffer<uint8_t> responseBuffer;
  {
    arangodb::velocypack::Builder result(responseBuffer);
    auto const success = _cache.store().readMultiple(body, result);
    auto const code =
        std::find(success.begin(), success.end(), false) == success.end()
            ? fuerte::StatusOK
            : fuerte::StatusBadRequest;

    header.contentType(fuerte::ContentType::VPack);
    header.responseCode = code;
  }

  auto response = std::make_unique<fuerte::Response>(std::move(header));
  response->setPayload(std::move(responseBuffer), 0);

  return response;
}

auto AsyncAgencyStorePoolConnection::handleWrite(VPackSlice body)
    -> std::unique_ptr<fuerte::Response> {
  auto [success, index] = _cache.applyTestTransaction(body);

  auto const code =
      std::find_if(success.begin(), success.end(),
                   [&](int i) -> bool { return i != 0; }) == success.end()
          ? fuerte::StatusOK
          : fuerte::StatusPreconditionFailed;

  VPackBuffer<uint8_t> responseBody;
  VPackBuilder bodyObj(responseBody);
  {
    {
      VPackObjectBuilder o(&bodyObj);
      bodyObj.add("results", VPackValue(VPackValueType::Array));
      for (auto const& s : success) {
        bodyObj.add(
            VPackValue((s == arangodb::consensus::APPLIED ? index : 0)));
      }
      bodyObj.close();
    }
  }

  fuerte::ResponseHeader header;
  header.contentType(fuerte::ContentType::VPack);
  header.responseCode = code;

  auto response = std::make_unique<fuerte::Response>(std::move(header));
  response->setPayload(std::move(responseBody), 0);

  return response;
}

void AsyncAgencyStorePoolConnection::cancel(){};

void AsyncAgencyStorePoolConnection::sendRequest(
    std::unique_ptr<fuerte::Request> req, fuerte::RequestCallback cb) {
  std::unique_ptr<fuerte::Response> resp;

  if (req->header.restVerb == fuerte::RestVerb::Post) {
    if (req->header.path.find("write") != std::string::npos) {
      resp = handleWrite(req->slice());
    } else if (req->header.path.find("read") != std::string::npos) {
      resp = handleRead(req->slice());
    } else {
      throw std::logic_error("invalid operation");
    }
  } else {
    throw std::logic_error("only post requests for agency");
  }

  cb(fuerte::Error::NoError, std::move(req), std::move(resp));
}

std::shared_ptr<fuerte::Connection> AsyncAgencyStorePoolMock::createConnection(
    fuerte::ConnectionBuilder& builder) {
  return std::make_shared<AsyncAgencyStorePoolConnection>(
      _server.getFeature<ClusterFeature>().agencyCache(),
      builder.normalizedEndpoint());
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
