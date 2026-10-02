# Contributing to EmbCC

The contributor's guide is
[docs/internals/contributing.md](docs/internals/contributing.md). It
covers building EmbCC, the code style, commit messages, how to add an
option, an attribute, a builtin, a warning, an IR operation or a target,
and the tests a change needs before it is merged. The test suites are
described in [docs/internals/testing.md](docs/internals/testing.md), and
the project's standing choices in
[docs/internals/decisions.md](docs/internals/decisions.md).

The rules that apply to every change:

- EmbCC refuses what it cannot do correctly, with an error that names
  it. It never emits code that silently does something else.
- A change is done when a test exercises it, and a new test counts only
  after it has failed against the code before the change.
- A commit subject has the form `area: summary`. Do not add a
  `Co-Authored-By` line.
- A design decision goes into `docs/internals/decisions.md` with its
  reason and what would reopen it, under a new `D-0NN` identifier.
