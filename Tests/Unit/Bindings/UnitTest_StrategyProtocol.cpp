#include "StrategyProtocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bte::bindings::python::decodeMessage;
using bte::bindings::python::encodeMessage;
using bte::bindings::python::frameCanonicalPayload;
using bte::bindings::python::maximumProtocolPayloadBytes;
using bte::bindings::python::parseCanonicalJson;
using bte::bindings::python::ProtocolMessage;
using bte::bindings::python::readMessage;
using bte::bindings::python::serializeCanonicalJson;
using bte::bindings::python::writeMessage;

TEST(StrategyProtocolTest, canonicalizesIeee754NumbersAndJcsStringEscapes) {
  const auto value = parseCanonicalJson(
      R"json({"number":333333333.33333329,"small":0.000001,"tiny":1e-7,"large":1e30,"whole":1.0,"negativeZero":-0.0,"text":"€$\u000f\u000aA'B\u0022\\\u0022/"})json");
  ASSERT_TRUE(value.ok()) << value.error().message;

  const auto bytes = serializeCanonicalJson(value.value());

  ASSERT_TRUE(bytes.ok()) << bytes.error().message;
  EXPECT_EQ(
      bytes.value(),
      R"json({"large":1e+30,"negativeZero":0,"number":333333333.3333333,"small":0.000001,"text":"€$\u000f\nA'B\"\\\"/","tiny":1e-7,"whole":1})json");
}

TEST(StrategyProtocolTest, sortsObjectKeysByUtf16CodeUnits) {
  const auto value = parseCanonicalJson(
      R"json({"€":"Euro","\r":"Carriage","﮳":"Hebrew","1":"One","😀":"Emoji","\u0080":"Control","ö":"Latin"})json");
  ASSERT_TRUE(value.ok()) << value.error().message;

  const auto bytes = serializeCanonicalJson(value.value());

  ASSERT_TRUE(bytes.ok()) << bytes.error().message;
  EXPECT_EQ(
      bytes.value(),
      R"json({"\r":"Carriage","1":"One","":"Control","ö":"Latin","€":"Euro","😀":"Emoji","﮳":"Hebrew"})json");
}

TEST(StrategyProtocolTest, rejectsDuplicatePropertiesAndOutOfModelNumbers) {
  EXPECT_FALSE(parseCanonicalJson(R"json({"key":1,"key":2})json").ok());
  EXPECT_FALSE(
      parseCanonicalJson(R"json({"value":9007199254740993})json").ok());

  auto nonFinite = nlohmann::json::object();
  nonFinite["value"] = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(serializeCanonicalJson(nonFinite).ok());
}

TEST(StrategyProtocolTest, rejectsNonFinitePayloadWhenEncodingAnEnvelope) {
  const auto encoded = encodeMessage(ProtocolMessage{
      .protocolVersion = 1,
      .runId = "run-123",
      .sequence = 0,
      .messageType = "callback-result",
      .payload =
          nlohmann::json{{"value", std::numeric_limits<double>::infinity()}},
  });

  ASSERT_FALSE(encoded.ok());
  EXPECT_EQ(encoded.error().code,
            bte::core::ErrorCode::strategyProtocolViolation);
}

TEST(StrategyProtocolTest, framesAtTheExactPayloadBoundary) {
  const auto makePayload = [](const std::size_t byteCount) {
    constexpr std::string_view prefix = R"json({"data":")json";
    constexpr std::string_view suffix = R"json("})json";
    return std::string{prefix} +
           std::string(byteCount - prefix.size() - suffix.size(), 'x') +
           std::string{suffix};
  };

  const auto below =
      frameCanonicalPayload(makePayload(maximumProtocolPayloadBytes - 1));
  const auto exact =
      frameCanonicalPayload(makePayload(maximumProtocolPayloadBytes));
  const auto over =
      frameCanonicalPayload(makePayload(maximumProtocolPayloadBytes + 1));

  EXPECT_TRUE(below.ok());
  ASSERT_TRUE(exact.ok()) << exact.error().message;
  EXPECT_EQ(exact.value().size(), maximumProtocolPayloadBytes + 36);
  EXPECT_FALSE(over.ok());
  EXPECT_FALSE(
      parseCanonicalJson(std::string(maximumProtocolPayloadBytes + 1, ' '))
          .ok());
  EXPECT_FALSE(
      serializeCanonicalJson(std::string(maximumProtocolPayloadBytes + 1, 'x'))
          .ok());
}

TEST(StrategyProtocolTest, encodesAndChecksClosedEnvelopeIdentityAndSequence) {
  const ProtocolMessage message{
      .protocolVersion = 1,
      .runId = "run-123",
      .sequence = 7,
      .messageType = "callback-result",
      .payload = nlohmann::json{{"commands", nlohmann::json::array()},
                                {"success", true}},
  };
  const auto frame = encodeMessage(message);
  ASSERT_TRUE(frame.ok()) << frame.error().message;

  const auto decoded = decodeMessage(frame.value(), "run-123", 7);

  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value().messageType, "callback-result");
  EXPECT_EQ(decoded.value().sequence, 7U);
  EXPECT_EQ(decoded.value().payload, message.payload);
  EXPECT_FALSE(decodeMessage(frame.value(), "another-run", 7).ok());
  EXPECT_FALSE(decodeMessage(frame.value(), "run-123", 8).ok());
}

TEST(StrategyProtocolTest, rejectsUnknownEnvelopeKeysAndNonCanonicalFrames) {
  auto message = nlohmann::json{
      {"messageType", "ready"}, {"payload", nlohmann::json::object()},
      {"protocolVersion", 1},   {"runId", "run-123"},
      {"sequence", 0},          {"unexpected", true}};
  const auto canonical = serializeCanonicalJson(message);
  ASSERT_TRUE(canonical.ok());
  const auto unknownKeyFrame = frameCanonicalPayload(canonical.value());
  ASSERT_TRUE(unknownKeyFrame.ok());
  EXPECT_FALSE(decodeMessage(unknownKeyFrame.value(), "run-123", 0).ok());

  const auto nonCanonical = frameCanonicalPayload(
      R"json({ "messageType":"ready","payload":{},"protocolVersion":1,"runId":"run-123","sequence":0})json");
  ASSERT_TRUE(nonCanonical.ok());
  EXPECT_FALSE(decodeMessage(nonCanonical.value(), "run-123", 0).ok());
}

TEST(StrategyProtocolTest, rejectsFrameDigestLengthAndTrailingByteViolations) {
  const auto frame = encodeMessage({.protocolVersion = 1,
                                    .runId = "run-123",
                                    .sequence = 0,
                                    .messageType = "ready",
                                    .payload = nlohmann::json::object()});
  ASSERT_TRUE(frame.ok()) << frame.error().message;
  auto invalidDigest = frame.value();
  invalidDigest[4] ^= 0x01;
  EXPECT_FALSE(decodeMessage(invalidDigest, "run-123", 0).ok());
  auto trailing = frame.value();
  trailing.push_back(0);
  EXPECT_FALSE(decodeMessage(trailing, "run-123", 0).ok());
}

TEST(StrategyProtocolTest,
     rejectsInvalidEnvelopeTypesVersionsSequencesAndIdentity) {
  const auto makeFrame = [](nlohmann::json envelope) {
    const auto payload = serializeCanonicalJson(envelope);
    if (!payload.ok()) {
      return bte::core::Result<std::string>{payload.error()};
    }
    return frameCanonicalPayload(payload.value());
  };
  const auto baseEnvelope = [] {
    return nlohmann::json{{"messageType", "ready"},
                          {"payload", nlohmann::json::object()},
                          {"protocolVersion", 1},
                          {"runId", "run-123"},
                          {"sequence", 0}};
  };
  const std::array<
      std::pair<std::string_view, std::function<void(nlohmann::json &)>>, 8>
      invalidEnvelopes{{
          {"negative version",
           [](nlohmann::json &value) { value["protocolVersion"] = -1; }},
          {"string version",
           [](nlohmann::json &value) { value["protocolVersion"] = "1"; }},
          {"unsupported version",
           [](nlohmann::json &value) { value["protocolVersion"] = 2; }},
          {"negative sequence",
           [](nlohmann::json &value) { value["sequence"] = -1; }},
          {"string sequence",
           [](nlohmann::json &value) { value["sequence"] = "0"; }},
          {"non-string run id",
           [](nlohmann::json &value) { value["runId"] = 123; }},
          {"non-string message type",
           [](nlohmann::json &value) { value["messageType"] = false; }},
          {"empty message type",
           [](nlohmann::json &value) { value["messageType"] = ""; }},
      }};

  for (const auto &[label, mutate] : invalidEnvelopes) {
    auto envelope = baseEnvelope();
    mutate(envelope);
    const auto frame = makeFrame(std::move(envelope));
    ASSERT_TRUE(frame.ok()) << label << ": " << frame.error().message;
    const auto decoded = decodeMessage(frame.value(), "run-123", 0);
    EXPECT_FALSE(decoded.ok()) << label;
    EXPECT_EQ(decoded.error().code,
              bte::core::ErrorCode::strategyProtocolViolation)
        << label;
  }

  const auto valid = makeFrame(baseEnvelope());
  ASSERT_TRUE(valid.ok()) << valid.error().message;
  EXPECT_FALSE(decodeMessage(valid.value(), "other-run", 0).ok());
  EXPECT_FALSE(decodeMessage(valid.value(), "run-123", 1).ok());
  EXPECT_FALSE(decodeMessage(valid.value(), "run-123",
                             std::numeric_limits<std::uint64_t>::max())
                   .ok());

  EXPECT_FALSE(encodeMessage({.protocolVersion = 2,
                              .runId = "run-123",
                              .sequence = 0,
                              .messageType = "ready",
                              .payload = nlohmann::json::object()})
                   .ok());
  EXPECT_FALSE(encodeMessage({.protocolVersion = 1,
                              .runId = "",
                              .sequence = 0,
                              .messageType = "ready",
                              .payload = nlohmann::json::object()})
                   .ok());
  EXPECT_FALSE(encodeMessage({.protocolVersion = 1,
                              .runId = "run-123",
                              .sequence = 0,
                              .messageType = "",
                              .payload = nlohmann::json::object()})
                   .ok());
  EXPECT_FALSE(
      encodeMessage({.protocolVersion = 1,
                     .runId = "run-123",
                     .sequence = std::numeric_limits<std::uint64_t>::max(),
                     .messageType = "ready",
                     .payload = nlohmann::json::object()})
          .ok());
}

TEST(StrategyProtocolTest, rejectsMalformedFramesAndUnconfiguredStreams) {
  EXPECT_FALSE(decodeMessage("short", "run-123", 0).ok());

  const auto invalidJson = frameCanonicalPayload("not-json");
  ASSERT_TRUE(invalidJson.ok()) << invalidJson.error().message;
  EXPECT_FALSE(decodeMessage(invalidJson.value(), "run-123", 0).ok());

  const std::string oversizedHeader{"\0\x10\0\x01", 4};
  std::size_t headerOffset = 0;
  const auto oversizedReader =
      [&oversizedHeader, &headerOffset](const std::span<std::byte> destination)
      -> bte::core::Result<std::size_t> {
    const auto count =
        std::min(destination.size(), oversizedHeader.size() - headerOffset);
    std::memcpy(destination.data(), oversizedHeader.data() + headerOffset,
                count);
    headerOffset += count;
    return count;
  };
  const auto oversized = readMessage(oversizedReader, "run-123", 0);
  ASSERT_FALSE(oversized.ok());
  EXPECT_EQ(oversized.error().code,
            bte::core::ErrorCode::strategyProtocolViolation);

  EXPECT_FALSE(readMessage({}, "run-123", 0).ok());
  const auto failingReader =
      [](const std::span<std::byte>) -> bte::core::Result<std::size_t> {
    return bte::core::makeError(bte::core::ErrorCode::internal,
                                "injected read failure");
  };
  const auto readFailure = readMessage(failingReader, "run-123", 0);
  ASSERT_FALSE(readFailure.ok());
  EXPECT_EQ(readFailure.error().code, bte::core::ErrorCode::internal);

  const auto zeroReader =
      [](const std::span<std::byte>) -> bte::core::Result<std::size_t> {
    return std::size_t{0};
  };
  const auto earlyEof = readMessage(zeroReader, "run-123", 0);
  ASSERT_FALSE(earlyEof.ok());
  EXPECT_EQ(earlyEof.error().code,
            bte::core::ErrorCode::strategyProtocolViolation);
  const auto oversizedRead = [](const std::span<std::byte> destination)
      -> bte::core::Result<std::size_t> { return destination.size() + 1; };
  EXPECT_FALSE(readMessage(oversizedRead, "run-123", 0).ok());
}

TEST(StrategyProtocolTest, rejectsInvalidWriterProgressAndPropagatesIoErrors) {
  const ProtocolMessage message{.protocolVersion = 1,
                                .runId = "run-123",
                                .sequence = 0,
                                .messageType = "ready",
                                .payload = nlohmann::json::object()};
  EXPECT_FALSE(writeMessage({}, message).ok());

  const auto failingWriter =
      [](const std::span<const std::byte>) -> bte::core::Result<std::size_t> {
    return bte::core::makeError(bte::core::ErrorCode::internal,
                                "injected write failure");
  };
  const auto writeFailure = writeMessage(failingWriter, message);
  ASSERT_FALSE(writeFailure.ok());
  EXPECT_EQ(writeFailure.error().code, bte::core::ErrorCode::internal);

  const auto zeroWriter =
      [](const std::span<const std::byte>) -> bte::core::Result<std::size_t> {
    return std::size_t{0};
  };
  const auto noProgress = writeMessage(zeroWriter, message);
  ASSERT_FALSE(noProgress.ok());
  EXPECT_EQ(noProgress.error().code,
            bte::core::ErrorCode::strategyProtocolViolation);

  const auto excessiveWriter = [](const std::span<const std::byte> bytes)
      -> bte::core::Result<std::size_t> { return bytes.size() + 1; };
  const auto excessiveProgress = writeMessage(excessiveWriter, message);
  ASSERT_FALSE(excessiveProgress.ok());
  EXPECT_EQ(excessiveProgress.error().code,
            bte::core::ErrorCode::strategyProtocolViolation);

  const ProtocolMessage invalidMessage{.protocolVersion = 2,
                                       .runId = "run-123",
                                       .sequence = 0,
                                       .messageType = "ready",
                                       .payload = nlohmann::json::object()};
  const auto encodedInvalidMessage = encodeMessage(invalidMessage);
  ASSERT_FALSE(encodedInvalidMessage.ok());
  const auto encodingFailure = writeMessage(zeroWriter, invalidMessage);
  ASSERT_FALSE(encodingFailure.ok());
  EXPECT_EQ(encodingFailure.error().code,
            bte::core::ErrorCode::strategyProtocolViolation);
}

TEST(StrategyProtocolTest, readsAndWritesFragmentedStreamsAndRejectsEarlyEof) {
  const ProtocolMessage message{.protocolVersion = 1,
                                .runId = "run-123",
                                .sequence = 4,
                                .messageType = "callback-result",
                                .payload = nlohmann::json{{"success", true}}};
  const auto expectedFrame = encodeMessage(message);
  ASSERT_TRUE(expectedFrame.ok()) << expectedFrame.error().message;

  std::string writtenBytes;
  const auto writeSome = [&writtenBytes](const std::span<const std::byte> bytes)
      -> bte::core::Result<std::size_t> {
    const auto count = std::min<std::size_t>(bytes.size(), 3);
    writtenBytes.append(reinterpret_cast<const char *>(bytes.data()), count);
    return count;
  };
  ASSERT_TRUE(writeMessage(writeSome, message).ok());
  EXPECT_EQ(writtenBytes, expectedFrame.value());

  const auto makeReader = [](const std::string &bytes) {
    auto offset = std::make_shared<std::size_t>(0);
    return [bytes, offset](const std::span<std::byte> destination)
               -> bte::core::Result<std::size_t> {
      if (*offset == bytes.size()) {
        return std::size_t{0};
      }
      const auto count = std::min<std::size_t>(destination.size(), 2);
      const auto available = std::min(count, bytes.size() - *offset);
      std::memcpy(destination.data(), bytes.data() + *offset, available);
      *offset += available;
      return available;
    };
  };
  const auto decoded = readMessage(makeReader(writtenBytes), "run-123", 4);
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value().payload, message.payload);

  const auto truncated =
      readMessage(makeReader(writtenBytes.substr(0, 10)), "run-123", 4);
  EXPECT_FALSE(truncated.ok());
  EXPECT_EQ(truncated.error().code,
            bte::core::ErrorCode::strategyProtocolViolation);
}

TEST(StrategyProtocolTest, rejectsExcessiveNestingBeforeBuildingTheJsonTree) {
  const std::string nested(130, '[');
  const std::string closing(130, ']');
  EXPECT_FALSE(parseCanonicalJson(nested + "0" + closing).ok());
}

} // namespace
