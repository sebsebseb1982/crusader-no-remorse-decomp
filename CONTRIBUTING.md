# Contributing

Thanks for your interest in the project! Contributions of all kinds are welcome: bug reports, fixes, new features, documentation, and testing on different hardware.

## Before you start

- For substantial changes, **open an issue first** to discuss the approach.
- This repository contains **no game files**, and none must ever be committed: no game code, data, ISO, or screenshots of the original game. You need your own copy of *Crusader: No Remorse* to run the full test suite.
- By contributing, you agree that your work is released under the [GNU GPL v3](LICENSE) (or any later version).

## Reporting a bug

Please include:

- the game version (French 1.08 or US 1.23) and the port version (`./crusader-no-remorse --version`);
- your operating system (and device, for PortMaster handhelds);
- a screenshot, and a **savegame** if the bug happens in game;
- the steps to reproduce it;
- any relevant error messages or logs.

A savegame that reproduces the problem is the most useful thing you can attach.

## Building and testing

Requirements: CMake 3.21 or later and a C++20 compiler (Visual Studio 2022, GCC 11+, or Clang 14+).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # Windows: -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The tests in `tests/` do not require game data. The end-to-end tests in `e2e/` run only when the original game data is found (`-DCRUSADER_DATA_DIR=…`, default `data/`); tests that load saved games read them from `-DCRUSADER_TEST_SAVES_DIR=…`; `us_data_tests` runs when US data is found (`-DCRUSADER_US_DATA_DIR=…`, default `data_us/`). See the [README](README.md#building-from-source) for details.

All tests that can run on your machine must pass before you submit a pull request.

## Pull requests

- Keep each pull request **focused** on a single change.
- Describe **what** changes and **why**.
- Add or update tests where it makes sense, and update the documentation if behavior changes.
- Match the style of the surrounding code (naming, comment density, idioms).
- If you fix a bug found with a savegame, add it as a regression test when possible.

## Commit messages

Releases are fully automated: versions are computed from [Conventional Commits](https://www.conventionalcommits.org), so commit messages matter.

```
<type>(<optional scope>): <short description>
```

| Type | Effect on version |
| --- | --- |
| `feat` | minor bump |
| `fix`, `perf`, `revert` | patch bump |
| `docs`, `refactor`, `test`, `build`, `ci`, `chore` | no release |
| `BREAKING CHANGE` in the footer | minor bump (before 1.0) |

Examples: `feat(aiming): add directional aiming for right stick controls`, `fix(portmaster): remove ghost key strokes`.

See [`docs/RELEASING.md`](docs/RELEASING.md) for the release process.

## Other ways to help

- Test the game on other platforms and report what you find.
- Share savegames that reproduce bugs.
- Support the project financially: see [SPONSORING.md](SPONSORING.md).
