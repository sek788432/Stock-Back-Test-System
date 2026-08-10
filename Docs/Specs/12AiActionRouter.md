# 12 — AI-Assisted Strategy Authoring

> **Status:** Planned. No AI provider, browser extension, localhost action service, credential flow, or action router is implemented.

## Purpose

AI may help a user draft a Selectable Conditions plan or a Python strategy. It is an authoring assistant, not a trusted execution authority.

## Accepted safety boundary

- The application stores no vendor LLM credential and makes no direct provider call in V1.
- AI-produced text is always an untrusted candidate artifact.
- The candidate is displayed as a diff or preview and requires explicit user acceptance.
- Acceptance imports the artifact into the normal conditions or Python editor.
- Normal schema validation, Python validation, engine checks, and user confirmation still apply.
- Once accepted, Selectable Conditions execute as a typed plan in the
  project-owned C++ engine; accepted Python may submit commands through the
  managed worker, but neither AI nor Python becomes an execution engine.
- AI cannot start a backtest, place an order, change data, overwrite a strategy, delete results, or bypass a validation failure.
- Generated artifacts contain no hidden executable payload, credentials, or unrestricted file paths.

## Candidate envelope

Future integrations should exchange a versioned, declarative envelope containing:

- unique request and candidate identifiers;
- artifact kind: `conditions` or `python_strategy`;
- template/API version;
- human-readable summary and assumptions;
- complete proposed artifact;
- validation diagnostics;
- creation time and optional external provenance.

Unknown fields are ignored only when the envelope version explicitly permits it. Unknown artifact kinds or major versions are rejected.

## Interaction flow

1. User asks an external assistant for a strategy draft.
2. A candidate artifact is brought into the application through a future explicit import mechanism.
3. The application parses and validates it without execution.
4. The user reviews the complete artifact and diagnostics.
5. The user accepts, edits, or rejects it.
6. Accepted content becomes an ordinary versioned strategy artifact and follows `05` and `11`.

Transport is deliberately undecided. Clipboard, file import, or an authenticated local integration may be evaluated later. This document does not authorize a localhost listener, browser extension, socket protocol, or background service.

## Auditability

If implemented, the application records candidate ID, artifact hash, validation outcome, user decision, accepted artifact hash, and strategy API/template version. Prompts and transcripts are retained only with explicit user consent.

## Required tests before the feature is merge-blocking

- Positive: valid conditions and Python candidates import, validate, and require acceptance.
- Negative: malformed, unsupported, oversized, duplicate, path-bearing, or executable control envelopes fail closed.
- Boundary: empty candidate, maximum supported size, Unicode, version edges, repeated imports, and stale candidate replacement.
- Regression: every bug fix or behavior change adds a test that would fail on the previous behavior.

## Not implemented yet

- Import transport and authentication model.
- Candidate-envelope schema and size limits.
- Preview/diff and acceptance UI.
- Audit persistence and retention policy.
- Provider-neutral examples and end-to-end tests.

Until implemented, product documentation must describe AI authoring as planned and must not instruct agents to call a specific model or endpoint.
