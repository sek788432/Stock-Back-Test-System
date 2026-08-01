#pragma once

#include "Bte/Core/Bar.h"
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
    enum class Source { auto_, duckdb, csv };

    std::string symbol;
    std::string schemaName;
    core::DateRange range{};
    std::filesystem::path csvDir = std::filesystem::path{"StockData"} / "Extracted";
    int prefetchBars = 4096;
    Source source = Source::auto_;
};

class BarStream {
  public:
    virtual ~BarStream() = default;

    [[nodiscard]] virtual std::optional<core::Bar> next() = 0;
    [[nodiscard]] virtual std::int64_t totalBars() const noexcept = 0;
    [[nodiscard]] virtual std::int64_t consumed() const noexcept = 0;
    [[nodiscard]] virtual core::DateRange range() const noexcept = 0;
    [[nodiscard]] virtual std::string symbol() const = 0;
    [[nodiscard]] virtual std::string schemaName() const = 0;
    [[nodiscard]] virtual std::optional<core::Bar> at(std::int64_t barIndex) const = 0;
    [[nodiscard]] virtual bool seek(std::int64_t barIndex) noexcept = 0;
};

class CsvBarStream final : public BarStream {
  private:
    struct ConstructionKey final {};

  public:
    [[nodiscard]] static core::Result<std::unique_ptr<CsvBarStream>> open(const StreamRequest& request);

    CsvBarStream(ConstructionKey, std::string symbol, std::string schemaName, core::DateRange range,
                 std::vector<core::Bar> bars);

    [[nodiscard]] std::optional<core::Bar> next() override;
    [[nodiscard]] std::int64_t totalBars() const noexcept override;
    [[nodiscard]] std::int64_t consumed() const noexcept override;
    [[nodiscard]] core::DateRange range() const noexcept override;
    [[nodiscard]] std::string symbol() const override;
    [[nodiscard]] std::string schemaName() const override;
    [[nodiscard]] std::optional<core::Bar> at(std::int64_t barIndex) const override;
    [[nodiscard]] bool seek(std::int64_t barIndex) noexcept override;

  private:
    std::string symbol_;
    std::string schemaName_;
    core::DateRange range_{};
    std::vector<core::Bar> bars_;
    std::int64_t consumed_ = 0;
};

} // namespace bte::data
