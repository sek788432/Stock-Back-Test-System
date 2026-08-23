# Domain Docs

How the engineering skills consume this repository's domain documentation.

## Before exploring, read these

- `CONTEXT.md` at the repository root, when it exists.
- `CONTEXT-MAP.md` at the repository root, when it exists; it points to the
  context files relevant to a multi-context repository.
- `Docs/Decisions/ImportantDecisions.md` for living decisions that constrain
  the area being touched.

If a context file does not exist, proceed silently. The `/domain-modeling`
skill creates it lazily when terminology actually needs clarification. Do not
create a second decision archive; qualifying choices update the one living
important-decisions document.

## Use the glossary's vocabulary

When output names a domain concept, use the term defined in `CONTEXT.md`. Do
not drift to synonyms the glossary explicitly avoids. A missing term can mean
the proposed language is foreign to the project or that the glossary has a real
gap; distinguish those cases before editing it.

## Flag decision conflicts

If output contradicts a living important decision, surface the conflict instead
of silently overriding it. Resolve the ambiguity with the maintainer and update
the owning specification and living entry together before implementation.
