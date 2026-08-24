# DataFetcher

Python tooling that fetches Databento OHLCV aggregates into DuckDB and exports CSV snapshots. This module is the **only writer** to `StockData/MarketData.duckdb`. The database is a developer ingestion/verification store; the V1 release design builds an immutable snapshot from the extracted CSV inputs.

For the system architecture, see [`Docs/Specs/`](../Docs/Specs/README.md) (in
particular [`DataLayer.md`](../Docs/Specs/DataLayer.md)).

---

## Data layout

| Path                          | Purpose                                                                           |
| ----------------------------- | --------------------------------------------------------------------------------- |
| `StockData/Symbols.txt`       | List of tickers to fetch or export                                                |
| `StockData/MarketData.duckdb` | DuckDB database (created when you collect)                                        |
| `StockData/Extracted/`        | CSV files produced by extraction (one file per symbol; the current reviewed snapshot is tracked) |
| `DataFetcher/`                | `FetchDatabento.py`, `GetFromDb.py`, and their pinned Python requirements          |

The fetch scripts locate the **project root** by walking upward from `DataFetcher/` until they find **`StockData/Symbols.txt`**, or use **`STOCK_REPO_ROOT`** (see below).

Bar interval must be a **native Databento schema** (`ohlcv-1s`, **`ohlcv-1m`**, **`ohlcv-1h`**, `ohlcv-1d`, `ohlcv-eod`; there is no **`ohlcv-30min`**). Configure the schema and time range via CLI flags or env vars (**`--schema-name` / `SCHEMA_NAME`**, **`--start-ts` / `START_TS`**, **`--end-ts` / `END_TS`**). The default end timestamp is fixed (see `DEFAULT_END_TS` in **`FetchDatabento.py`**); bump it or pass **`--end-ts`** to extend the window. For 30‑minute bars, fetch finer data (e.g. `ohlcv-1m`) and resample in pandas. The DuckDB table **`hourlyBars`** keeps a legacy name; the real interval is **`schemaName`** on each row.

---

## Prerequisites (Python pipeline)

Create a virtual environment at the **repository root** (recommended):

```bash
python3 -m venv .venv
source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install -r DataFetcher/requirements.txt
```

## API key (Databento)

Export the key in the shell that runs the fetcher:

```bash
export DATABENTO_API_KEY='your-key-here'
```

The checked-in Python entry points do not load a `.env` file. Do not commit
credentials. Rotate the key immediately if it is exposed.

---

## Symbol list (`StockData/Symbols.txt`)

- One symbol per line.
- Blank lines and lines starting with `#` are ignored.
- Allowed characters per line: letters, digits, `._-` only (consistent with **`GetFromDb.py`** extraction).

Example:

```text
# US equities
AAPL
TSLA
```

## Collect data (API → DuckDB)

From the **repository root**:

```bash
python3 DataFetcher/FetchDatabento.py
```

Writes **`StockData/MarketData.duckdb`** and upserts rows for every symbol using the configured schema (default `ohlcv-1h`).

Optional:

```bash
python3 DataFetcher/FetchDatabento.py \
  --symbol-file path/to/other_symbols.txt \
  --db-path path/to/other.duckdb \
  --schema-name ohlcv-1m \
  --start-ts 2020-01-01T00:00:00Z \
  --end-ts 2024-12-31T23:59:59Z \
  --symbol-batch-size 25
```

**Large watchlists (e.g. S&P 500–sized):** Databento may return **`402 account_insufficient_funds`** when a request exceeds **prepaid / per-call** limits. The fetcher splits by **fewer symbols** first (stderr note). If **`402`** persists for **one symbol** across the full **`--start-ts` … `--end-ts`** window, it **bisects the clock range**, merges the pieces, and retries until slices fit—or until each slice is down to roughly **one hour** of calendar span (then warns once, skips that symbol, and continues; widen credits / use a coarser schema / narrow the window to recover it). Tune **`--symbol-batch-size`** / **`SYMBOL_BATCH_SIZE`** (default **50**; **`0`** batches every symbol in one call). **[Databento billing](https://databento.com/docs/portal/billing)**.

**Parallel batches (default):** **`--parallel-batches`** / **`PARALLEL_BATCHES`** (default **4**, **`1`** = serial) drives multiple Databento fetches concurrently with **`ThreadPoolExecutor`**, while **`saveToDuckdb`** + **`roundtripCheck`** remain single-threaded as each future completes (DuckDB has one writer). Lower this number if you start hitting **`429`** rate-limit errors; raise it only when your connection and Databento entitlements permit more concurrency.

**Re-running after a partial fetch:** By default, **`FetchDatabento.py`** skips symbols that **already have rows in DuckDB** spanning **`--start-ts` through `--end-ts`** (same **`--schema-name`**, `source='databento'`), so you can resume without re-billing earlier batches. Coverage uses **asymmetric slack** because markets don't trade overnight: a generous start slack (≥ 2 days, ~48 bars) accepts that the first stored bar lags the calendar window-start by hours/weekends, while a tight end slack (≥ 5 min, ~2 bars) ensures that extending **`--end-ts`** triggers a fresh fetch of the new tail. Use **`--force-full`** if you want every symbol re-requested (e.g. after changing how you ingest data).

**Unknown or wrong tickers:** If Databento rejects a symbol (e.g. not on **`XNAS.ITCH`**, delisted, typo), the fetcher **bisects that batch**, prints **`Warning: skipping …`** to **stderr** for each bad symbol, and keeps loading the rest. Use **`--strict-symbols`** to fail fast on the first batch error instead.

**`422 data_end_after_available_end`:** your **`--end-ts`** / **`END_TS`** is past the dataset's **`available_end`**. Lower **`END_TS`** to the latest time Databento reports for **`XNAS.ITCH`** (see [datasets](https://databento.com/docs/api-reference-historical/basics/datasets)).

Before saving, fetched bars are validated: non-positive prices, negative volume, and OHLC range violations (`high < max(open,close,low)` or `low > min(open,close,high)`) abort the run. After saving, an invariant check asserts `rowsAfter == rowsBefore + newKeys`, then a roundtrip diff confirms every fetched value matches DuckDB.

---

## Extract data (DuckDB → CSV)

Requires an existing **`StockData/MarketData.duckdb`** (run collect first unless you passed a custom **`--db-path`**):

```bash
python3 DataFetcher/GetFromDb.py
```

Reads **`StockData/Symbols.txt`** by default and writes one CSV **per symbol that has matching rows** under **`StockData/Extracted/`** (e.g. `AAPL.csv`; dots in tickers become underscores in filenames). Symbols with **no matching rows do not produce a CSV**—and any **stale CSV** already on disk with that name is **deleted**. Each file contains only the OHLCV columns (`symbol, ts, open, high, low, close, volume`) and timestamps are emitted in **UTC** so output is reproducible across machines.

Optional:

```bash
python3 DataFetcher/GetFromDb.py \
  --out-dir path/to/dir \
  --symbol-file path/to/symbols.txt \
  --db-path path/to/MarketData.duckdb \
  --schema-name ohlcv-1h \
  --include-provenance
```

If **`--schema-name`** is omitted and a symbol has rows under more than one schema in DuckDB, extraction errors out instead of silently mixing intervals — pass **`--schema-name ohlcv-1h`** (or whatever schema you fetched) to disambiguate. Pass **`--include-provenance`** to also emit `source`, `schemaName`, and `ingestedAt` columns. If **any requested symbol has zero matching rows**, the script prints a warning and exits with status **2** (no CSV is written for those symbols).

**Why two symbols can have different row counts:** trade-derived **`ohlcv-1h`** bars are emitted **per symbol**; sparse hours may be **absent** for one ticker but present for another. That is normal vendor behavior—not a CSV bug. Align series in pandas/SQL by reindexing to a fixed hourly calendar if needed.

---

## Environment variables

| Variable            | Meaning                                                                                                                                                                                                   |
| ------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `DATABENTO_API_KEY` | Required for `FetchDatabento.py`. Export it in the invoking shell.                                                                                                                                        |
| `STOCK_REPO_ROOT`   | Absolute path to repo root containing **`StockData/`**. Use if auto-discovery via **`StockData/Symbols.txt`** fails.                                                                                      |
| `SCHEMA_NAME`       | Default schema for **`FetchDatabento.py`** when **`--schema-name`** is not passed (default `ohlcv-1h`).                                                                                                   |
| `START_TS`          | Default ISO‑8601 UTC start for **`FetchDatabento.py`** (default `2018-05-01T00:00:00Z`).                                                                                                                  |
| `END_TS`            | Default ISO‑8601 UTC end for **`FetchDatabento.py`** (see **`DEFAULT_END_TS`** in code; bump or override with `--end-ts`; must stay **on or before** the dataset **`available_end`** to avoid **`422`**). |
| `SYMBOL_BATCH_SIZE` | Default symbols per request for **`FetchDatabento.py`** when **`--symbol-batch-size`** is not passed (**`50`**; use **`0`** for a single request for all symbols).                                        |
| `PARALLEL_BATCHES`  | Default concurrent Databento fetches for **`FetchDatabento.py`** when **`--parallel-batches`** is not passed (**`4`**; **`1`** = serial). DuckDB writes always serialized.                                |

**Repo root discovery:** Export `STOCK_REPO_ROOT` yourself when the scripts are
outside their normal checkout layout. Otherwise both Python entry points walk
upward from `DataFetcher/` until they find `StockData/Symbols.txt`.

---

## Typical workflow

1. Edit **`StockData/Symbols.txt`**.
2. `python3 DataFetcher/FetchDatabento.py` — populate or update `StockData/MarketData.duckdb`.
3. `python3 DataFetcher/GetFromDb.py` — export `StockData/Extracted/<symbol>.csv`.

---

## Command help

From the repository root with `.venv` activated and **`DATABENTO_API_KEY`** exported if fetching:

```bash
python3 DataFetcher/FetchDatabento.py --help
python3 DataFetcher/GetFromDb.py --help
```
