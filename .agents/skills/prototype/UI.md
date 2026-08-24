# UI Prototype

Generate several structurally different Qt Widgets variations and let the user
switch between them in one temporary harness. Remove the harness after the
question is answered.

If the question is about logic/state rather than what something looks like — wrong branch. Use [LOGIC.md](LOGIC.md).

## When this is the right shape

- "What should this page look like?"
- "I want to see a few options for this dashboard before committing."
- "Try a different layout for the settings screen."
- Any time the user would otherwise spend a day picking between three vague mockups in their head.

## Two shapes — prefer the existing page

A prototype is easier to judge beside the application's real tabs, controls,
and density. Prefer a temporary variant host inside the closest existing page.
Use a standalone throwaway Qt widget only when no current page is a meaningful
host.

### Existing page (preferred)

Keep the current data/view-model boundary and replace only the temporary widget
subtree being compared. Do not add production navigation for a prototype.

### Standalone widget (last resort)

Use a temporary `QWidget` host launched by one verified local command. Keep it
outside the production tab registry and submitted build wiring unless the user
later approves an implementation.

## Process

### 1. State the question and pick N

Default to **3 variants**. More than 5 stops being radically different and starts being noise — cap there.

Write down the plan in one line, in the prototype's location or a top-of-file comment:

> "Three variants of the Backtest condition editor, switchable in one temporary
> Qt host using the existing page dimensions."

This works whether the user is here to push back or not.

### 2. Generate radically different variants

Draft each variant. Hold each one to:

- The page's purpose and the data it has access to.
- The existing Qt Widgets style and project-owned presentation conventions.
- A clear local widget name such as `VariantA`, `VariantB`, or `VariantC`.

Variants must be **structurally different** — different layout, different information hierarchy, different primary affordance, not just different colours. Three slightly-tweaked card grids isn't a UI prototype, it's wallpaper. If two drafts come out too similar, redo one with explicit "do not use a card grid" guidance.

### 3. Wire them together

Create one temporary Qt selector that swaps the displayed widget through a
`QStackedWidget`. Keep existing data acquisition above the variant boundary so
each layout sees equivalent representative state.

### 4. Build the temporary selector

Use a clearly labeled `QComboBox`, buttons, or temporary tabs outside the layout
being evaluated. Show the current variant name and keep ordinary text-entry key
handling intact. The selector is throwaway scaffolding and must not be hidden
inside a production build.

### 5. Hand it over

Give the user the verified launch command and the exact comparison to make.

### 6. Capture the answer and clean up

Record the selected variant or combination and why. Remove every alternative and
all selector scaffolding. Reimplement the accepted design under production
error-handling, accessibility, test, and review requirements; do not copy the
prototype directly into the submitted branch.

## Anti-patterns

- **Variants that differ only in colour or copy.** That's a tweak, not a prototype. Real variants disagree about structure.
- **Sharing too much layout code between variants.** Shared representative data
  is useful; a shared layout defeats the comparison.
- **Wiring variants to real mutations.** Read-only prototypes are fine. If a variant needs to mutate, point it at a stub — the question is "what should this look like", not "does the backend work".
- **Promoting the prototype directly to production.** The variant code was written under prototype constraints (no tests, minimal error handling). Rewrite it properly when you fold it in.
