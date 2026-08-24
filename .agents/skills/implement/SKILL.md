---
name: implement
description: "Implement a piece of work based on a spec or set of tickets."
---

Implement the work described by the user in the spec or tickets.

Read the canonical governance playbook, applicable module specs, relevant project skills, and Definition of Done before editing. Use the repository `tdd` skill where practical, at agreed behavioral seams.

Protect unrelated staged, unstaged, and untracked user work. Run the narrowest relevant tests during development. Before handoff, run every applicable checked-in verification command documented by the repository; for this repository that includes `./RunTest.sh`, applicable `./RunQuality.sh` checks, and the Definition of Done.

Once implementation and verification are complete, use the repository's comprehensive `review` skill. Resolve every blocking finding and rerun affected checks.

Commit only when the user requested or otherwise authorized a commit, and stage only the files in scope.
