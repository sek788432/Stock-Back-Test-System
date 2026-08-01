# 04 — Data Layer

How the C++ app reads bars from DuckDB and CSV, exposes them as a `BarStream`, and stays compatible with the existing Python pipeline (`DataFetcher/`).

The Python side **owns the schema**. The C++ side is a strict consumer: it never writes, and it tolerates schema additions but fails loudly on schema breakage.

---

## 1. Sources of truth (today)

From `README.md` and `DataFetcher/`:

- DuckDB file: `StockData/MarketData.duckdb`
- Single table: `hourlyBars`
- Columns (per `GetFromDb.py`):
  - core: `symbol, ts, open, high, low, close, volume`
  - provenance: `source, schemaName, ingestedAt`
- `ts` is `TIMESTAMPTZ`, UTC.
- `schemaName` indicates the bar interval (`ohlcv-1s`, `ohlcv-1m`, `ohlcv-1h`, `ohlcv-1d`, ...). The table name is **legacy** — `hourlyBars` does **not** mean hourly only.
- Multiple `schemaName` values can coexist for one symbol; consumers must filter.

CSV snapshots in `StockData/Extracted/<SYMBOL>.csv` mirror the same columns, with optional provenance columns. Header is the source of truth for column order.

---

## 2. Public API

```cpp
namespace bte::data {

struct DataSourceConfig {
    std::filesystem::path duckDbPath;             // primary source
    std::optional<std::filesystem::path> csvDir;  // fallback / portable mode
    bool readOnly = true;                         // always true; assert at open
};

class DataSource {
public:
    static core::Result<std::unique_ptr<DataSource>> open(const DataSourceConfig& cfg);

    // Discovery
    core::Result<std::vector<std::string>> listSymbols() const;
    core::Result<std::vector<std::string>> listSchemasFor(std::string_view symbol) const;
    core::Result<core::DateRange>          rangeFor(std::string_view symbol,
                                                    std::string_view schemaName) const;
    core::Result<int64_t>                  rowCount(std::string_view symbol,
                                                    std::string_view schemaName,
                                                    core::DateRange range) const;

    // Streaming
    core::Result<std::unique_ptr<BarStream>> openStream(const StreamRequest& req);

    // Cache control
    void clearCache();
    Stats stats() const;

    virtual ~DataSource();
};

}  // namespace bte::data
```

### 2.1 `BarStream`

```cpp
class BarStream {
public:
    virtual ~BarStream() = default;

    virtual std::optional<core::Bar> next() = 0;        // returns nullopt on end
    virtual int64_t totalBars() const = 0;              // -1 if unknown (live feed someday)
    virtual int64_t consumed() const = 0;
    virtual core::DateRange range() const = 0;
    virtual std::string symbol() const = 0;
    virtual std::string schemaName() const = 0;

    // Random access (only supported by file-backed streams; returns nullopt if not)
    virtual std::optional<core::Bar> at(int64_t barIndex) = 0;
    virtual bool seek(int64_t barIndex) = 0;
};
```

`next()` is the hot path. It must be **non-blocking**: the engine pulls a bar, consumes it, and pulls again. Implementations buffer ahead in a worker thread (see §4).

### 2.2 `StreamRequest`

```cpp
struct StreamRequest {
    std::string symbol;
    std::string schemaName;             // required; resolves multi-schema ambiguity
    core::DateRange range;              // half-open; defaults to full available
    int prefetchBars = 4096;            // ring buffer size
    enum class Source { auto_, duckdb, csv } source = Source::auto_;
};
```

Resolution order when `source == auto_`:
1. If DuckDB has rows that fully cover the range → DuckDB.
2. Else, if `<csvDir>/<symbol>.csv` exists and covers the range → CSV.
3. Else `Result::error(ErrorCode::dataUnavailable)`.

---

## 3. DuckDB adapter

We use the **DuckDB C++ amalgamation** (`duckdb.hpp`) vendored in `ThirdParty/duckdb/`. Read-only.

### 3.1 Schema discovery at startup

```cpp
// pseudo-implementation
auto cols = conn.Query("PRAGMA table_info('hourlyBars')")->Materialize();
verifyHasColumns(cols, {"symbol","ts","open","high","low","close","volume"});
warnIfMissing(cols, {"source","schemaName","ingestedAt"});  // older DBs may lack
```

If required columns are missing, `open()` returns `ErrorCode::schemaMismatch`. If optional provenance columns are missing, we log a warning and continue with `schemaName="unknown"`.

### 3.2 Streaming query

For each `BarStream`, we run **one** parameterized query:

```sql
SELECT ts, open, high, low, close, volume
FROM hourlyBars
WHERE symbol = $sym
  AND schemaName = $schema
  AND ts >= $start
  AND ts <  $end
ORDER BY ts
```

DuckDB's `QueryResult` exposes a `Chunk` API; we read chunks (default 2048 rows) and feed them into the ring buffer. We never materialize the full result.

### 3.3 Range / count helpers

```sql
SELECT MIN(ts), MAX(ts), COUNT(*) FROM hourlyBars
WHERE symbol = ? AND schemaName = ?;
```

Cached for 5 seconds per `(symbol, schema)` pair to keep UI dropdowns snappy.

### 3.4 Concurrency

DuckDB allows multiple read-only connections. The `DataSource` keeps a small pool (default 4) of connections shared across streams. The Python pipeline must **not** be holding an exclusive write lock while the C++ app runs — which it isn't, because `CollectData.sh` is a one-shot batch.

If the file is locked (Windows), `open()` retries 3× over 1 second, then returns `ErrorCode::permissionDenied` with a hint.

---

## 4. CSV adapter

For users who deploy without DuckDB (e.g. data shipped as CSVs only), or for the sandbox/test harness.

### 4.1 Format

```csv
symbol,ts,open,high,low,close,volume
AAPL,2018-05-01 13:00:00+00:00,...
```

Optional provenance columns (`source, schemaName, ingestedAt`) accepted and used to populate stream metadata.

### 4.2 Parser

- `csv-parser` single-header (vendored) or hand-rolled (CSV is simple here — no quoted commas in our domain).
- Stream-friendly: open file, iterate line-by-line, parse one bar at a time.
- Timestamps parsed with `bte::core::time::parseIso8601`.
- Header row required.

### 4.3 Indexing

For random access (`at(i)`, `seek`), we lazily build a `<symbol>.csv.idx` file: 8-byte offsets per line. Built once on first `seek`, persisted next to the CSV. Invalidated if file mtime changes.

---

## 5. Prefetch & buffering

Each `BarStream` owns:

```cpp
moodycamel::ReaderWriterQueue<Bar> buffer_;   // single-producer / single-consumer
std::jthread prefetcher_;                     // produces bars
std::stop_source stopSource_;
```

- The prefetcher runs in a thread the `DataSource` owns, fills `buffer_` up to `prefetchBars`, and blocks when full.
- `next()` pops from the queue; if empty and producer alive, waits up to 100 ms then returns `nullopt` only if the producer is also done.
- Cancellation: caller drops the stream, destructor sets `stopSource_`, the prefetcher exits.

This keeps the engine loop CPU-bound on indicators + strategy, never on disk.

---

## 6. Caching

Two caches inside `DataSource`:

| Cache | Key | Value | Eviction |
|---|---|---|---|
| Range cache | `(symbol, schema)` | `(min ts, max ts, count)` | TTL 5 s, LRU 1024 entries |
| Bar cache | `(symbol, schema, barIndex / 4096)` | `std::vector<Bar>` (one chunk) | LRU 256 chunks ≈ 1M bars ≈ ~56 MB worst case |

The bar cache is what makes **replay scrubbing** instant — see `07`.

---

## 7. Multi-symbol streams (basket strategies)

For strategies that act on more than one symbol at once:

```cpp
class MergedBarStream {
public:
    static core::Result<std::unique_ptr<MergedBarStream>>
    create(std::vector<std::unique_ptr<BarStream>> streams);

    // returns the next bar across all symbols, by ts
    std::optional<core::SymbolBar> next();
};
```

Implementation is a min-heap keyed by `ts`. Required for the basket replay UI later; **not** in MVP UI but the API is here so engine doesn't need to refactor.

---

## 8. Configuration & paths

`DataSourceConfig.duckDbPath` defaults to:

1. `<repoRoot>/StockData/MarketData.duckdb` if launched from a checkout.
2. `<userData>/config/settings.json` value if set.
3. UI prompt the user once on first launch.

`csvDir` defaults to `<repoRoot>/StockData/Extracted/` when present.

---

## 9. Errors users will see

| Symptom | `ErrorCode` | UI message |
|---|---|---|
| File missing | `notFound` | "DuckDB file not found at <path>. Run the Python collector first." |
| File locked (Windows) | `permissionDenied` | "DuckDB file is in use. Close other tools and retry." |
| Symbol not in DB | `notFound` | "No data for SYMBOL." |
| Symbol has rows for >1 schema, no `schemaName` given | `invalidArgument` | "<sym> has both ohlcv-1h and ohlcv-1m. Pick one." |
| `hourlyBars` missing required columns | `schemaMismatch` | "Database schema is older than this app expects. Re-run the collector." |
| Range entirely outside data | `dataUnavailable` | "No bars between <start> and <end>." |

---

## 10. Tests

- Round-trip vs CSV: open the same range from DuckDB and from CSV, assert bar-for-bar equality (we already trust the Python pipeline's roundtrip check; this guards the C++ side).
- Schema-mismatch: open a fixture DuckDB with `hourlyBars` missing `volume` → `schemaMismatch`.
- Multi-schema ambiguity: fixture with two schemas for one symbol → `invalidArgument` if `schemaName` not specified.
- Prefetch correctness: slow consumer never causes producer to spin; cancel mid-stream; assert no leaks (`asan`).
- 1M-row stream: total wall time within budget on CI (smoke perf gate).
