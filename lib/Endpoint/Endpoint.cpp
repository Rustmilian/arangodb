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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include <errno.h>
#include <cstdint>
#include <cstring>
#include <limits>

#include "Endpoint.h"

#include "Basics/Exceptions.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/debugging.h"
#include "Basics/operating-system.h"
#include "Basics/socket-utils.h"
#include "Basics/voc-errors.h"
#include "Endpoint/EndpointIp.h"
#include "Endpoint/EndpointIpV4.h"
#include "Endpoint/EndpointIpV6.h"
#include "Endpoint/EndpointSrv.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"

#if ARANGODB_HAVE_DOMAIN_SOCKETS
#include "Endpoint/EndpointUnixDomain.h"
#endif

namespace arangodb {

using namespace arangodb::basics;

Endpoint::Endpoint(DomainType domainType, EndpointType type,
                   TransportType transport, EncryptionType encryption,
                   std::string const& specification, int listenBacklog)
    : _domainType(domainType),
      _type(type),
      _transport(transport),
      _encryption(encryption),
      _specification(specification),
      _listenBacklog(listenBacklog),
      _connected(false) {
  TRI_invalidatesocket(&_socket);
}

std::string Endpoint::uriForm(std::string const& endpoint) {
  if (endpoint.starts_with("http+tcp://")) {
    return "http://" + endpoint.substr(11);
  } else if (endpoint.starts_with("http+ssl://")) {
    return "https://" + endpoint.substr(11);
  } else if (endpoint.starts_with("tcp://")) {
    return "http://" + endpoint.substr(6);
  } else if (endpoint.starts_with("ssl://")) {
    return "https://" + endpoint.substr(6);
  } else if (endpoint.starts_with("unix://")) {
    return endpoint;
  } else if (endpoint.starts_with("http+unix://")) {
    return "unix://" + endpoint.substr(12);
  } else {
    return StaticStrings::Empty;
  }
}

std::string Endpoint::unifiedForm(std::string const& specification) {
  if (specification.size() < 7) {
    return StaticStrings::Empty;
  }

  TransportType protocol = TransportType::HTTP;

  std::string prefix("http+");
  std::string const localName("localhost");
  std::string const localIP("127.0.0.1");

  std::string copy = specification;
  StringUtils::trimInPlace(copy);
  if (copy.starts_with("https://")) {
    // turn https:// into ssl:// for convenience
    copy = "ssl://" + copy.substr(8);
  }
  if (copy.starts_with("http://")) {
    // turn http:// into tcp:// for convenience
    copy = "tcp://" + copy.substr(7);
  }

  if (copy.ends_with('/')) {
    // address ends with a slash => remove
    copy.pop_back();
  }

  size_t pos = copy.find("://");
  if (pos == std::string::npos) {
    return StaticStrings::Empty;
  }
  // lowercase schema for prefix-checks
  std::string schema = StringUtils::tolower(copy.substr(0, pos + 3));

  // read protocol from string
  if (schema.starts_with("http+") || schema.starts_with("http@")) {
    protocol = TransportType::HTTP;
    prefix = "http+";
    copy = copy.substr(5);
    schema = schema.substr(5);
  }

  if (schema.starts_with("vst+")) {
    protocol = TransportType::VST;
    prefix = "vst+";
    copy = copy.substr(4);
    schema = schema.substr(4);
  }

  if (schema.starts_with("unix://")) {
#if ARANGODB_HAVE_DOMAIN_SOCKETS
    return prefix + schema + copy.substr(7);
#else
    // no unix socket for windows
    return StaticStrings::Empty;
#endif
  }

  if (schema.starts_with("srv://")) {
#ifndef _WIN32
    return prefix + schema + copy.substr(6);
#else
    return StaticStrings::Empty;
#endif
  }

  // strip tcp:// or ssl://
  if (schema.starts_with("ssl://")) {
    prefix.append("ssl://");
  } else if (schema.starts_with("tcp://")) {
    prefix.append("tcp://");
  } else {
    return StaticStrings::Empty;
  }
  copy = StringUtils::tolower(copy.substr(6, copy.length()));

  // handle tcp or ssl
  size_t found;
  if (copy[0] == '[') {
    // ipv6
    found = copy.find("]:", 1);
    if (found != std::string::npos && found > 2 && found + 2 < copy.size()) {
      // hostname and port (e.g. [address]:port)
      return prefix + copy;
    }

    found = copy.find("]", 1);
    if (found != std::string::npos && found > 2 && found + 1 == copy.size()) {
      // hostname only (e.g. [address])
      if (protocol == TransportType::VST) {
        return prefix + copy + ":" +
               StringUtils::itoa(EndpointIp::_defaultPortVst);
      } else {
        return prefix + copy + ":" +
               StringUtils::itoa(EndpointIp::_defaultPortHttp);
      }
    }

    // invalid address specification
    return StaticStrings::Empty;
  }

  // Replace localhost with 127.0.0.1
  found = copy.find(localName);
  if (found != std::string::npos) {
    copy.replace(found, localName.length(), localIP);
  }

  // ipv4
  found = copy.find(':');
  if (found != std::string::npos && found + 1 < copy.size()) {
    // hostname and port
    return prefix + copy;
  }

  // hostname only
  if (protocol == TransportType::HTTP) {
    return prefix + copy + ":" +
           StringUtils::itoa(EndpointIp::_defaultPortHttp);
  } else {
    return prefix + copy + ":" + StringUtils::itoa(EndpointIp::_defaultPortVst);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a server endpoint object from a string value
////////////////////////////////////////////////////////////////////////////////

Endpoint* Endpoint::serverFactory(std::string const& specification,
                                  int listenBacklog, bool reuseAddress) {
  return Endpoint::factory(EndpointType::SERVER, specification, listenBacklog,
                           reuseAddress);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a client endpoint object from a string value
////////////////////////////////////////////////////////////////////////////////

Endpoint* Endpoint::clientFactory(std::string const& specification) {
  return Endpoint::factory(EndpointType::CLIENT, specification, 0, false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create an endpoint object from a string value
////////////////////////////////////////////////////////////////////////////////

Endpoint* Endpoint::factory(Endpoint::EndpointType type,
                            std::string const& specification, int listenBacklog,
                            bool reuseAddress) {
  if (specification.size() < 7) {
    return nullptr;
  }

  // backlog is only allowed for server endpoints
  TRI_ASSERT(listenBacklog == 0 || type != EndpointType::CLIENT);

  if (listenBacklog == 0 && type == EndpointType::SERVER) {
    // use some default value
    listenBacklog = 10;
  }

  std::string copy = unifiedForm(specification);
  TransportType protocol = TransportType::HTTP;

  if (copy.starts_with("http+")) {
    copy = copy.substr(5);
  } else {
    // invalid protocol
    return nullptr;
  }

  EncryptionType encryption = EncryptionType::NONE;

  if (copy.starts_with("unix://")) {
#if ARANGODB_HAVE_DOMAIN_SOCKETS
    return new EndpointUnixDomain(type, listenBacklog, copy.substr(7));
#else
    // no unix socket for windows
    return nullptr;
#endif
  }

  if (copy.starts_with("srv://")) {
    if (type != EndpointType::CLIENT) {
      return nullptr;
    }

#ifndef _WIN32
    return new EndpointSrv(copy.substr(6));
#else
    return nullptr;
#endif
  }

  if (copy.starts_with("ssl://")) {
    encryption = EncryptionType::SSL;
  } else if (!copy.starts_with("tcp://")) {
    // invalid type
    return nullptr;
  }

  // tcp or ssl
  copy = copy.substr(6);
  uint16_t defaultPort = EndpointIp::_defaultPortHttp;
  size_t found;

  if (copy[0] == '[') {
    found = copy.find("]:", 1);

    // hostname and port (e.g. [address]:port)
    if (found != std::string::npos && found > 2 && found + 2 < copy.size()) {
      int64_t value = StringUtils::int64(copy.substr(found + 2));
      // check port over-/underrun
      if (value < (std::numeric_limits<uint16_t>::min)() ||
          value > (std::numeric_limits<uint16_t>::max)()) {
        LOG_TOPIC("7ccf9", ERR, arangodb::Logger::FIXME)
            << "specified port number '" << value
            << "' is outside the allowed range";
        return nullptr;
      }
      uint16_t port = static_cast<uint16_t>(value);
      std::string host = copy.substr(1, found - 1);

      return new EndpointIpV6(type, protocol, encryption, listenBacklog,
                              reuseAddress, host, port);
    }

    found = copy.find("]", 1);

    // hostname only (e.g. [address])
    if (found != std::string::npos && found > 2 && found + 1 == copy.size()) {
      std::string host = copy.substr(1, found - 1);

      return new EndpointIpV6(type, protocol, encryption, listenBacklog,
                              reuseAddress, host, defaultPort);
    }

    // invalid address specification
    return nullptr;
  }

  // ipv4
  found = copy.find(':');

  // hostname and port
  if (found != std::string::npos && found + 1 < copy.size()) {
    int64_t value = StringUtils::int64(copy.substr(found + 1));
    // check port over-/underrun
    if (value < (std::numeric_limits<uint16_t>::min)() ||
        value > (std::numeric_limits<uint16_t>::max)()) {
      LOG_TOPIC("127ce", ERR, arangodb::Logger::FIXME)
          << "specified port number '" << value
          << "' is outside the allowed range";
      return nullptr;
    }
    uint16_t port = static_cast<uint16_t>(value);
    std::string host = copy.substr(0, found);

    return new EndpointIpV4(type, protocol, encryption, listenBacklog,
                            reuseAddress, host, port);
  }

  // hostname only
  return new EndpointIpV4(type, protocol, encryption, listenBacklog,
                          reuseAddress, copy, defaultPort);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the default endpoint (http/vstream)
////////////////////////////////////////////////////////////////////////////////

std::string Endpoint::defaultEndpoint(TransportType type) {
  switch (type) {
    case TransportType::HTTP:
      return "http+tcp://" + std::string(EndpointIp::_defaultHost) + ":" +
             StringUtils::itoa(EndpointIp::_defaultPortHttp);

    case TransportType::VST:
      return "vst+tcp://" + std::string(EndpointIp::_defaultHost) + ":" +
             StringUtils::itoa(EndpointIp::_defaultPortVst);

    default: {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "invalid transport type");
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief compare two endpoints
////////////////////////////////////////////////////////////////////////////////

bool Endpoint::operator==(Endpoint const& that) const {
  return specification() == that.specification();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief set socket timeout
////////////////////////////////////////////////////////////////////////////////

bool Endpoint::setTimeout(TRI_socket_t s, double timeout) {
  return TRI_setsockopttimeout(s, timeout);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief set common socket flags
////////////////////////////////////////////////////////////////////////////////

bool Endpoint::setSocketFlags(TRI_socket_t s) {
  if (_encryption == EncryptionType::SSL && _type == EndpointType::CLIENT) {
    // SSL client endpoints are not set to non-blocking
    return true;
  }

  // set to non-blocking, executed for both client and server endpoints
  bool ok = TRI_SetNonBlockingSocket(s);

  if (!ok) {
    LOG_TOPIC("572b6", ERR, arangodb::Logger::FIXME)
        << "cannot switch to non-blocking: " << errno << " (" << strerror(errno)
        << ")";

    return false;
  }

  // set close-on-exec flag, executed for both client and server endpoints
  ok = TRI_SetCloseOnExecSocket(s);

  if (!ok) {
    LOG_TOPIC("1ef8e", ERR, arangodb::Logger::FIXME)
        << "cannot set close-on-exit: " << errno << " (" << strerror(errno)
        << ")";

    return false;
  }

  return true;
}

std::ostream& operator<<(std::ostream& stream, Endpoint::TransportType type) {
  switch (type) {
    case Endpoint::TransportType::HTTP:
      stream << "http";
      break;
    case Endpoint::TransportType::VST:
      stream << "vst";
      break;
  }
  return stream;
}

std::ostream& operator<<(std::ostream& stream, Endpoint::EndpointType type) {
  switch (type) {
    case Endpoint::EndpointType::SERVER:
      stream << "server";
      break;
    case Endpoint::EndpointType::CLIENT:
      stream << "client";
      break;
  }
  return stream;
}

std::ostream& operator<<(std::ostream& stream, Endpoint::EncryptionType type) {
  switch (type) {
    case Endpoint::EncryptionType::NONE:
      stream << "none";
      break;
    case Endpoint::EncryptionType::SSL:
      stream << "ssl";
      break;
  }
  return stream;
}

std::ostream& operator<<(std::ostream& stream, Endpoint::DomainType type) {
  switch (type) {
    case Endpoint::DomainType::UNIX:
      stream << "unix";
      break;
    case Endpoint::DomainType::IPV4:
      stream << "ipv4";
      break;
    case Endpoint::DomainType::IPV6:
      stream << "ipv6";
      break;
    case Endpoint::DomainType::SRV:
      stream << "srv";
      break;
    case Endpoint::DomainType::UNKNOWN:
      stream << "unknown";
      break;
  }
  return stream;
}

}  // namespace arangodb
