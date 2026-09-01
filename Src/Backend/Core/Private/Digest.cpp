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

std::string sha256(const std::span<const std::byte> input) {
  std::vector<std::byte> padded{input.begin(), input.end()};
  padded.push_back(std::byte{0x80});
  while ((padded.size() + sizeof(std::uint64_t)) % 64 != 0) {
    padded.push_back(std::byte{0});
  }
  appendBigEndian(padded, static_cast<std::uint64_t>(input.size()) * 8U);

  std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                    0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                    0x1f83d9abU, 0x5be0cd19U};

  for (std::size_t chunk = 0; chunk < padded.size(); chunk += 64) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      words[index] = readBigEndianWord(padded, chunk + index * 4);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto s0 = std::rotr(words[index - 15], 7) ^
                      std::rotr(words[index - 15], 18) ^
                      (words[index - 15] >> 3U);
      const auto s1 = std::rotr(words[index - 2], 17) ^
                      std::rotr(words[index - 2], 19) ^
                      (words[index - 2] >> 10U);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];

    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choose = (e & f) ^ ((~e) & g);
      const auto temp1 =
          h + sum1 + choose + roundConstants[index] + words[index];
      const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = sum0 + majority;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
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
