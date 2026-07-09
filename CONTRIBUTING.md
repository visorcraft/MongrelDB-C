# Contributing to MongrelDB C

Thanks for taking the time to help the MongrelDB C client. This document
describes how to propose a change, what we expect from a pull request, and
the coding standards that apply to the codebase.

If anything here is unclear or out of date, open an issue or a PR.

## Code of conduct

Be kind, be specific, assume good faith. Disagree about the technical
details, not the person. Public reviews stay focused on the diff.

## How to propose a change

The MongrelDB C client uses a standard **fork -> branch -> pull request**
workflow on GitHub.

1. **Fork** [`visorcraft/MongrelDB-C`](https://github.com/visorcraft/MongrelDB-C)
   to your GitHub account.
2. **Clone** your fork and add the upstream remote:

   ```sh
   git clone git@github.com:<you>/MongrelDB-C.git
   cd MongrelDB-C
   git remote add upstream https://github.com/visorcraft/MongrelDB-C.git
   ```

3. **Branch** from `master`. Pick a descriptive, kebab-case branch name:
   `fix-query-decode`, `feature/vector-search`, `docs/auth-guide`.

   ```sh
   git fetch upstream
   git switch -c my-change upstream/master
   ```

4. **Make focused commits.** One logical change per commit. Run the
   preflight (see below) before pushing.
5. **Open a pull request** against `master` on `visorcraft/MongrelDB-C`.
   Fill in the PR template:
   - **What.** One paragraph summary of the change.
   - **Why.** Bug fix? New feature? Doc fix? Link the issue if one
     exists.
   - **How to test.** The exact commands a reviewer should run.
   - **Risk.** What might break? What did you not test?

## Before you push: preflight

Run the full CI preflight locally:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-Wall -Wextra -Wpedantic -Werror"
cmake --build build
ctest --test-dir build --output-on-failure
```

The build must be warning-clean under `-Wall -Wextra -Wpedantic`. If a check
fails, fix the root cause - don't silence warnings or skip the test.

To run the live integration suite (requires a running `mongreldb-server`):

```sh
# Either boot a local daemon:
MONGRELDB_SERVER=./bin/mongreldb-server ctest --test-dir build --output-on-failure
# Or point at an already-running one:
MONGRELDB_URL=http://127.0.0.1:8453 ./build/test_mongreldb
```

Live tests self-skip when no server is reachable.

## What we look for in a review

- The change does one thing and does it well.
- Behavior changes ship with tests. New client behavior: a unit test
  alongside the code. Wire-format changes: cover the exact outgoing JSON
  keys. Daemon-dependent coverage: a live test that skips cleanly when no
  server is available.
- The change keeps this repo a thin client over `mongreldb-server`. Don't
  re-implement storage, indexing, WAL, or SQL planning logic here.
- Documentation is updated alongside the code (`docs/`, `README.md`) if the
  change affects users.
- Commits have clear messages (see below).

## Coding standards

### C

- **Version.** C99 (ISO/IEC 9899:1999). Do not use C11/C17-only features
  (such as `_Static_assert` beyond what compilers widely support, or
  `alignas`) without a fallback. The CI builds with `-std=c99`.
- **Warnings.** `-Wall -Wextra -Wpedantic` must be clean. Treat warnings
  as errors locally (`-Werror`).
- **Dependencies.** libcurl is the only runtime dependency beyond libc.
  The JSON parser is bundled and dependency-free - do not pull in an
  external JSON library. New third-party dependencies must be MIT or
  Apache-2.0 licensed and justified.
- **Memory model.** Document ownership in the header comments. The client
  owns all returned buffers (rows, strings, table lists); they are valid
  until the next call on the same client or `mongreldb_close()`.
- **Thread safety.** A `mongreldb_client` is not thread-safe. Document
  this and do not add locking.
- **Errors.** Return `int` error codes from every public function. Set
  the message via the internal `set_error` helpers. Never leak memory on
  an error path.
- **Naming.** `snake_case` for functions and types, `MONGRELDB_*` macros
  and constants. Header guards are `MONGRELDB_H` / `MONGRELDB_ENGINE_H`.
- **Style.** 4-space indent, no tabs, opening brace on the same line,
  `snake_case` locals. Run `clang-format` if you have it, but matching the
  surrounding style is what matters.

### Commit messages

- Subject line: imperative mood, <= 72 characters, no trailing period.
  Example: `Add FM-index full-text condition to query builder`.
- Body: wrap at 72 characters. Explain *why*, not *what* (the diff
  shows the what).
- Reference issues with `Fixes #123` / `Refs #123` on a final line
  when applicable.
- **Never** add AI/assistant attribution (no `Co-Authored-By`, no
  `Generated with`, no tool names).

## Issue reports

A useful bug report includes:

- The MongrelDB C client version (from git tag or `MONGRELDB_C_VERSION_*`).
- Your compiler and version (`cc --version`) and OS.
- The `mongreldb-server` version if the issue involves live requests.
- The exact code or commands that reproduce the issue.
- The expected result and the actual result.
- Any error output or stack trace.

Feature requests are welcome. Please describe the problem you're trying
to solve before proposing the solution.

## Security

If you find a vulnerability, **do not** open a public GitHub issue.
Report it privately through GitHub's private vulnerability reporting -
the repository's **Security** tab -> **Report a vulnerability**. The full
policy is in [`SECURITY.md`](SECURITY.md).

## Licensing

The MongrelDB C client is dual-licensed under MIT OR Apache-2.0. By
contributing, you agree that your changes are made available under the
same license.

- Do **not** paste code from other database clients unless you have done
  a license review first.
- New third-party dependencies must be MIT or Apache-2.0 licensed.

Thanks again - looking forward to your PR.
