---
name: prototype
description: Build a throwaway prototype to answer a design question. Use when the user wants to sanity-check whether a state model or logic feels right, or explore what a UI should look like.
---

# Prototype

A prototype is temporary evidence for one design question. It is not an
implementation shortcut and never becomes a second product contract.

## Pick a branch

Identify which question is being answered — from the user's prompt, the surrounding code, or by asking if the user is around:

- **"Does this logic / state model feel right?"** → [LOGIC.md](LOGIC.md).
- **"What should this Qt page look like?"** → [UI.md](UI.md).

The two branches produce very different artifacts — getting this wrong wastes the whole prototype. If the question is genuinely ambiguous and the user isn't reachable, default to whichever branch better matches the surrounding code (a backend module → logic; a page or component → UI) and state the assumption at the top of the prototype.

## Repository constraints

1. Read the owning canonical spec and current implementation first.
2. Work in an isolated throwaway branch, worktree, or temporary directory. Do
   not mix prototype artifacts into a production or cleanup commit.
3. Use the checked-in C++/Qt/Python toolchain. Do not add a package, framework,
   production navigation seam, or task runner solely for the prototype.
4. Provide one command that was actually run. Use synthetic or copied test data;
   never mutate `StockData/Extracted` or another authoritative input.
5. Make the state and alternatives visible enough for the user to judge the
   question. Skip polish unrelated to that decision.
6. Record the question, observed result, and accepted/rejected conclusion in the
   issue or PR. Remove the prototype afterward. Promote only explicitly approved
   behavior through the normal spec, test, review, and implementation workflow.
