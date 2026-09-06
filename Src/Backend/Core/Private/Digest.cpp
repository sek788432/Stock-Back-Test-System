#include "Bte/Core/Digest.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace bte::core {
namespace {

constexpr std::array<std::uint32_t, 64> roundConstants{
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

std::uint32_t readBigEndianWord(const std::span<const std::byte> bytes,
                                const std::size_t offset) {
  return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8U) |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

void appendBigEndian(std::vector<std::byte> &bytes, const std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

} // namespace

std::string sha256(const std::span<const std::byte> bytes) {
  std::vector<std::byte> padded{bytes.begin(), bytes.end()};
  padded.push_back(std::byte{0x80});
  while ((padded.size() + sizeof(std::uint64_t)) % 64 != 0) {
    padded.push_back(std::byte{0});
  }
  appendBigEndian(padded, static_cast<std::uint64_t>(bytes.size()) * 8U);

  std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                    0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                    0x1f83d9abU, 0x5be0cd19U};

  for (std::size_t chunk = 0; chunk < padded.size(); chunk += 64) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      words.at(index) = readBigEndianWord(padded, chunk + index * 4);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto sigmaZero = std::rotr(words.at(index - 15), 7) ^
                             std::rotr(words.at(index - 15), 18) ^
                             (words.at(index - 15) >> 3U);
      const auto sigmaOne = std::rotr(words.at(index - 2), 17) ^
                            std::rotr(words.at(index - 2), 19) ^
                            (words.at(index - 2) >> 10U);
      words.at(index) =
          words.at(index - 16) + sigmaZero + words.at(index - 7) + sigmaOne;
    }

    auto workingA = hash.at(0);
    auto workingB = hash.at(1);
    auto workingC = hash.at(2);
    auto workingD = hash.at(3);
    auto workingE = hash.at(4);
    auto workingF = hash.at(5);
    auto workingG = hash.at(6);
    auto workingH = hash.at(7);

    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sumOne = std::rotr(workingE, 6) ^ std::rotr(workingE, 11) ^
                          std::rotr(workingE, 25);
      const auto choose = (workingE & workingF) ^ ((~workingE) & workingG);
      const auto tempOne = workingH + sumOne + choose +
                           roundConstants.at(index) + words.at(index);
      const auto sumZero = std::rotr(workingA, 2) ^ std::rotr(workingA, 13) ^
                           std::rotr(workingA, 22);
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

    hash.at(0) += workingA;
    hash.at(1) += workingB;
    hash.at(2) += workingC;
    hash.at(3) += workingD;
    hash.at(4) += workingE;
    hash.at(5) += workingF;
    hash.at(6) += workingG;
    hash.at(7) += workingH;
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto word : hash) {
    output << std::setw(8) << word;
  }
  return output.str();
}

std::string sha256(const std::string_view text) {
  return sha256(std::as_bytes(std::span{text.data(), text.size()}));
}

} // namespace bte::core
