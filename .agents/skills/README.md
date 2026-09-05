# Project Agent Skills

This is the repository's only project-skill directory. Agent hosts that support
`.agents/skills` should discover these folders automatically. Every other AI
agent must consult the relevant `SKILL.md` through the mandatory repository
playbook in
[`Docs/Governance/AGENTS.md`](../../Docs/Governance/AGENTS.md).

Repository instructions always take precedence over skill guidance.

## Authority model

| Layer | Canonical location | Purpose |
|---|---|---|
| Hard invariants | [`Docs/Governance/AGENTS.md` §2](../../Docs/Governance/AGENTS.md) | Always-read, non-negotiable repository rules |
| Product and architecture contracts | [`Docs/Specs/`](../../Docs/Specs/README.md) and [`ImportantDecisions.md`](../../Docs/Decisions/ImportantDecisions.md) | Defines required behavior and active important decisions |
| Task-specific workflows | This `.agents/skills/` directory | Detailed guidance loaded only when a skill matches the task |
| Mechanical enforcement (currently partial) | [`.github/workflows/`](../../.github/workflows/) and [`Tools/`](../../Tools/) | Detects the covered subset without relying on agent memory |

Do not make a non-negotiable rule live only in a skill. Put its concise,
authoritative form in `Docs/Governance/AGENTS.md`, keep implementation detail in
the relevant skill, and add CI coverage when the rule is mechanically
checkable. Until a gate exists, governance remains authoritative and human
review must cover the gap.

## Repository-specific C++ skills

| Skill | Activates when | Enforces |
|---|---|---|
| [`cpp-modern-style`](cpp-modern-style/SKILL.md) | Editing or refactoring C++ and naming files or symbols | Modern C++20, project naming, and bans on C-style resource management and APIs |
| [`cpp-thread-safety`](cpp-thread-safety/SKILL.md) | Working with threads, callbacks, mutexes, atomics, or cross-thread Qt calls | RAII, immutable or singly owned state, scoped locks, and `std::jthread` cancellation |
| [`cpp-performance`](cpp-performance/SKILL.md) | Changing hot paths, indicators, engine loops, or replay ticks | Complexity, allocation, copying, dispatch, and benchmark discipline |
| [`cpp-oop-design`](cpp-oop-design/SKILL.md) | Designing modules, classes, interfaces, seams, or refactors | SOLID, composition, narrow interfaces, and appropriate design patterns |
| [`cpp-static-analysis`](cpp-static-analysis/SKILL.md) | Running or interpreting formatting, lint, analyzers, and sanitizers | The repository's complete static-analysis and sanitizer workflow |

These five skills contain the repository's detailed C++ workflows and examples.
The canonical hard requirements live in `Docs/Governance/AGENTS.md` so they
apply even when an agent host does not support skill discovery.

## Comprehensive review

| Skill | Activates when | Enforces |
|---|---|---|
| [`review`](review/SKILL.md) | Reviewing a branch, pull request, handoff, or uncommitted change set | Complete code, documentation, test-intent, semantic-coverage, and verified-check review across the branch delta and working tree |

## Plan execution

| Skill | Activates when | Enforces |
|---|---|---|
| [`implement`](implement/SKILL.md) | Explicitly executing an approved implementation plan or dependency-ordered ticket set | Dependency-ordered vertical slices that stay within applicable CI and Definition of Done gates, with all five project `cpp-*` skills applied to C++ slices |

## How the C++ skills layer with specs

| Question | Primary source |
|---|---|
| How should this C++ loop or API be written? | `cpp-modern-style` |
| Where does this module belong? | [`Docs/Specs/Architecture.md`](../../Docs/Specs/Architecture.md) |
| How should this strategy type plug in? | [`Docs/Specs/StrategyAuthoring.md`](../../Docs/Specs/StrategyAuthoring.md) and `cpp-oop-design` |
| What are the CI gates? | [`Docs/Specs/CiDevFlow.md`](../../Docs/Specs/CiDevFlow.md) |
| Is this code thread-safe? | `cpp-thread-safety` |
| Does this allocation matter? | `cpp-performance` |
| What does this analyzer warning mean? | `cpp-static-analysis` |

## Retained shared workflows

| Skill | Purpose |
|---|---|
| [`code-review`](code-review/SKILL.md) | Review a diff independently against repository standards and its originating specification |
| [`codebase-design`](codebase-design/SKILL.md) | Design deeper module interfaces and seams |
| [`diagnosing-bugs`](diagnosing-bugs/SKILL.md) | Build a reproducible diagnosis loop |
| [`domain-modeling`](domain-modeling/SKILL.md) | Maintain domain language and important decisions |
| [`grill-me`](grill-me/SKILL.md) | Start an explicit maintainer grilling session |
| [`grill-with-docs`](grill-with-docs/SKILL.md) | Grill a decision while maintaining domain documentation |
| [`grilling`](grilling/SKILL.md) | Resolve material ambiguity with the maintainer |
| [`improve-codebase-architecture`](improve-codebase-architecture/SKILL.md) | Find and visualize deep-module architecture opportunities |
| [`prototype`](prototype/SKILL.md) | Answer a bounded logic or Qt UI design question |
| [`research`](research/SKILL.md) | Capture primary-source technical research |
| [`resolving-merge-conflicts`](resolving-merge-conflicts/SKILL.md) | Resolve an active merge or rebase safely |
| [`tdd`](tdd/SKILL.md) | Develop behavior through a red-green test loop |
| [`teach`](teach/SKILL.md) | Build a persistent, evidence-based teaching workspace |
| [`to-spec`](to-spec/SKILL.md) | Turn an established conversation into an issue-tracker specification |
| [`writing-great-skills`](writing-great-skills/SKILL.md) | Reference the vocabulary and principles for predictable skills |

## Vendored skill provenance

- Source: [mattpocock/skills](https://github.com/mattpocock/skills)
- Upstream release: `1.1.0`
- Pinned commit: `2ab958093e83e0ec752e6c1c5932da465bf23e0c`
- License: MIT; see [`LICENSE.mattpocock-skills`](LICENSE.mattpocock-skills)
- Retained and adapted: the shared workflows listed above that remain relevant
  to this repository. Their local guidance is derived from the pinned snapshot
  and reconciled with current repository contracts.
- Removed: optional, overlapping, or repository-inapplicable packages. Git
  history and the pinned upstream commit preserve their source when needed.

These files are locally adapted from a vendored snapshot. They do not update
automatically. Review upstream changes and commit a new pin plus any deliberate
local reconciliation.

## Invocation and maintenance

Each `SKILL.md` description controls automatic activation. Explicit-only skills
must be named by the user. Skills should remain focused and under 500 lines;
move detailed material into directly linked sibling references when needed.

Add new project skills only under `.agents/skills/<skill-name>/`. Use
`$skill-creator` to create or revise a skill, validate every changed skill, and
update this catalog when the project-level set changes.
