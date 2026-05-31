# Release Process

How a version of `stockBacktester` ships from `main` to a GitHub Release the Launcher can install (Specs/09).

Small team, no dedicated release manager — the role rotates. The current rotation is in the team's pinned issue (`role: release-manager`).

---

## Release cadence

- **Patch (`x.y.Z`)**: as needed, often. Bug fixes, no new features.
- **Minor (`x.Y.0`)**: monthly-ish. New features, no breaking changes.
- **Major (`X.0.0`)**: when the plugin ABI or Lua API breaks. Rare; coordinate with team.
- **Pre-release (`x.y.z-beta.N`)**: to test something risky with a subset of users via the Launcher's "channels" filter.

Versioning is **semver** (Specs/09 §6). The plugin ABI and Lua API have **independent** version numbers; their majors don't bump in lockstep with the app's.

---

## Pre-release checklist (the day before)

The release manager:

- [ ] All issues in the milestone are closed or moved to the next.
- [ ] [`Governance/CHANGELOG.md`](Governance/CHANGELOG.md) has a section for this version with **Added / Changed / Fixed / Removed**.
- [ ] No PRs in flight that should make this release. (If there are, decide: hold the release or punt the PRs.)
- [ ] Determinism fixture is current; CI green on `main`.
- [ ] Plugin SDK sample plugin builds on all three OSes.
- [ ] If breaking the plugin ABI, `BTE_PLUGIN_ABI_MAJOR` was bumped in the PR that introduced the break, with an ADR.
- [ ] If breaking the Lua API, `bte::apiVersion` was bumped, with an ADR.
- [ ] Codesigning credentials available if configured (optional; see ADR 0005). Linux GPG key available.

---

## Cutting the release

1. **Open the release PR** from `main`:

   ```bash
   git switch -c release/v0.3.0
   # bump version in CMakeLists.txt project(... VERSION x.y.z)
   # finalize Docs/Governance/CHANGELOG.md "Unreleased" -> "[0.3.0] - YYYY-MM-DD"
   git commit -m "chore(release): v0.3.0"
   gh pr create --title "chore(release): v0.3.0" --base main
   ```

   The release PR is a normal PR — it goes through the same gates. CI is the safety net.

2. **Merge** when green.

3. **Tag** on `main` after merge:

   ```bash
   git switch main && git pull
   git tag -s v0.3.0 -m "Release v0.3.0"
   git push origin v0.3.0
   ```

4. **CI release workflow runs automatically** on the tag. It:
   - Re-runs the full matrix.
   - Builds packages on each OS (Specs/09 §3).
   - Codesigns + notarizes (macOS) and signs (Windows) if credentials are configured in secrets; skipped otherwise.
   - Generates `release-manifest.json`.
   - Creates a draft GitHub Release with body sourced from `Docs/Governance/CHANGELOG.md`.
   - Uploads all artifacts (app + SDK).

5. **Verify** the draft release:
   - Download the macOS dmg, Windows msi, Linux AppImage on a clean machine and launch them.
   - Verify the Launcher can install this version (point Launcher at the draft tag).
   - Check that `release-manifest.json` lists every expected asset with correct sha256.
   - Verify Help → About shows the right version.

6. **Publish** the release (un-draft) once verified. The Launcher's "Available" list will pick it up within 10 minutes.

7. **Announce** in sync chat: version, link to release, headline of the changelog.

---

## If something goes wrong

### Bad release already published

1. Mark the release as **pre-release** in the GitHub UI (so the Launcher hides it from default users).
2. Decide: hotfix-and-republish, or yank entirely.

### Hotfix flow

```bash
git switch -c hotfix/v0.3.1 v0.3.0
# fix the bug, add a regression test
git commit -m "fix(engine): correct fill price for stop orders gapped through"
gh pr create --base main
# merge → tag v0.3.1 → release workflow runs → publish
```

Hotfix PRs go through the same review and CI gates. No "emergency override".

### Yank a release entirely

- In the GitHub UI: delete the release (this leaves the tag).
- Update `release-manifest.json` for the **next** release with `supersedes: ["0.3.0"]` — the Launcher uses this to mark the bad version as do-not-install.
- Post a sync-chat note explaining what happened.

---

## Release-time invariants (cannot be true and shipping)

If any of these is true, **do not publish** the release. Hold and fix.

- [ ] Determinism fixture changed without a corresponding ADR.
- [ ] Linux GPG signature is missing (macOS/Windows signing is optional per ADR 0005).
- [ ] Plugin SDK release is missing for any platform (Launcher users can build but plugin authors can't).
- [ ] `Docs/Governance/CHANGELOG.md` doesn't mention something user-visible that landed in this version.
- [ ] No one on the team has launched the new version on real hardware (not just CI).

---

## Roles for the release

| Role | Who | Responsibility |
|---|---|---|
| **Release manager** | Rotating | Drives the checklist; opens the release PR; tags; verifies; publishes. |
| **Engine reviewer** | Engine CODEOWNER | Reviews the determinism diff if any. |
| **Build reviewer** | Build CODEOWNER | Verifies CI artifacts on at least one OS. |
| **Communicator** | Anyone available | Posts the announcement. |

For 4–8 people, one person can hold multiple roles. The release manager is the only one explicitly named for the release.

---

## After the release

- Open a "milestone retro" issue (small): what went well, what didn't, what to change for next time.
- Move untriaged issues into the next milestone or backlog.
- Update the team's pinned `role: release-manager` issue to the next person.
- If anything in this doc was wrong, fix it in a PR. The doc is the spec for next time.

---

## Why we keep this small

A 4–8 person team can't sustain a release theater. The CI does the heavy lifting (Specs/10 gates, Specs/09 codesigning). This doc is just the human checklist around the automation. If a step here feels redundant with CI, it probably is — propose removing it in a PR.
