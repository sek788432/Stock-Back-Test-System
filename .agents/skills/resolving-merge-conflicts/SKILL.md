---
name: resolving-merge-conflicts
description: "Use when you need to resolve an in-progress git merge/rebase conflict."
---

1. **See the current state.** Inspect `git status`, the merge/rebase operation,
   the conflicting files, and unrelated user-owned changes before editing.

2. **Find the primary sources** for each conflict. Understand deeply why each change was made, and what the original intent was. Read the commit messages, check the PRs, check original issues/tickets.

3. **Resolve only when the intents are compatible.** Preserve both when
   possible and do not invent behavior. If the correct result requires a
   material product choice, destructive history change, or permission outside
   the task, stop and ask. Aborting is valid when the operation targets the
   wrong branch or the user requests it; do not abort merely because a conflict
   is difficult.

4. Run the applicable checked-in commands from `Docs/DefinitionOfDone.md` and
   `Docs/Specs/CiDevFlow.md`. Do not invent a generic typecheck/format command.

5. **Finish only the authorized operation.** Stage the resolved paths, verify
   that no conflict marker remains, and continue the merge/rebase. Never stage
   unrelated working-tree changes.
