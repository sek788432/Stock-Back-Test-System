# Incremental Implement Skill Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `$implement` execute an entire plan as dependency-ordered, CI-green vertical slices instead of accumulating all features before verification.

**Architecture:** Expand the existing explicit-only project skill with a gate-aware slice loop that derives requirements from repository authority, applies test-first implementation, and blocks later slices until the current slice passes its applicable checks. Validate the behavioral contract with before-and-after pressure scenarios, then validate the skill package and documentation mechanically.

**Tech Stack:** Markdown Agent Skills, YAML UI metadata, Codex skill validator, Git, GitHub

**Spec:** `Docs/Plans/ImplementSkillDesign.md`

## Global Constraints

- Repository governance and the relevant specifications override skill guidance.
- Never disable, bypass, defer, or weaken an implemented merge gate.
- Distinguish implemented merge-blocking, implemented local, planned/not merge-blocking, and external/unverified checks.
- Continue through the complete plan, but never begin a later slice while the current slice is red.
- Preserve unrelated and user-owned changes.
- Keep `$implement` explicit-only.
- Every C++-bearing slice loads and applies all five project `cpp-*` skills;
  non-applicable concerns require a concrete reason.
- The final review may focus on intent and design only after mechanical quality evidence is complete; the mandatory review step remains in place.

---

### Task 1: Capture Baseline Failure Modes

**Files:**
- Read: `.agents/skills/implement/SKILL.md`
- Create: `/tmp/implement-skill-baseline-*.md` (temporary evidence only)

**Interfaces:**
- Consumes: current `$implement` instructions and three pressure scenarios
- Produces: verbatim choices and rationalizations that the revised skill must address

- [x] **Step 1: Run three independent scenarios without supplying the proposed skill**

Use fresh agents for: an oversized multi-feature plan under deadline pressure; a partially implemented slice with failing focused tests under sunk-cost pressure; and a green focused test whose required full project gate fails under authority pressure.

- [x] **Step 2: Record observable baseline failures**

Capture whether each agent batches unrelated features, advances while red, substitutes focused checks for full gates, weakens a gate, or mislabels planned CI as enforced. Preserve its exact rationalization in temporary evidence.

- [x] **Step 3: Convert failures into skill requirements**

Map every observed wrong behavior to a positive workflow contract or, for deliberate shortcuts under pressure, an explicit counter and red flag.

### Task 2: Implement the Gate-Aware Slice Loop

**Files:**
- Modify: `.agents/skills/implement/SKILL.md`
- Modify: `.agents/skills/implement/agents/openai.yaml`
- Modify: `.agents/skills/README.md`

**Interfaces:**
- Consumes: a user-approved plan or tickets, repository authority, and checked-in CI/local commands
- Produces: completed dependency-ordered slices with applicable quality evidence and a final intent/design review

- [x] **Step 1: Rewrite the skill discovery metadata and overview**

Keep explicit invocation and describe the trigger as executing an approved plan or ticket set. State the core invariant: one green vertical slice at a time.

- [x] **Step 2: Define the preflight and slicing contract**

Require reading the plan, governance, relevant specs/ADRs/skills, Definition of Done, CI workflow, and verified local commands. Require a dependency-ordered slice ledger whose entries identify behavior, acceptance evidence, affected scope, and applicable gates.

- [x] **Step 3: Define the per-slice execution loop**

Require test-first work at a pre-agreed seam; all five project `cpp-*` skills
for C++ changes; focused tests and formatting during iteration; broader affected
checks before accepting the slice; and no work on later slices until the current
slice is green.

- [x] **Step 4: Define checkpoints, failure handling, and final verification**

Require review and a cohesive commit for each green slice when commits are authorized. On failure, diagnose only the current slice, narrow it without dropping required behavior when possible, and stop on a genuine blocker. At completion, run every applicable full project gate on the exact submitted commit and then perform the mandatory review with mechanical evidence already resolved.

- [x] **Step 5: Add quick reference, shortcut counters, and common mistakes**

Address the exact baseline rationalizations. Make clear that a focused pass is not a full-gate pass, planned checks are not enforced checks, code review remains mandatory, and a large plan is not permission for a large red change.

- [x] **Step 6: Update UI metadata and the project skill catalog**

Use a 25–64 character short description and a default prompt that explicitly invokes `$implement`. Add an explicit-workflow catalog entry without duplicating the full skill.

- [x] **Step 7: Review the documentation diff before testing**

Check frontmatter, links, terminology, explicit-only policy, repository status labels, and the absence of unrelated edits.

### Task 3: Verify Behavior and Package Quality

**Files:**
- Read: `.agents/skills/implement/SKILL.md`
- Read: `.agents/skills/implement/agents/openai.yaml`
- Read: `.agents/skills/README.md`
- Create: `/tmp/implement-skill-green-*.md` (temporary evidence only)

**Interfaces:**
- Consumes: the revised `$implement` skill and the Task 1 scenarios
- Produces: behavioral evidence that the skill keeps every slice within the applicable CI contract

- [x] **Step 1: Re-run the three pressure scenarios with the revised skill**

Supply the complete revised skill to fresh agents and require an explicit action choice. Verify that no agent begins a later slice while the current one is red or substitutes a narrow check for an applicable full gate.

- [x] **Step 2: Close observed loopholes and re-test**

If an agent finds a new shortcut, add the smallest explicit counter, update the rationalization table or red flags, and re-run that scenario until it complies.

- [x] **Step 3: Validate the skill package and YAML**

```bash
ruby -e 'require "yaml"; skill = File.read(".agents/skills/implement/SKILL.md"); data = YAML.safe_load(skill.split(/^---\s*$/, 3)[1]); abort unless data["name"] == "implement" && data["disable-model-invocation"] == true; puts "SKILL.md: valid"'
ruby -e 'require "yaml"; data = YAML.load_file(".agents/skills/implement/agents/openai.yaml"); abort unless data.dig("policy", "allow_implicit_invocation") == false; puts "openai.yaml: valid"'
```

- [x] **Step 4: Verify documentation and repository hygiene**

Run placeholder/link checks, inspect `git diff --check`, review the complete branch diff hunk by hunk, and confirm unrelated untracked files remain untouched. Code build, runtime tests, and C++ quality gates are `N/A — documentation-only skill change`.

- [x] **Step 5: Commit the verified skill and review-driven refinements**

```bash
git add .agents/skills/implement/SKILL.md .agents/skills/implement/agents/openai.yaml .agents/skills/README.md Docs/Plans/ImplementSkillImplementationPlan.md
git commit -m "docs(skills): harden incremental implementation gates"
```

### Task 4: Publish the Pull Request

**Files:**
- Read: `.github/PULL_REQUEST_TEMPLATE.md`
- Read: branch diff and commit history

**Interfaces:**
- Consumes: verified branch commits and the repository PR template
- Produces: a pushed branch and pull request targeting `main`

- [ ] **Step 1: Push the branch**

```bash
git push -u origin codex/implement-ci-slices
```

- [ ] **Step 2: Create the pull request**

Use the repository template without blank sections, mark build/runtime/code-test items `N/A — documentation-only skill change`, include behavioral pressure-test evidence, and identify intent/design as the requested reviewer focus after mechanical checks.

- [ ] **Step 3: Report the PR and final evidence**

Provide the PR link, commits, changed files, validation commands, behavioral outcomes, and any remaining external/unverified checks without claiming they passed.
