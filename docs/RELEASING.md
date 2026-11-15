# Releasing

Releases are fully automated by [`.github/workflows/release.yml`](../.github/workflows/release.yml) with [semantic-release](https://semantic-release.gitbook.io) (configuration in [`.releaserc.json`](../.releaserc.json)).

On every push to `master`, the commits since the last `vX.Y.Z` tag are analysed:

| Commit type | Release |
|---|---|
| `fix:`, `perf:`, `revert:` | patch |
| `feat:` | minor |
| `BREAKING CHANGE` footer or `!` after the type | minor while on `0.x`, major from `1.0.0` |
| `docs:`, `chore:`, `ci:`, `refactor:`, `test:`, `style:`, `build:` | none |

When a release is due:

1. the Linux and Windows builds are compiled with `-DCRUSADER_VERSION=X.Y.Z`, tested (`tests/`, no game data) and packaged;
2. the PortMaster build is cross-compiled for aarch64 with Zig (`tools/portmaster/build.py`) and packaged with its launcher;
3. `SHA256SUMS` is computed and a GitHub release `vX.Y.Z` is created with generated notes and the archives.

Pushes to `master` and pull requests are checked by [`.github/workflows/ci.yml`](../.github/workflows/ci.yml): commit messages (commitlint) and a build with the tests on Linux and Windows. Pushes to `master` also trigger the release workflow, which analyses the commits and builds and tests the binaries when a release is due.

There is nothing to do by hand: to publish a version, merge or push a `fix:` or `feat:` commit.
