#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace bte::core {

class Sha256Accumulator final {
public:
  void update(std::span<const std::byte> bytes);
  // Returns a digest snapshot; later updates continue from the same input.
  [[nodiscard]] std::string finalize() const;

private:
  void compressBlock(std::span<const std::byte, 64> block);

  std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                      0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                      0x1f83d9abU, 0x5be0cd19U};
  std::array<std::byte, 64> buffer_{};
  std::uint64_t totalBytes_ = 0;
  std::size_t bufferedBytes_ = 0;
};

[[nodiscard]] std::string sha256(std::span<const std::byte> bytes);
[[nodiscard]] std::string sha256(std::string_view text);

} // namespace bte::core
