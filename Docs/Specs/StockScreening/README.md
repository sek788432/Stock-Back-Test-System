# Stock Screening and Valuation Proposals

> **Status:** Non-normative design archive and UI prototype. Nothing in this
> folder is implemented or an approved database/runtime contract. Canonical
> specs `03`–`07`, `11`, and `12` take precedence.

## Useful contents

- [`ScreenerV2.html`](ScreenerV2.html): local interactive UI prototype.
- [`ScreenerUiOverview.md`](ScreenerUiOverview.md): prototype walkthrough.
- [`SpecAScreenerEngine.md`](SpecAScreenerEngine.md): proposed filtering concepts.
- [`SpecBValuationEngine.md`](SpecBValuationEngine.md): proposed valuation models; outside current V1 scope.
- [`SpecCDatabase.md`](SpecCDatabase.md): historical database proposal; it does not override the managed-snapshot design.
- [`SpecDNlPythonRuntime.md`](SpecDNlPythonRuntime.md): current constraints for any future Python/AI screening work.
- [`ApiDataRequirements.md`](ApiDataRequirements.md): candidate fundamental-data inventory, pending source and redistribution validation.

Images and `FakeFundamentals.csv` are prototype fixtures only.

## Promotion rule

Before implementation, move the accepted behavior into the owning canonical
spec and, when the threshold applies, the living important-decisions document;
reconcile it with the managed snapshot and C++ engine, and add
positive, negative, boundary, and regression tests. Do not copy database
schemas, C++ interfaces, Python sandbox claims, AI-provider calls, or numeric
defaults from these proposals without that review.
