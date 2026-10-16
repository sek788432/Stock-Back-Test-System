# Logic Prototype

A small throwaway harness that lets the user drive a state model through
representative cases. Use this when the question is about business logic, state
transitions, or data shape and needs executable evidence before a production
contract is chosen.

## When this is the right shape

- "I'm not sure if this state machine handles the edge case where X then Y."
- "Does this data model actually let me represent the case where..."
- "I want to feel out what the API should look like before writing it."
- Anything where the user wants to apply actions and inspect each resulting
  state.

If the question is "what should this look like" — wrong branch. Use [UI.md](UI.md).

## Process

### 1. State the question

Before writing code, write down what state model and what question you're prototyping. One paragraph, in the prototype's README or a comment at the top of the file. A logic prototype that answers the wrong question is pure waste — make the question explicit so it can be checked later, whether the user is watching now or returning to it AFK.

### 2. Pick the language

Use C++20 for backend/application logic. Python is appropriate only when the
question belongs to DataFetcher or the planned Python strategy boundary. Do not
add another runtime, package manager, or dependency for the prototype.

### 3. Isolate the logic in a portable module

Keep the experimental logic behind a small pure interface. Both that logic and
its driver remain throwaway evidence; an accepted result is implemented later
through the repository's production workflow.

The right shape depends on the question:

- **A pure transition function** — accepts state and an action, then returns the
  next state. Good when actions are discrete events and state is one value.
- **A state machine** — explicit states and transitions. Good when "which actions are even legal right now" is part of the question.
- **A small set of pure functions** over a plain data type. Good when there's no implicit current state — just transformations.
- **A class or module with a clear method surface** when the logic genuinely owns ongoing internal state.

Pick whichever shape best fits the question, not whichever is easiest to drive.
Keep I/O, terminal presentation, clocks, and filesystem access outside the
experimental logic.

This separation makes the conclusion transferable. It does not authorize
copying the prototype into production without the normal spec, tests, and
review.

### 4. Build the smallest driver that exposes the state

Use fixed scenarios or a line-oriented terminal driver with no new UI library.
For every action, show the action, accepted/rejected result, and complete
relevant state in stable field order. Pin clocks and random seeds. Prefer
synthetic in-memory inputs; if the question requires persistence, use a clearly
temporary database or file below an isolated temporary directory.

The driver exists to compare observable transitions, not to prove that its
presentation or internal structure belongs in production.

### 5. Make it runnable in one command

Use one verified command from the checked-in CMake/Python toolchain. Keep any
throwaway target or harness outside the submitted production diff unless the
user explicitly chooses to promote it.

### 6. Hand it over

Give the user the run command. They'll drive it themselves; the interesting moments are when they say "wait, that shouldn't be possible" or "huh, I assumed X would be different" — those are the bugs in the _idea_, which is the whole point. If they want new actions added, add them. Prototypes evolve.

### 7. Capture the answer and the prototype

Once the prototype answers its question, record the observation and conclusion
in the issue or PR, then remove the harness. Implement the accepted behavior
separately with the required tests and review evidence.

## Anti-patterns

- **Don't add tests.** A prototype that needs tests is no longer a prototype.
- **Don't wire it to the real database.** Use an in-memory store unless the question is specifically about persistence.
- **Don't generalise.** No "what if we wanted to support X later." The prototype answers one question.
- **Don't blur logic and its driver.** Experimental state transitions must not
  depend on prompts, terminal formatting, or local paths.
- **Don't ship the driver into production.** Only an independently reviewed
  production implementation may be retained.
