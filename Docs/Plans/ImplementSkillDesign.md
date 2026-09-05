# Implement Skill Design

## Purpose

Revise the existing explicit-only `$implement` project skill so an agent can
execute a complete plan without accumulating an oversized, unverified change.
The agent should preserve forward progress by keeping every intermediate slice
compatible with the repository's applicable CI contract.

## Selected approach

Use a gate-aware vertical-slice loop inside the existing
`.agents/skills/implement` skill. Before editing code, the agent reads the plan,
repository governance, relevant specifications, and the checked-in CI and local
verification entry points. It then converts the remaining work into the
smallest dependency-ordered slices that deliver verifiable behavior.

For each slice, the agent:

1. defines the slice's acceptance evidence and applicable CI checks;
2. applies the repository's impact-specific test contract at the planned seam,
   using red-first TDD for behavior changes and bug fixes;
3. applies all five project `cpp-*` skills when the slice touches C++;
4. runs focused checks during development;
5. runs the broader applicable checks before accepting the slice;
6. reviews and checkpoints the green slice before starting the next one.

The loop continues until the plan is complete. A failed check blocks later
slices: the agent diagnoses and fixes the current slice, narrows it when that
can preserve the plan's intent, or reports a genuine blocker. It must not hide,
disable, defer, or relabel a failing required check.

## CI scope model

The skill does not freeze today's command list into reusable instructions.
Instead, it derives the current gate contract from authoritative repository
files and distinguishes:

- implemented merge-blocking checks;
- implemented local checks;
- planned checks that are not yet enforced;
- external settings that cannot be verified from the checkout.

Focused tests and checks provide fast feedback inside a slice. The repository's
required full local workflow still runs at the points mandated by governance,
including on the exact submitted commit when applicable. A focused green check
never substitutes for an applicable full gate.

For C++ slices, the gate contract includes the implementation and verification
rules from `cpp-modern-style`, `cpp-oop-design`, `cpp-performance`,
`cpp-thread-safety`, and `cpp-static-analysis`. A concern that is not affected
is recorded as not applicable with a concrete reason rather than silently
skipped.

## Files and discovery

- Expand `.agents/skills/implement/SKILL.md` with the incremental execution
  contract, stop conditions, quick reference, and pressure-tested counters to
  common shortcuts.
- Update `.agents/skills/implement/agents/openai.yaml` so its interface text
  accurately describes plan execution.
- Add `$implement` to `.agents/skills/README.md` as an explicit project
  workflow.

The skill remains explicit-only because implementation and committing are
material actions that should begin only when the user invokes the workflow.

## Validation

Skill behavior is tested with baseline and post-change pressure scenarios. The
scenarios combine a large multi-feature plan, time pressure, sunk cost, and a
failing gate. Success requires the agent to keep work in dependency-ordered
vertical slices, refuse to begin later slices while the current one is red,
and report CI status without confusing planned checks with enforced gates.

After behavioral testing, validate the standard skill fields plus the
repository's explicit-only policy, parse the UI metadata, review the
complete diff, verify links and catalog consistency, and run the applicable
documentation checks from the Definition of Done.

## Scope boundaries

This change does not modify CI, weaken a merge gate, implement product
features, or require an important-decision entry. It changes only the project
workflow used to carry out already-authorized plans.
