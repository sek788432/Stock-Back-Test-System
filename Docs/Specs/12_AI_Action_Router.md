# 12 — AI Action Router (CLI Skills + Chrome Extension)

This spec defines how the desktop app accepts natural-language-driven instructions originating from external AI surfaces — **Claude Code / Codex on the CLI side** and **ChatGPT / Gemini on the web side via a Chrome extension** — and turns them into concrete UI/state changes inside the Qt app.

It is the technical realization of Owner 4's pillar — "**AI chat → script / manipulate front end**" — and refines the **NL authoring mode** described in [`11_Stock_Screener_KLine_Product.md`](11_Stock_Screener_KLine_Product.md) §3.

When this document disagrees with `02_Frontend_Qt.md` or `11_Stock_Screener_KLine_Product.md` on the placement of the NL authoring entry point, this file is the **implementation contract**; the older specs describe the in-app surface that this router augments (and may eventually replace for the AI-driven flows).

---

## 1. Purpose & scope

### 1.1 Problem

The product needs natural-language-driven authoring (Spec `11` §3) and front-end manipulation. Three concerns make a Qt-embedded chat panel a poor first step:

1. **No LLM credentials.** The team does not want to ship, store, or proxy API keys. Users already pay for ChatGPT Plus / Gemini Advanced; we should ride along.
2. **AI tooling moves fast.** Claude Code and Codex Desktop already have mature "skills" mechanisms. Re-implementing a chat host inside Qt duplicates that work.
3. **Owner 4 should not be blocked on Owner 5's Qt UI timeline.** A thin contract layer between AI and Qt lets the AI side ship independently against a mock view-model.

### 1.2 Solution at a glance

A single **action JSON** contract carried from any AI surface to the Qt app:

```
repo skills (single spec)
   ├──► Claude Code / Codex      (native skill loading on CLI)
   └──► ChatGPT / Gemini         (Chrome extension injects skills as system prompt)
                │
                ▼
        action JSON               (unified intermediate format)
                ▼
        Qt parser + localhost     (Owner 4 owns)
                ▼
        Qt view-models            (Owner 5 owns)
                ▼
        UI updated
```

### 1.3 Hard constraints

| # | Constraint | Source |
|---|---|---|
| C1 | The router process MUST NOT call any external LLM API. No `OPENAI_API_KEY`, no Anthropic SDK in this module. | Team decision (no-API-key principle). |
| C2 | The Qt app MUST NOT write to `StockData/MarketData.duckdb`. | `Governance/AGENTS.md` §2 H2. |
| C3 | All engine-affecting behaviour MUST stay deterministic. | `07_Engine_Replay_PnL.md` §8. The router only loads/saves artifacts; it does not perturb engine state directly. |
| C4 | NL → artifact requires explicit user acceptance before compile/run. | `11_Stock_Screener_KLine_Product.md` §3.2. This spec adapts the acceptance gate to the conversation level (see §8.2). |
| C5 | New third-party deps require an ADR + entry in `Decisions/dependencies.md`. | `Governance/AGENTS.md` §6 / H9. |

### 1.4 Out of scope

- Live brokerage execution.
- Running a backtest from AI input (deferred to Scope 2; see §10).
- Bidirectional AI ↔ app queries (deferred to Scope 3).
- Multi-user / multi-Qt-instance coordination.
- Chrome Web Store distribution (MVP is unpacked dev load).
- Persistent pairing across Qt restarts.
- Non-English skill content. AI replies in whatever language the user uses; the skill specification itself is English.

---

## 2. Architecture

Five layers, top to bottom:

| # | Layer | Owner | Notes |
|---|---|---|---|
| 1 | **Skills** (spec layer) | Owner 4 | Markdown + frontmatter in `.cursor/skills/stockbt-actions/`. Single source of truth for what the app supports and how AI should emit JSON. |
| 2 | **AI entry points** (two, parallel) | Existing tooling (CLI) + Owner 4 (web) | CLI: Claude Code / Codex load skills natively. Web: Chrome extension injects skill content into ChatGPT / Gemini conversations. |
| 3 | **Action JSON** (contract layer) | Owner 4 | Versioned envelope. Identical shape regardless of producer. |
| 4 | **Qt parser + localhost endpoint** | Owner 4 | New module `Src/Backend/AiActionRouter/`. cpp-httplib server, dispatch table, auth. |
| 5 | **Qt view-models** | Owner 5 | Existing MVVM layer (`02_Frontend_Qt.md`). Owner 4 calls; does not edit. |

Owner 4's net-new code surface:

1. `.cursor/skills/stockbt-actions/` — skill spec files (markdown + JSON Schema).
2. `Src/Backend/AiActionRouter/` — Qt-side HTTP server, dispatcher, action implementations.
3. `Extension/` — Chrome extension (Manifest V3, TypeScript).

The only inter-owner contract is the set of Qt view-model methods invoked by the router. See §6.6.

---

## 3. Skill format

### 3.1 Location and granularity

```
.cursor/skills/
  stockbt-actions/
    SKILL.md          # main entry — markdown with frontmatter
    schema.json       # machine-readable JSON Schema for the envelope and actions
    examples.md       # optional, conversational worked examples
```

**One master skill** for MVP — Scope 1 has only four actions; a single file keeps Claude Code, Codex, and the extension all reading the same artifact. If Scope 3 introduces many actions (~20+) the skill can be split per action; the path layout above accommodates that without breaking consumers.

### 3.2 SKILL.md shape

```markdown
---
name: stockbt-actions
description: |
  Use when the user wants the Stock-Backtest app to load a strategy or
  screener, save a draft as a file, or otherwise act on the local app.
version: 1
contract: stockbt-action-v1
---

# Stock-Backtest action emitter

When the user expresses an intent that should affect the local
Stock-Backtest app, emit one or more `stockbt-action` JSON blocks
in your reply.

## Output convention

Wrap action JSON in a fenced code block tagged `stockbt-action`:

    ```stockbt-action
    {
      "version": "1",
      "actions": [ { "type": "load_strategy_by_path", "path": "..." } ]
    }
    ```

## Available actions

### apply_strategy_python
…
```

### 3.3 Detection marker

Action JSON is detected by a **fenced code block whose info string is `stockbt-action`**. Rationale:

- Markdown-native, so the same text renders the same way in ChatGPT, Gemini, and any CLI chat surface.
- AI models default to placing JSON inside code blocks; we only need them to use the correct language hint.
- Users see and can copy the block from the chat history.

Alternatives considered (XML tags, envelope-field detection across all `json` blocks) and rejected for either UI noise or false-positive risk. See `Decisions/0005-ai-action-router-architecture.md` (to be filed).

### 3.4 Skill serving on the web side

The Chrome extension does not bundle the skill content. It fetches `GET http://127.0.0.1:<port>/skills` after pairing and caches the markdown in `chrome.storage.local`. This way:

- The skill versions automatically with the Qt app — no extension republish on skill edit.
- The extension cannot be tricked into using a stale spec, because re-pairing forces a refresh.

---

## 4. Action JSON contract

### 4.1 Envelope

```json
{
  "version": "1",
  "actions": [ /* ordered list, executed sequentially */ ],
  "id": "<optional uuid-v4 or any unique string>",
  "stopOnError": true
}
```

| Field | Required | Notes |
|---|---|---|
| `version` | yes | Protocol version string. MVP = `"1"`. Unknown versions → HTTP 400. |
| `actions` | yes | Ordered array. Must contain at least 1 and at most 10 actions. Outside this range → HTTP 400. |
| `id` | no | Producer-supplied identifier; the router uses it for 24h dedup on POST retries. |
| `stopOnError` | no | Default `true`. If `false`, the router attempts every action regardless of per-action failure. |

### 4.2 Scope 1 actions

Four actions, two semantic groups.

**`apply_*` — AI-authored artifact, save and load atomically.** The web AI has no filesystem; it provides the source inline, the router writes it to disk under the user's data directory, then loads it.

```json
{
  "type": "apply_strategy_python",
  "name": "rsi_30_70",
  "source": "import bte\n\ndef on_bar(ctx, bar):\n    ..."
}
```

- `name` must match `[a-zA-Z0-9_-]+`, max 64 chars. No `/`, `..`, or leading `.`.
- Saved path: `~/.stockBacktester/strategies/<name>.py`.
- After save, calls `StrategyEditorVm::loadFromPath(<path>)`.

```json
{
  "type": "apply_screener_rule",
  "name": "value_picks",
  "rule": { /* rule.json object per `05_Strategy_Authoring.md` §3 */ }
}
```

- Saved path: `~/.stockBacktester/screeners/<name>.rule.json`.
- After save, calls `ScreenerEditorVm::loadFromPath(<path>)`.

**`load_*_by_path` — load an existing artifact already on disk.** Used when the user references files they already have. The router resolves the absolute path with `std::filesystem::canonical` and rejects any path outside the per-user whitelist directories.

```json
{ "type": "load_strategy_by_path", "path": "/Users/me/.stockBacktester/strategies/rsi.py" }
{ "type": "load_screener_by_path", "path": "/Users/me/.stockBacktester/screeners/value.rule.json" }
```

### 4.3 Response

```json
{
  "ok": true,
  "id": "echo-from-request",
  "results": [
    {
      "index": 0,
      "type": "apply_strategy_python",
      "ok": true,
      "savedPath": "/Users/me/.stockBacktester/strategies/rsi_30_70.py"
    }
  ]
}
```

| HTTP | When |
|---|---|
| 200 | Envelope was valid. Per-action `ok` may be `true`, `false`, or `null` (`skipped` after a `stopOnError` cut-off). |
| 400 | Envelope-level failure: unparseable JSON, unsupported `version`, empty `actions`, body too large. |
| 401 | Missing / invalid bearer token. |
| 413 | Payload exceeds the 500 KB envelope limit or 100 KB per-action `source` limit. |
| 429 | Rate-limit exceeded (default 10 envelopes per token per second). |

### 4.4 Versioning

| Change | How |
|---|---|
| Add a new action type (e.g., Scope 2 `open_replay`) | Keep `version: "1"`. Router responds with `{ok:false, error:"unsupported action type"}` if it does not recognise the type — clients fall back gracefully. |
| Modify an existing action's schema | Bump `version` to `"2"`. Router supports `"1"` and `"2"` in parallel for at least one release cycle. |
| Skill `contract:` mismatches router's supported list | Extension warns the user "update the app" and refuses to inject the skill. |

---

## 5. Chrome extension

Manifest V3. TypeScript source under `Extension/`. Built with esbuild / vite. MVP distribution is unpacked dev load.

### 5.1 Components

| File | Responsibility |
|---|---|
| `service-worker.ts` | Token storage, `POST /actions`, toast notifications, settings persistence in `chrome.storage.local`. |
| `content-scripts/<platform>.ts` | DOM injection per platform — MutationObserver on the chat container, extracts `stockbt-action` blocks, injects the "Activate Stock-BT" button. |
| `popup/popup.{html,ts}` | Toolbar popup — pairing UI (PIN entry), connection status, "Confirm each action" toggle, last-N action log. |
| `platform-adapters/` | `chatgpt.ts`, `gemini.ts` implementing a common adapter interface (`findCodeBlocks`, `injectActivationButton`, `prependToNextMessage`). New platforms = new adapter file. |

### 5.2 Permissions

```json
"permissions": ["storage", "activeTab"],
"host_permissions": [
  "https://chatgpt.com/*",
  "https://gemini.google.com/*",
  "http://127.0.0.1/*"
]
```

### 5.3 Activation model

A floating button labelled "🤖 Activate Stock-BT" is injected by the content script when a new conversation starts on `chatgpt.com` or `gemini.google.com`. Clicking it:

1. Fetches the cached skill content from `chrome.storage.local`.
2. Prepends the skill content to the user's next message (or sends it as a hidden first message — see §5.4).
3. Marks the conversation as "stockbt-active" in extension state for that tab.

Subsequent user messages in the same conversation receive no further injection; the model has the skill context for the rest of the session.

Alternative activation models (slash command, auto-on-every-chat) considered and rejected — see ADR.

### 5.4 Skill injection mechanics

Two acceptable approaches; the implementation picks whichever is more reliable per platform:

- **Prepend to user's next message** — the content script intercepts the send action, prepends a `<details>` block containing the skill, then submits. Visible-but-collapsible.
- **Auto-send as first message** — content script clicks send with the skill as the first user turn. AI acknowledges briefly, then user types.

Either way the skill content is visible in the conversation history. We do not mask it from the user.

### 5.5 Pairing — PIN flow

1. On Qt startup, the router generates a fresh 6-digit numeric PIN (e.g. `482913`), valid for **5 minutes**, **single-use**. Displayed in Qt's status bar.
2. User opens the extension popup → enters the PIN → popup `POST /pair {pin}` to `127.0.0.1:<port>`.
3. Router validates PIN and (within the 5-minute window) returns a long-lived bearer token. PIN is then invalidated.
4. Extension stores `{token, port}` in `chrome.storage.local`.
5. Extension makes `GET /skills` with the token to fetch the skill content and cache it.

Token is held only in Qt process memory — when Qt restarts, the token is gone and the user must re-pair. MVP accepts this tradeoff for simplicity and security; persistent tokens are out of scope.

### 5.6 Confirmation UX

A toggle in the popup, **off by default**, labelled "Confirm each action before applying". When on:

- Service worker shows a `chrome.notifications.create` (or in-popup confirm dialog) before each `POST /actions`, summarising what will happen.
- User taps "Apply" or "Cancel".

The toggle is hard-coded **on and immutable** for actions that destructive-affect engine state (Scope 2 and later — see §10).

---

## 6. Qt parser + localhost endpoint

### 6.1 Module location

```
Src/Backend/AiActionRouter/
├── HttpServer.{h,cpp}        # cpp-httplib wrapper, bind 127.0.0.1
├── ActionDispatcher.{h,cpp}  # dispatch table, schema validation
├── Authn.{h,cpp}             # PIN generation, token issuance, request authentication
├── SkillsLoader.{h,cpp}      # reads .cursor/skills/stockbt-actions/SKILL.md from repo
└── Actions/
    ├── ApplyStrategyPython.{h,cpp}
    ├── ApplyScreenerRule.{h,cpp}
    ├── LoadStrategyByPath.{h,cpp}
    └── LoadScreenerByPath.{h,cpp}
Tests/AiActionRouter/
├── UnitTest_Envelope.cpp
├── UnitTest_Authn.cpp
├── UnitTest_PathWhitelist.cpp
└── UnitTest_<each action>.cpp
```

### 6.2 HTTP endpoints

| Method | Path | Purpose | Auth |
|---|---|---|---|
| GET | `/health` | Liveness probe; returns `{ok, version, supportedActions[]}`. | none |
| POST | `/pair` | Body `{pin}`. Returns `{token}` or 401. | PIN (single-use) |
| GET | `/skills` | Returns raw SKILL.md content. | Bearer token |
| POST | `/actions` | Body = envelope. Returns result envelope. | Bearer token |

Bind only to `127.0.0.1`. `Access-Control-Allow-Origin` whitelist: `https://chatgpt.com`, `https://gemini.google.com`, and `chrome-extension://<extension-id>`.

### 6.3 HTTP library choice

[cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT, single-header). Justifications:

- Zero build-system friction — header-only, drops into CMake.
- Synchronous handler model maps cleanly onto Qt's cross-thread `invokeMethod`.
- Mature, ~10k stars, in use by many projects.

`Decisions/dependencies.md` entry: `cpp-httplib | 0.18.x | MIT | localhost HTTP server for AiActionRouter`. An ADR is also required (see §10).

QHttpServer was considered but rejected for MVP — the Qt 6 module is still TP/experimental on some distributions and has thinner documentation. Revisit if a future Qt version makes QHttpServer materially better integrated than cpp-httplib.

### 6.4 Parser pipeline

```
POST /actions                                    [HTTP thread, cpp-httplib]
  1. parse body as JSON                       → 400 on syntax error
  2. validate envelope schema                  → 400 on version/structure error
  3. for each action in envelope.actions:
       a. dispatch_table[action.type]          → result.ok=false, "unknown type"
       b. action-specific JSON schema validate → result.ok=false, "invalid params"
       c. QMetaObject::invokeMethod(
              viewModel, ..., Qt::BlockingQueuedConnection)   [→ GUI thread]
       d. collect result
       e. if !result.ok and stopOnError: break
  4. serialise result envelope, return 200
```

### 6.5 Threading

cpp-httplib runs its accept loop on a dedicated worker thread. Each request handler executes on a thread from cpp-httplib's pool. Qt widget access is gated through `QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)`, which:

- posts the view-model call onto the GUI thread's event loop,
- blocks the HTTP thread until the GUI thread completes,
- captures the result for serialisation.

This is acceptable for Scope 1 because all four actions are fast (a file write plus a view-model load, both bounded by I/O). Scope 2's `run_backtest` and similar long-running actions will require an asynchronous job pattern — see §10.

### 6.6 View-model contract with Owner 5

| Method | Caller | Notes |
|---|---|---|
| `StrategyEditorVm::loadFromPath(QString path) → Result<void, Error>` | `ApplyStrategyPython`, `LoadStrategyByPath` | Implementation owns parsing the file; router only passes the path. |
| `ScreenerEditorVm::loadFromPath(QString path) → Result<void, Error>` | `ApplyScreenerRule`, `LoadScreenerByPath` | Same shape. |

These method signatures are declared in `Src/Frontend/ViewModels/StrategyEditorVm.h` and `…/ScreenerEditorVm.h` (Owner 5's tree). Owner 5 implements them; Owner 4 calls them and tests with a fake implementation injected into the router for unit tests.

### 6.7 PIN and token

- PIN: 6 random digits, generated at Qt startup, valid 5 minutes, single-use. Brute-force surface is `10^6` over 5 minutes; combined with the per-source 10-req/min rate limit on `/pair`, the expected discovery time exceeds the validity window by orders of magnitude.
- Token: 256-bit hex string generated at successful `/pair`. Held in an in-memory `TokenStore` inside the router. Lifetime = Qt process lifetime.
- All `/skills` and `/actions` requests require `Authorization: Bearer <token>`; absence or mismatch → 401.

---

## 7. End-to-end flow — Scope 1 happy path

1. User launches the Qt app. Router starts; status bar shows `Pair PIN: 482-913 (4:59)`.
2. User opens the Chrome extension popup → enters `482913` → popup `POST /pair`. Token returned; popup displays `Paired ✓`. Extension calls `GET /skills` and caches the markdown.
3. User opens `chatgpt.com` and starts a new chat. Content script renders the "🤖 Activate Stock-BT" floating button.
4. User clicks Activate. Extension prepends the cached skill into the next outgoing user message (in a collapsible `<details>` block) and marks the conversation as stockbt-active.
5. User types: "Please write an RSI 14 strategy — buy when RSI < 30, sell when RSI > 70."
6. AI responds with prose plus the action block:
   ````
   ```stockbt-action
   {
     "version": "1",
     "actions": [{
       "type": "apply_strategy_python",
       "name": "rsi_30_70",
       "source": "import bte\n\ndef on_bar(ctx, bar):\n    ..."
     }]
   }
   ```
   ````
7. MutationObserver detects the new AI message. Adapter extracts the `stockbt-action` block. Service worker `POST /actions` to `http://127.0.0.1:<port>/actions` with the bearer token.
8. Router authenticates, parses, dispatches `apply_strategy_python`:
   - Validates `name` (`rsi_30_70` matches `[a-zA-Z0-9_-]+`).
   - Writes `~/.stockBacktester/strategies/rsi_30_70.py` with the supplied `source`.
   - `invokeMethod(strategyEditorVm, "loadFromPath", path, Qt::BlockingQueuedConnection)`.
9. View-model loads the file in the Qt editor; status bar shows `Loaded rsi_30_70.py`.
10. Router serialises `{ok:true, results:[{index:0, ok:true, savedPath:"…"}]}` and replies 200.
11. Service worker shows toast `✓ Applied: rsi_30_70.py`.

---

## 8. Cross-cutting concerns

### 8.1 Security

| Control | Detail |
|---|---|
| Bind | `127.0.0.1` only. Never `0.0.0.0`. |
| CORS | Allow-Origin: `chatgpt.com`, `gemini.google.com`, `chrome-extension://<id>`. All other origins rejected. |
| PIN | 6 digits, 5-minute validity, single-use; `/pair` rate-limited to 10/min per source IP. |
| Token | 256-bit hex, in-memory only, dies with the Qt process. |
| Path whitelist | `load_*_by_path` resolves with `std::filesystem::canonical` and rejects any path outside the per-user data dirs. Symlink escape detected and rejected. |
| Filename | `apply_*` `name` matches `[a-zA-Z0-9_-]+`, max 64 chars. |
| Payload limit | 100 KB per `source`; 500 KB per envelope; 413 on exceed. |
| Rate limit | 10 envelopes per token per second. |

We do **not** sandbox the Python `source` in this module — that is the job of the strategy host described in `05_Strategy_Authoring.md` §5 and the future Python ADR. SKILL.md must warn users that `apply` runs unverified Python and that they should review the generated source before backtesting.

### 8.2 Acceptance gate (relation to Spec 11 §3.2)

`11_Stock_Screener_KLine_Product.md` §3.2 requires "explicit user acceptance" before NL output is compiled or run. This spec satisfies that requirement at the **conversation level** rather than per-artifact:

- The "Activate Stock-BT" button is the user's explicit broad opt-in.
- The action JSON is visible in the chat transcript before it is dispatched — the user can read what will happen.
- Scope 1 actions are reversible (write a file the user can delete; load an editor the user can swap).

A per-action confirmation toggle exists in the popup for cautious users (off by default in MVP). For Scope 2 onwards, any action that mutates engine or portfolio state (`run_backtest`, future order-emitting actions) forces the confirmation toggle on with no override.

This deviation from the literal text of Spec 11 §3.2 is non-trivial and requires an ADR — see §10.

### 8.3 Errors

| Category | Trigger | User-visible result |
|---|---|---|
| Wire | Qt not running, port unreachable | Toast: "Qt app offline" |
| Wire | Connection timeout | Toast: "Qt did not respond", retry button |
| Auth | Token rejected (Qt restarted, token gone) | Popup: "Re-pair needed", prompts for new PIN |
| Auth | PIN wrong on `/pair` | Popup: "Invalid PIN" |
| Envelope | Malformed JSON / unsupported version | Toast: "AI emitted invalid JSON" + popup log entry |
| Action | Unknown action `type` | Toast: "Unsupported action: <type>" |
| Action | Validation failure (bad filename, path outside whitelist) | Toast: "<reason>" |
| Action | View-model failure | Toast carries the `Error::message` from the Qt side |
| UI | AI replied without a `stockbt-action` block | No action; user is just chatting |

All failure paths surface to the user via toast or popup. No silent failures.

### 8.4 Testing

| Layer | Approach |
|---|---|
| Qt parser unit | GoogleTest under `Tests/AiActionRouter/`. Fake view-models; no GUI started. Covers envelope parsing, schema validation, authn, path whitelist, and each of the four actions individually. |
| Qt integration | Headless Qt (`-platform offscreen`), real router, `curl` against the endpoints. Verifies end-to-end JSON round-trip. |
| Extension unit | Vitest for pure logic in `service-worker.ts` and the adapters' parsing functions. |
| Extension DOM | Playwright against fixture HTML captured from real ChatGPT / Gemini DOMs. Re-record fixtures whenever a layout change breaks a test. |
| Skill self-validation | CI step that validates every example in `SKILL.md` against `schema.json`. Prevents documentation drift. |
| End-to-end | Manual. The 11-step happy path in §7 is the acceptance test. Record video and attach to PR for each Scope graduation. |

Every public symbol in `Src/Backend/AiActionRouter/` requires a test per `10_CI_Dev_Flow.md` §7. Mutation testing applies to this module the same as any other.

### 8.5 Determinism

The router itself has no engine state and emits no orders. It only loads/saves artifacts. Determinism (`07_Engine_Replay_PnL.md` §8) is unaffected because the engine still consumes the exact same artifact format whether the artifact originated in the Qt editor or via the router.

---

## 9. Repository layout impact

```
.cursor/skills/
  stockbt-actions/        # new — Owner 4
    SKILL.md
    schema.json
    examples.md

Src/Backend/
  AiActionRouter/         # new — Owner 4
    …

Extension/                # new — Owner 4
  manifest.json
  service-worker.ts
  content-scripts/
    chatgpt.ts
    gemini.ts
  popup/
    popup.html
    popup.ts
  platform-adapters/
    common.ts
    chatgpt.ts
    gemini.ts
  package.json
  vite.config.ts

Tests/
  AiActionRouter/         # new — Owner 4
    …
```

---

## 10. Upgrade path — Scope 2 & 3

| Scope | New actions | Mechanisms required |
|---|---|---|
| **2** | `open_replay`, `open_backtest_tab`, `open_screener_tab`, `set_symbol`, `set_timeframe`, `set_date_range`, `set_initial_capital`, `run_backtest`, `run_screener`, `pause`, `resume`, `step`. | Asynchronous job pattern: `POST /actions` returns `{jobId}` for long-running actions; extension polls `GET /jobs/<id>` or opens an SSE stream. Confirmation toggle forced on for `run_*`. Protocol `version` remains `"1"`. |
| **3** | `query_current_state`, `query_loaded_strategy`, `export_trade_log`, `export_equity_curve`, `compare_strategies`, `screenshot_chart`, `set_theme`. | Bidirectional channel: WebSocket from Qt to extension, or long-poll on `/events`. Envelope gains a `responseRequired` field. Protocol `version` bumps to `"2"`; Qt supports `"1"` and `"2"` concurrently. |

Scope graduation criteria (each Scope ships when):

- All listed actions implemented and have unit tests.
- The end-to-end demo in §7 is updated and re-recorded with the new actions.
- Acceptance-gate carve-outs for destructive actions are enforced (see §8.2).

---

## 11. Impact on existing specs

| Spec | Required change |
|---|---|
| `02_Frontend_Qt.md` | Add a section "External AI integration via Action Router" pointing at this spec. Consider an optional "AI Console" tab that surfaces the action log for transparency. |
| `05_Strategy_Authoring.md` | Add a cross-reference noting that strategy artifacts may also originate from the Action Router; the compile/lint pipeline is unchanged. |
| `11_Stock_Screener_KLine_Product.md` | Add a note in §3 that the NL-mode user surface is now realised primarily via the Chrome extension defined here, retaining the §3.2 acceptance requirement (interpreted per §8.2). |
| `10_CI_Dev_Flow.md` | Add CI jobs: `AiActionRouter` unit/integration test job, extension build + Vitest job, skill schema-validation job. |
| `Decisions/dependencies.md` | Add `cpp-httplib`. |
| `Governance/owner.md` and `Team_Ownership_And_Product_Pillars.md` | Update Owner 4's surface to explicitly include `AiActionRouter`, the Chrome extension, and the `.cursor/skills/stockbt-actions/` skill. |

### 11.1 ADRs to file (before implementation)

- **`0005-ai-action-router-architecture.md`** — captures the unified skills + JSON contract, the no-LLM-credential constraint, and the choice of cpp-httplib (per `Governance/AGENTS.md` §5 and §6).
- **`0006-acceptance-gate-at-conversation-level.md`** — captures the §8.2 deviation from Spec 11 §3.2 and the carve-out for Scope 2 destructive actions.

---

## 12. Traceability matrix

| Concern | Primary sources |
|---|---|
| Product NL surface | `11_Stock_Screener_KLine_Product.md` §3, this spec |
| Strategy artifact format | `05_Strategy_Authoring.md` §3 (rule.json), §5 (Python host) |
| UI binding for loaded artifacts | `02_Frontend_Qt.md` §2 (MVVM, editors) |
| Error model | `03_Backend_Core.md` §6 (`Result<T, Error>`) |
| Threading | `01_Architecture.md` §3, `00_Overview.md` §4 |
| CI gates | `10_CI_Dev_Flow.md` §3 / §5 / §7 |
| Acceptance gate (NL) | `11_Stock_Screener_KLine_Product.md` §3.2, this spec §8.2 |
| Dependency governance | `Governance/AGENTS.md` §6, `Decisions/dependencies.md` |
