# 0006 — Full-tree project standards gate

- **Status**: Accepted
- **Date**: 2026-08-01
- **Deciders**: repository maintainer
- **Supersedes**: —
- **Superseded by**: —

## Context

The project standards check originally inspected only lines added by a pull
request. That prevented new violations but allowed existing violations to remain
on `main`. It also meant an unrelated compliant pull request could report green
while the checked-out repository as a whole did not meet the documented rules.

The baseline audit in ISSUE-39 found 148 reports: 98 genuine trailing-whitespace
violations, 45 bare allocations, and 5 false positives where Markdown explained
the task-comment policy. With the genuine debt removed, CI can enforce one
consistent standard across the repository.

## Decision

Run `tools/checkProjectStandards.py --full-tree` as a required GitHub Actions job
on every pull request and every push to `main`. The audit reads every tracked
UTF-8 text file from the exact Git commit under test. Binary files are ignored.

The full-tree audit enforces formatting, banned C++ idioms, test-quality rules,
and registration of every `UnitTest_*.cpp` and `UnitTest_*.py` file. The
base-versus-head comparison remains in place for the new-top-level-module rule:
every new module must add a registered unit test.

`TODO` and `FIXME` enforcement applies to source and configuration comments, not
to Markdown prose that documents the rule. A task comment is accepted only when
the same comment references `ISSUE-NNN`.

This decision does not change ADR 0004's diff-coverage policy. Coverage measures
changed executable behavior; this gate checks repository content that can be
made fully compliant and kept that way.

## Consequences

**Positive:**

- A green standards check certifies the complete checked-out revision.
- Legacy violations cannot be hidden behind a clean pull-request diff.
- Tool regressions and manually committed test files are caught before merge.
- Policy documentation can describe forbidden markers without false positives.

**Negative:**

- Every pull request scans the full tracked tree instead of only its diff.
- Tightening a rule requires cleaning the existing baseline in the same change.
- The line-oriented checker cannot replace compiler- or AST-based analysis.

**Mitigations:**

- The current repository is small enough for the scan to complete in seconds.
- Every checker rule and accepted exception has a unit regression test.
- Compiler warnings and the complete CTest suite remain independent merge gates.

## Alternatives considered

1. **Keep diff-only enforcement.** Rejected because `main` could remain
   non-compliant indefinitely and a green check would describe only the patch.
2. **Maintain a baseline suppression file.** Rejected because all genuine
   findings were mechanically or safely correctable; a suppression file would
   create permanent debt and a second policy surface.
3. **Apply task-marker rules to all prose.** Rejected because documentation must
   be able to explain the policy and show invalid examples.
4. **Use a whole-project coverage threshold too.** Rejected by ADR 0004; static
   content compliance and behavioral coverage have different debt profiles.

## References

- ISSUE-39
- [`../Specs/10_CI_Dev_Flow.md`](../Specs/10_CI_Dev_Flow.md)
- [`../../tools/checkProjectStandards.py`](../../tools/checkProjectStandards.py)
- [ADR 0004](0004-anti-cheat-ci-gate-with-mutation-testing.md)
