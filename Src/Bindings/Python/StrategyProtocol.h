#pragma once

#include <Bte/Core/Result.h>

#include <cstddef>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace bte::bindings::python {

inline constexpr std::size_t maximumProtocolPayloadBytes =
    std::size_t{1024} * 1024;
inline constexpr std::uint32_t currentProtocolVersion = 1;

struct ProtocolMessage {
  std::uint32_t protocolVersion = currentProtocolVersion;
  std::string runId;
  std::uint64_t sequence = 0;
  std::string messageType;
  nlohmann::json payload;
};

using ProtocolReadSome =
    std::function<core::Result<std::size_t>(std::span<std::byte>)>;
using ProtocolWriteSome =
    std::function<core::Result<std::size_t>(std::span<const std::byte>)>;

[[nodiscard]] core::Result<nlohmann::json>
parseCanonicalJson(std::string_view bytes);

[[nodiscard]] core::Result<std::string>
serializeCanonicalJson(const nlohmann::json &value);

[[nodiscard]] core::Result<std::string>
frameCanonicalPayload(std::string_view payloadBytes);

[[nodiscard]] core::Result<std::string>
encodeMessage(const ProtocolMessage &message);

[[nodiscard]] core::Result<ProtocolMessage>
decodeMessage(const std::string &frame, std::string_view expectedRunId,
              std::uint64_t expectedSequence);

[[nodiscard]] core::Result<ProtocolMessage>
readMessage(const ProtocolReadSome &readSome, std::string_view expectedRunId,
            std::uint64_t expectedSequence);

[[nodiscard]] core::Result<void>
writeMessage(const ProtocolWriteSome &writeSome,
             const ProtocolMessage &message);

} // namespace bte::bindings::python
