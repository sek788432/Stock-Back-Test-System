# Important Decision Entry Format

Add or update an entry in the repository's single living important-decisions
document. Do not create a numbered, append-only, superseded, or deprecated
decision file.

## Template

```md
## {Decision title}

- **Current decision:** {The constraint that applies now.}
- **Why:** {Why this choice was approved.}
- **Important rejected alternatives:** {Only alternatives whose trade-offs
  remain useful, and why each was rejected.}
- **Consequences:** {Costs, limits, and follow-on obligations.}
- **Owning specification:** {Link to the existing canonical specification.}
```

## Maintenance

- Reach shared understanding with the maintainer before recording a material
  choice.
- Keep detailed behavior in the owning specification; the decision entry owns
  rationale and trade-offs.
- Update the entry in place when the current decision changes.
- Remove the entry when it no longer constrains the project.
- Use Git history for obsolete chronology.

## When to offer an entry

All three must be true:

1. **Hard to reverse** — the choice materially constrains future work and would
   have a meaningful cost to change.
2. **Surprising without context** — a future contributor could not reliably
   infer the rationale from the owning spec and implementation.
3. **Real trade-off** — more than one reasonable answer existed and the
   rejected alternatives remain important to understand.

An implemented merge-gate change always requires an entry under repository
governance. For another listed category that does not pass all three tests,
explain why an entry is unnecessary in the PR.
