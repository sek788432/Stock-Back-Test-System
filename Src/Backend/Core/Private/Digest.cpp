#include "Bte/Core/Digest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace bte::core {
namespace {

constexpr std::size_t roundCount = 64;
constexpr std::array<std::uint32_t, roundCount> roundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

// SHA-256 uses fixed, nonzero rotations. Keep their normalization out of the
// measured hashing hot path, including unoptimized coverage builds.
template <unsigned int shift>
  requires(shift > 0 && shift < 32)
constexpr std::uint32_t rotateRight(const std::uint32_t value) noexcept {
  return (value >> shift) | (value << (32U - shift));
}

std::uint32_t readBigEndianWord(const std::span<const std::byte> bytes,
                                const std::size_t offset) {
  return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8U) |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

} // namespace

void Sha256Accumulator::update(const std::span<const std::byte> bytes) {
  constexpr std::size_t blockSize = 64;
  totalBytes_ += static_cast<std::uint64_t>(bytes.size());
  std::size_t offset = 0;

  if (bufferedBytes_ > 0) {
    const auto count = std::min(blockSize - bufferedBytes_, bytes.size());
    std::ranges::copy(bytes.first(count), buffer_.begin() + bufferedBytes_);
    bufferedBytes_ += count;
    offset += count;
    if (bufferedBytes_ == blockSize) {
      compressBlock(std::span<const std::byte, blockSize>{buffer_});
      bufferedBytes_ = 0;
    }
    if (bufferedBytes_ > 0) {
      return;
    }
  }

  while (bytes.size() - offset >= blockSize) {
    compressBlock(std::span<const std::byte, blockSize>{
        bytes.subspan(offset, blockSize)});
    offset += blockSize;
  }

  const auto remainder = bytes.subspan(offset);
  std::ranges::copy(remainder, buffer_.begin());
  bufferedBytes_ = remainder.size();
}

void Sha256Accumulator::compressBlock(
    const std::span<const std::byte, 64> block) {
  constexpr std::span<const std::uint32_t, roundCount> constants{
      roundConstants};
  std::array<std::uint32_t, roundCount> words{};
  const std::span<std::uint32_t, roundCount> schedule{words};
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = readBigEndianWord(block, index * 4);
  }
  for (std::size_t index = 16; index < schedule.size(); ++index) {
    const auto sigmaZero = rotateRight<7>(schedule[index - 15]) ^
                           rotateRight<18>(schedule[index - 15]) ^
                           (schedule[index - 15] >> 3U);
    const auto sigmaOne = rotateRight<17>(schedule[index - 2]) ^
                          rotateRight<19>(schedule[index - 2]) ^
                          (schedule[index - 2] >> 10U);
    schedule[index] =
        schedule[index - 16] + sigmaZero + schedule[index - 7] + sigmaOne;
  }
  auto workingA = state_[0];
  auto workingB = state_[1];
  auto workingC = state_[2];
  auto workingD = state_[3];
  auto workingE = state_[4];
  auto workingF = state_[5];
  auto workingG = state_[6];
  auto workingH = state_[7];

  for (std::size_t index = 0; index < schedule.size(); ++index) {
    const auto sumOne = rotateRight<6>(workingE) ^ rotateRight<11>(workingE) ^
                        rotateRight<25>(workingE);
    const auto choose = (workingE & workingF) ^ ((~workingE) & workingG);
    const auto tempOne =
        workingH + sumOne + choose + constants[index] + schedule[index];
    const auto sumZero = rotateRight<2>(workingA) ^ rotateRight<13>(workingA) ^
                         rotateRight<22>(workingA);
    const auto majority =
        (workingA & workingB) ^ (workingA & workingC) ^ (workingB & workingC);
    const auto tempTwo = sumZero + majority;
    workingH = workingG;
    workingG = workingF;
    workingF = workingE;
    workingE = workingD + tempOne;
    workingD = workingC;
    workingC = workingB;
    workingB = workingA;
    workingA = tempOne + tempTwo;
  }

  state_[0] += workingA;
  state_[1] += workingB;
  state_[2] += workingC;
  state_[3] += workingD;
  state_[4] += workingE;
  state_[5] += workingF;
  state_[6] += workingG;
  state_[7] += workingH;
}

std::string Sha256Accumulator::finalize() const {
  constexpr std::size_t blockSize = 64;
  auto finalized = *this;
  std::array<std::byte, blockSize * 2> tail{};
  auto tailBytes = std::span{tail};
  std::ranges::copy(std::span<const std::byte>{finalized.buffer_}.first(
                        finalized.bufferedBytes_),
                    tailBytes.begin());
  tailBytes[finalized.bufferedBytes_] = std::byte{0x80};
  const auto tailSize =
      finalized.bufferedBytes_ < blockSize - sizeof(std::uint64_t)
          ? blockSize
          : blockSize * 2;
  const auto bitLength = finalized.totalBytes_ * 8U;
  for (std::size_t index = 0; index < sizeof(bitLength); ++index) {
    tailBytes[tailSize - sizeof(bitLength) + index] =
        static_cast<std::byte>((bitLength >> (56U - index * 8U)) & 0xffU);
  }
  for (std::size_t offset = 0; offset < tailSize; offset += blockSize) {
    finalized.compressBlock(std::span<const std::byte, blockSize>{
        tailBytes.subspan(offset, blockSize)});
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto word : finalized.state_) {
    output << std::setw(8) << word;
  }
  return output.str();
}

std::string sha256(const std::span<const std::byte> bytes) {
  Sha256Accumulator accumulator;
  accumulator.update(bytes);
  return accumulator.finalize();
}

std::string sha256(const std::string_view text) {
  return sha256(std::as_bytes(std::span{text.data(), text.size()}));
}

} // namespace bte::core
