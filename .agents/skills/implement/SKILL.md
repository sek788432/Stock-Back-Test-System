---
name: implement
description: "Execute an approved implementation plan or dependency-ordered ticket set in CI-green slices."
---

# Implement

Execute the complete plan as dependency-ordered, CI-green vertical slices.

**Core invariant:** do not begin a later slice while the current slice has a
known failing applicable check. A deferred failure is still a red slice.

## Preflight

1. Read the plan and source spec, canonical governance, relevant specs, active
   important decisions and project skills, Definition of Done, CI workflow, and
   verified local commands. Repository authority wins.
2. Preserve unrelated staged, unstaged, and untracked user work. Confirm the
   base revision and authorization for commits, pushes, or external changes.
3. Classify checks from checked-in evidence: **merge-blocking**,
   **implemented local**, **planned/not merge-blocking**, or
   **external/unverified**. Never invent or relabel a check.
4. Split remaining work into the smallest dependency-ordered vertical slices
   that deliver verifiable behavior. Record each slice's behavior, blockers,
   test seam, evidence, affected scope, fast checks, and acceptance gates.

If slices cannot remain green, use expand-migrate-contract or narrow them
without dropping required behavior. Ask before changing plan intent.

## Slice loop

**REQUIRED SUB-SKILL:** Use `tdd` for every behavior-changing or bug-fix
slice. For behavior-preserving refactors and build or tooling work, follow the
repository's impact-specific test contract and record why a new red behavior
test is not applicable.

For every C++-bearing slice, **REQUIRED SUB-SKILLS:** Use
`cpp-modern-style`, `cpp-oop-design`, `cpp-performance`,
`cpp-thread-safety`, and `cpp-static-analysis`. Apply their implementation and
verification rules before accepting the slice. Record `N/A — <reason>` for a
skill concern the slice does not affect; loading the skill is not optional.

For each unblocked slice:

1. Mark only that slice in progress. At the pre-agreed seam, make a behavior
   change or bug fix red before implementing it. For a behavior-preserving
   refactor, establish green characterization evidence first; for tooling, use
   the required positive, negative, and boundary tool tests. Add the minimal
   change, then make the applicable evidence green.
2. During iteration, run the narrowest authoritative tests, typechecking,
   formatting, and analysis that cover the edit.
3. Before accepting it, run every affected-scope check required by project
   instructions. A documented fast mode is interim evidence, never the complete
   gate. If only a full-project check can validate the slice, run it.
4. Review against its evidence and Definition of Done. If authorized, make a
   cohesive commit containing only the slice so the green checkpoint is
   recoverable.
5. Update the ledger, then start the next unblocked slice.

If a check fails, stay on the slice, diagnose it, fix production or test code,
and rerun it. Never hide, disable, defer, or weaken a gate. Stop only for a
genuine blocker requiring new authority or a material plan decision.

## Completion

On the exact submission commit, run every applicable checked-in full workflow
required before CI. For this repository, applicable C++ work includes
`./RunTest.sh` and `./RunQuality.sh --base <base-revision> --head HEAD`. Record
results and label unrun, unavailable, or external checks accurately.

**REQUIRED SUB-SKILL:** Use `review` against the fixed base and resolve every
blocking finding.

Mechanical quality evidence should already be complete, so reviewer attention
can concentrate on test intent, unautomated standards, specification fidelity,
and design. Comprehensive review remains mandatory.

Commit and push only when authorized, staging only files in scope. Report
completed slices, evidence, and planned or blocked work without claiming
completion early.

## Quick reference

| State | Action |
|---|---|
| Focused check fails | Fix the current slice; do not advance |
| Focused checks pass | Run applicable affected-scope checks |
| Slice touches C++ | Apply all five project `cpp-*` skills |
| Documented fast gate passes | Treat as interim evidence only |
| Required full gate fails | Keep the current slice red |
| All slices complete | Run full required gates on submitted commit, then review |

## Common shortcuts to reject

| Rationalization | Reality |
|---|---|
| "The failures are deferred, not waived" | Known failure means the current slice is red now. |
| "One stabilization pass is cheaper" | Later slices enlarge the failure surface and erase localization. |
| "Focused tests passed" | They do not replace broader checks required for the affected scope. |
| "The user said finish everything" | Persistence does not authorize building on a red checkpoint. |
| "Review can skip code quality" | Automation reduces mechanical review; it does not replace judgment. |

## Red flags

- Editing multiple independent slices at once
- Starting the next slice with a known failure
- Calling a fast or focused check the full gate
- Treating a planned check as implemented, or the reverse
- Saving tests, analysis, formatting, or coverage repair for the end
- Asking review to discover failures that an authoritative command can detect
