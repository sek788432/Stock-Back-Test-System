#include "StrategyProtocol.h"

#include <Bte/Core/CanonicalJson.h>
#include <Bte/Core/Digest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>

namespace bte::bindings::python {
namespace {

constexpr std::uint64_t maximumSafeInteger = 9007199254740991ULL;
constexpr std::size_t frameHeaderBytes = 4;
constexpr std::size_t sha256DigestBytes = 32;
constexpr std::size_t framePrefixBytes = frameHeaderBytes + sha256DigestBytes;

[[nodiscard]] core::Error protocolError(std::string message) {
  return core::makeError(core::ErrorCode::strategyProtocolViolation,
                         std::move(message));
}

[[nodiscard]] std::string digestBytes(std::string_view bytes) {
  const auto hexadecimal = core::sha256(bytes);
  std::string result;
  result.reserve(sha256DigestBytes);
  const auto nibble = [](const char character) -> std::uint8_t {
    if (character >= '0' && character <= '9') {
      return static_cast<std::uint8_t>(character - '0');
    }
    return static_cast<std::uint8_t>(character - 'a' + 10);
  };
  for (std::size_t index = 0; index + 1 < hexadecimal.size(); index += 2) {
    result.push_back(static_cast<char>((nibble(hexadecimal[index]) << 4U) |
                                       nibble(hexadecimal[index + 1])));
  }
  return result;
}

[[nodiscard]] std::uint32_t
readBigEndianLength(const std::span<const std::byte> bytes) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < frameHeaderBytes; ++index) {
    value = (value << 8U) | std::to_integer<std::uint8_t>(bytes[index]);
  }
  return value;
}

[[nodiscard]] core::Result<void>
readExact(const ProtocolReadSome &readSome,
          const std::span<std::byte> destination) {
  std::size_t offset = 0;
  while (offset < destination.size()) {
    const auto read = readSome(destination.subspan(offset));
    if (!read.ok()) {
      return read.error();
    }
    if (read.value() == 0 || read.value() > destination.size() - offset) {
      return protocolError(
          "Protocol stream ended before the frame was complete.");
    }
    offset += read.value();
  }
  return {};
}

[[nodiscard]] core::Result<void>
writeExact(const ProtocolWriteSome &writeSome,
           const std::span<const std::byte> source) {
  std::size_t offset = 0;
  while (offset < source.size()) {
    const auto written = writeSome(source.subspan(offset));
    if (!written.ok()) {
      return written.error();
    }
    if (written.value() == 0 || written.value() > source.size() - offset) {
      return protocolError("Protocol stream made no valid forward progress.");
    }
    offset += written.value();
  }
  return {};
}

void appendBigEndianLength(std::string &bytes, const std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    bytes.push_back(
        static_cast<char>((value >> static_cast<unsigned int>(shift)) & 0xffU));
  }
}

[[nodiscard]] bool hasExactEnvelopeKeys(const nlohmann::json &value) {
  if (!value.is_object() || value.size() != 5) {
    return false;
  }
  return value.contains("protocolVersion") && value.contains("runId") &&
         value.contains("sequence") && value.contains("messageType") &&
         value.contains("payload");
}

[[nodiscard]] bool readSafeUnsigned(const nlohmann::json &value,
                                    std::uint64_t &result) {
  if (value.is_number_unsigned()) {
    result = value.get<std::uint64_t>();
  } else if (value.is_number_integer()) {
    const auto signedValue = value.get<std::int64_t>();
    if (signedValue < 0) {
      return false;
    }
    result = static_cast<std::uint64_t>(signedValue);
  } else {
    return false;
  }
  return result <= maximumSafeInteger;
}

} // namespace

core::Result<nlohmann::json> parseCanonicalJson(const std::string_view bytes) {
  if (bytes.size() > maximumProtocolPayloadBytes) {
    return protocolError("JSON payload exceeds the protocol limit.");
  }
  const auto parsed = core::parseCanonicalJson(bytes);
  if (!parsed.ok()) {
    return protocolError(parsed.error().message);
  }
  return parsed.value();
}

core::Result<std::string> serializeCanonicalJson(const nlohmann::json &value) {
  auto serialized = core::serializeCanonicalJson(value);
  if (!serialized.ok()) {
    return protocolError(serialized.error().message);
  }
  if (serialized.value().size() > maximumProtocolPayloadBytes) {
    return protocolError("JSON payload exceeds the protocol limit.");
  }
  return serialized.value();
}

core::Result<std::string>
frameCanonicalPayload(const std::string_view payloadBytes) {
  if (payloadBytes.size() > maximumProtocolPayloadBytes) {
    return protocolError("JSON payload exceeds the protocol limit.");
  }
  std::string frame;
  frame.reserve(framePrefixBytes + payloadBytes.size());
  appendBigEndianLength(frame, static_cast<std::uint32_t>(payloadBytes.size()));
  frame += digestBytes(payloadBytes);
  frame.append(payloadBytes.data(), payloadBytes.size());
  return frame;
}

core::Result<std::string> encodeMessage(const ProtocolMessage &message) {
  if (message.protocolVersion != currentProtocolVersion ||
      message.runId.empty() || message.messageType.empty() ||
      message.sequence > maximumSafeInteger) {
    return protocolError("Protocol message metadata is invalid.");
  }
  const nlohmann::json envelope{
      {"messageType", message.messageType},
      {"payload", message.payload},
      {"protocolVersion", message.protocolVersion},
      {"runId", message.runId},
      {"sequence", message.sequence},
  };
  const auto canonical = serializeCanonicalJson(envelope);
  if (!canonical.ok()) {
    return canonical.error();
  }
  return frameCanonicalPayload(canonical.value());
}

core::Result<ProtocolMessage>
decodeMessage(const std::string &frame, const std::string_view expectedRunId,
              const std::uint64_t expectedSequence) {
  if (frame.size() < framePrefixBytes) {
    return protocolError("Protocol frame is shorter than its header.");
  }
  const auto prefix = std::as_bytes(std::span{frame.data(), frameHeaderBytes});
  const auto payloadLength = readBigEndianLength(prefix);
  if (payloadLength > maximumProtocolPayloadBytes ||
      frame.size() != framePrefixBytes + payloadLength) {
    return protocolError(
        "Protocol frame length is invalid or has trailing bytes.");
  }
  const auto payloadBytes =
      std::string_view{frame}.substr(framePrefixBytes, payloadLength);
  const auto expectedDigest = digestBytes(payloadBytes);
  std::uint8_t digestDifference = 0;
  for (std::size_t index = 0; index < sha256DigestBytes; ++index) {
    digestDifference |= static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(frame[frameHeaderBytes + index]) ^
        static_cast<std::uint8_t>(expectedDigest[index]));
  }
  if (digestDifference != 0) {
    return protocolError("Protocol frame SHA-256 digest does not match.");
  }

  const auto value = parseCanonicalJson(payloadBytes);
  if (!value.ok()) {
    return value.error();
  }
  const auto canonical = serializeCanonicalJson(value.value());
  if (!canonical.ok() || canonical.value() != payloadBytes) {
    return protocolError(
        "Protocol frame payload is not RFC 8785 canonical JSON.");
  }
  if (!hasExactEnvelopeKeys(value.value())) {
    return protocolError(
        "Protocol message envelope has unknown or missing properties.");
  }

  const auto &envelope = value.value();
  std::uint64_t version = 0;
  std::uint64_t sequence = 0;
  if (!readSafeUnsigned(envelope.at("protocolVersion"), version) ||
      version != currentProtocolVersion ||
      !readSafeUnsigned(envelope.at("sequence"), sequence) ||
      !envelope.at("runId").is_string() ||
      !envelope.at("messageType").is_string() ||
      envelope.at("runId").get_ref<const std::string &>() != expectedRunId ||
      sequence != expectedSequence || expectedSequence > maximumSafeInteger ||
      envelope.at("messageType").get_ref<const std::string &>().empty()) {
    return protocolError(
        "Protocol message version, identity, sequence, or type is invalid.");
  }

  return ProtocolMessage{
      .protocolVersion = static_cast<std::uint32_t>(version),
      .runId = envelope.at("runId").get<std::string>(),
      .sequence = sequence,
      .messageType = envelope.at("messageType").get<std::string>(),
      .payload = envelope.at("payload"),
  };
}

core::Result<ProtocolMessage>
readMessage(const ProtocolReadSome &readSome,
            const std::string_view expectedRunId,
            const std::uint64_t expectedSequence) {
  if (!readSome) {
    return protocolError("Protocol reader is not configured.");
  }
  std::array<std::byte, frameHeaderBytes> lengthBytes{};
  const auto headerRead = readExact(readSome, lengthBytes);
  if (!headerRead.ok()) {
    return headerRead.error();
  }
  const auto payloadLength = readBigEndianLength(lengthBytes);
  if (payloadLength > maximumProtocolPayloadBytes) {
    return protocolError("Protocol frame exceeds the payload limit.");
  }

  std::string frame(framePrefixBytes + payloadLength, '\0');
  std::memcpy(frame.data(), lengthBytes.data(), lengthBytes.size());
  auto remainingBytes = std::as_writable_bytes(std::span{
      frame.data() + frameHeaderBytes, frame.size() - frameHeaderBytes});
  const auto bodyRead = readExact(readSome, remainingBytes);
  if (!bodyRead.ok()) {
    return bodyRead.error();
  }
  return decodeMessage(frame, expectedRunId, expectedSequence);
}

core::Result<void> writeMessage(const ProtocolWriteSome &writeSome,
                                const ProtocolMessage &message) {
  if (!writeSome) {
    return protocolError("Protocol writer is not configured.");
  }
  const auto frame = encodeMessage(message);
  if (!frame.ok()) {
    return frame.error();
  }
  const auto bytes =
      std::as_bytes(std::span{frame.value().data(), frame.value().size()});
  return writeExact(writeSome, bytes);
}

} // namespace bte::bindings::python
