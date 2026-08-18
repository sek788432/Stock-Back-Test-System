#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bte::data {

struct StreamRequest {
  enum class Source : std::uint8_t { automatic, duckdb, csv };

  std::string symbol;
  std::string schemaName;
  core::DateRange range{};
  std::filesystem::path csvDir =
      std::filesystem::path{"StockData"} / "Extracted";
  int prefetchBars = 4096;
  Source source = Source::automatic;
};

class BarStream {
public:
  virtual ~BarStream() = default;
  BarStream(const BarStream &) = delete;
  BarStream &operator=(const BarStream &) = delete;
  BarStream(BarStream &&) = delete;
  BarStream &operator=(BarStream &&) = delete;

  [[nodiscard]] virtual std::optional<core::Bar> next() = 0;
  [[nodiscard]] virtual std::int64_t totalBars() const noexcept = 0;
  [[nodiscard]] virtual std::int64_t consumed() const noexcept = 0;
  virtual void reset() noexcept = 0;

protected:
  BarStream() = default;
};

class CsvBarStream final : public BarStream {
private:
  struct ConstructionKey final {};

public:
  [[nodiscard]] static core::Result<std::unique_ptr<CsvBarStream>>
  open(const StreamRequest &request,
       const core::CancellationToken &cancellation = {});

  CsvBarStream(ConstructionKey, std::string symbol, std::string schemaName,
               core::DateRange range, std::vector<core::Bar> bars);

  [[nodiscard]] std::optional<core::Bar> next() override;
  [[nodiscard]] std::int64_t totalBars() const noexcept override;
  [[nodiscard]] std::int64_t consumed() const noexcept override;
  void reset() noexcept override;
  [[nodiscard]] core::DateRange range() const noexcept;
  [[nodiscard]] const std::string &symbol() const noexcept;
  [[nodiscard]] const std::string &schemaName() const noexcept;
  [[nodiscard]] std::optional<core::Bar> at(std::int64_t barIndex) const;
  [[nodiscard]] bool seek(std::int64_t barIndex) noexcept;

private:
  std::string symbol_;
  std::string schemaName_;
  core::DateRange range_{};
  std::vector<core::Bar> bars_;
  std::int64_t consumed_ = 0;
};

} // namespace bte::data
