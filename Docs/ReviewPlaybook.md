# Review Playbook

How to review a PR in this repo. Same checklist for humans and AI reviewers.

A good review is two passes: **what did the author intend, and does the diff achieve it without breaking other things?** The goals below help you stay on that track instead of getting lost in stylistic quibbles (those are clang-tidy's job).

---

## Author etiquette (before review starts)

If you're the author, save the reviewer time:

1. **Self-review first** using this same playbook on your own diff.
2. **Fill the PR template completely.** No blank fields.
3. **Confirm [`DefinitionOfDone.md`](DefinitionOfDone.md)** — copy the relevant checklist into the PR body.
4. **Pre-emptively answer the obvious questions.** "Why this approach over X?" → answer in the description.
5. **Keep it small.** ≤ 400 lines of diff is the sweet spot. ≥ 1000 lines and you'll get a perfunctory review.
6. **One concern per PR.** Found a bug while in the area? File an issue, don't smuggle a fix.

If a reviewer asks for clarification, treat it as a doc bug — the description should have answered it.

---

## Review SLAs

- **First response: within 1 business day.** "I'll get to it tomorrow" counts. Silence does not.
- **Review pass: within 2 business days** of the PR being ready (template filled, CI green).
- **Author response: within 1 business day** of new review comments.
- **Stale PR**: 5 business days with no author response → reviewer asks the author to close or pick up.

Hybrid team norms: don't wait for the weekly sync to comment. The PR is the forum.

---

## Reviewer pass 1: scan

Goal: 5–10 minutes. Decide whether to dive in or send back for fixes.

1. **PR template filled?** If sections are blank, request the author fills them out before you review.
2. **CI green?** If red, why? "Pre-existing flaky test" gets a fresh CI run; new failure gets an "address before review" comment.
3. **Scope clear?** One concern? Title accurate?
4. **Does the change do what it says it does?** Skim the diff. Is the PR title a fair summary?
5. **Are tests included?** New / changed public symbols → unit tests. If absent, request them before deeper review.

If pass 1 fails, request fixes and stop. Don't review code that isn't ready.

---

## Reviewer pass 2: deep read

Goal: 20–40 minutes for a typical PR. Read the diff hunk-by-hunk with the file open in context (not just the GitHub split view).

### Correctness

- [ ] The logic does what the code says. Walk through edge cases mentally: empty inputs, one-element inputs, max-size inputs, concurrent inputs.
- [ ] Off-by-ones, NaN, divide-by-zero, integer overflow handled where they can occur.
- [ ] Error paths handled — no swallowed `Result<T>::error()`, no silent failure.
- [ ] No look-ahead bias in trading code (Specs/07 §2.1 — fills happen on next bar's open, not current bar's close, by default).

### Tests

- [ ] Every new public symbol has a test that names it.
- [ ] Test names describe the **invariant** being checked, not the implementation (`testRsiWilderSmoothingMatchesReference` not `testRsi`).
- [ ] No anti-cheat patterns (Specs/10 §5).
- [ ] Mentally apply mutation: "if I changed `+` to `-` here, would a test catch it?"
- [ ] Fixtures are minimal — small enough to read, big enough to be interesting.

### Design

- [ ] Single responsibility per type / function.
- [ ] Public API minimal — would I shrink it before adding to it?
- [ ] No premature abstractions for "future flexibility" (YAGNI).
- [ ] Module dependency graph respected (Specs/01 §1).
- [ ] Right design pattern used — and used because the situation calls for it, not for its own sake (`cpp-oop-design`).

### Threading and memory

- [ ] No raw `new` / `delete`, no raw `mutex.lock()`.
- [ ] All resources RAII.
- [ ] Cross-thread comms via immutable snapshots or queued signals.
- [ ] If TSan was warranted, did the author run it?

### Performance

- [ ] No new heap allocations in documented hot paths.
- [ ] No `std::map` where `std::unordered_map` is appropriate.
- [ ] No `std::function` or virtual call inserted in tight loops.
- [ ] If the PR claims a perf win, are there nanobench numbers?

### Style and skills compliance

- [ ] Naming matches `lowerCamelCase` / `UpperCamelCase` rules (`cpp-modern-style`).
- [ ] No banned C-style idioms.
- [ ] No `using namespace std;` at any scope.
- [ ] Comments explain why, not what.

### Tests as documentation

- [ ] Reading the tests, can you understand the behavior of the type without reading the implementation? (If yes, the tests are good documentation. If no, they probably under-specify the behavior.)

---

## Comment categories

We use a tiny prefix system so authors can triage your feedback at a glance:

| Prefix | Meaning | Author response |
|---|---|---|
| **blocking:** | Must fix before merge | Required |
| **issue:** | Real concern, fix or push back with reasoning | Required (fix or discussion) |
| **suggestion:** | Improvement worth considering | Optional, brief reply if not taking |
| **nit:** | Style / preference, take it or leave it | No response needed |
| **question:** | I don't understand X | Required, reply or update code |
| **praise:** | This is good, called out so the author knows | No response needed |

Example:

```
blocking: this allocation is in the engine bar loop
(Specs/07 §9 budget). Move the std::vector outside the
loop and reserve(), or pass in a scratch buffer.
```

Use **blocking** sparingly. If everything's blocking, nothing is.

---

## Approving

Approve when:

- All `blocking:` comments are resolved.
- CI is green.
- The DoD checklist is honestly ticked.
- You'd be comfortable having to debug this code at 2 AM.

Don't approve when:

- You don't understand a hunk and the author hasn't responded.
- Tests look weak and you wouldn't trust them to fail when the code is wrong.
- The design feels off and you'd want to talk it through in the next sync.

If you can't approve but don't want to block, request changes with an explanation. **Silent ghosting is the worst review behavior.**

---

## Common review traps to avoid

- **Bikeshedding style** clang-format already enforces. If clang-tidy didn't catch it and `cpp-modern-style` doesn't ban it, it's not a real issue.
- **Reviewing for "what I would have written"** instead of "is this a good solution to the problem". The author's solution might be better than yours.
- **Approving fast** because CI is green. CI catches a lot, but not design and intent.
- **Demanding rewrites** for small style preferences. Use `nit:` and move on.
- **Demanding refactors of unrelated code.** File an issue; don't expand the scope of the PR.

---

## When two reviewers disagree

If two reviewers leave conflicting `blocking:` comments, the author shouldn't have to mediate. Default protocol:

1. Reviewers thread it out in the PR until they converge.
2. If they can't, escalate to the file's CODEOWNER.
3. If the CODEOWNER is one of them, escalate to the repo lead.
4. Whatever the resolution, **write it down in an ADR** if the disagreement was about design (so future PRs don't re-litigate).

---

## When you're new to reviewing

For your first 5 reviews on this repo:

- Pair with a more experienced reviewer if possible.
- Write **suggestion:** and **nit:** liberally; let the experienced reviewer decide what's blocking.
- Read the relevant Spec section before reviewing; you'll catch more.
- Ask "why" generously. Authors should be able to defend their choices.

After 5 reviews you'll know the codebase well enough to mark `blocking:` confidently.
