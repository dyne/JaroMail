# Repository Guidelines

## Project Structure & Module Organization

Jaromail is a terminal mail workflow built mostly from Zsh scripts plus small C and Objective-C helpers. The main command is `src/jaro`; reusable shell modules live under `src/zlibs/`, grouped by concern such as `accounts`, `addressbook`, `maildirs`, `filters`, `imap`, and `search`. C helpers such as `fetchaddr.c`, `parsedate.c`, `dotlock.c`, and `gpgewrap.c` are in `src/`. Mutt configuration and color assets are in `src/mutt/`. Build scripts are in `build/`; documentation is in `docs/`; optional integrations and tests are in `extras/`.

## Build, Test, and Development Commands

- `cmake -S . -B out/build -DCMAKE_BUILD_TYPE=Release`: configures the native build; use a fresh directory for each toolchain/sanitizer variant.
- `cmake --build out/build --parallel`: builds the seven native helpers and vendored abook; CMake defines all compiler targets, including regression drivers.
- `cmake --build out/build --target check-strict`: runs the full suite, requiring no skipped scripts. `ctest --test-dir out/build --output-on-failure` is also supported after building.
- `cmake --install out/build --prefix /your/prefix`: installs helpers, scripts, assigned version metadata and committed manuals. `DESTDIR` stages packages.
- `make`: compatibility wrapper around CMake using `build/gnu/cmake`; `make clean` cleans that build, not source files.
- `make test`: runs all source and focused regression tests, reporting missing dependencies as skips.
- `make test-strict`: runs the same suite and fails if any script is skipped; used by CI.
- `make install` or `cmake --install out/build`: defaults to the user prefix `$HOME/.local`; add `~/.local/bin` to `PATH`.
- `sudo make install-system` or `sudo cmake --build out/build --target install-system`: explicitly installs under `/usr/local`.
- `make install-system PREFIX=/usr DESTDIR="$PWD/package-root"`: stages distribution packages without root. The native CMake system target accepts `PREFIX` from the environment; both install targets honor `DESTDIR`.
- `extras/test/run-source.sh`: smoke test for the checked-out code; runs `src/jaro` with a local staged install in a temp path.
- `extras/test/test-account-wizard.sh`: validates the interactive `wizard` account flow using scripted stdin.
- `./extras/test/inittest.sh`: runs the installed smoke test using `/tmp/jaromail-test`.
- `PREFIX=/tmp/jaro make install`: checks install layout without touching `/usr/local`; adjust the smoke test paths if using this.

## Coding Style & Naming Conventions

Prefer minimal, readable changes. Keep Zsh functions lowercase, shell modules grouped by feature under `src/zlibs/`, and environment knobs such as `JAROMAILDIR` and `JAROWORKDIR` uppercase. Preserve tab-indented Makefile recipes. For C helpers, follow the local K&R-style brace layout and keep helper functions small. Do not add dependencies unless they are necessary and documented.

## Testing Guidelines

Use `make test` for checkout-level validation and `make test-strict` for a complete dependency-equipped run. The runner discovers `extras/test/test-*.sh` automatically; module fixtures require no live mail service. Integration tests depend on external mail tools (`fetchmail`, `msmtp`, `mutt`/`neomutt`, `notmuch`, `pinentry-curses`, `abook`, `wipe`, `mblaze`/`maddr`) plus build tools and `libncurses-dev libreadline-dev` from `.github/workflows/pr-tests.yml`. Installed Jaromail uses its bundled abook; some source fixtures still use a host abook. Missing prerequisites use exit 77; assertion failures must fail the test. Use `extras/test/inittest.sh` separately for installed-path validation. For narrow C helper changes, rebuild with `make` and add a focused command-line check where practical.

## Architecture Notes

Respect the existing slice-by-feature organization. Keep IO boundaries explicit: shell modules should call external mail tools clearly, and helpers should do one job well.

## Vendored abook

The complete pinned abook 0.6.2 release is in `vendor/abook/upstream`; its manifest and customization notes are in `vendor/abook/`. Keep CMake integration and generated headers outside the upstream tree (`cmake/abook/` and the build directory). Deliberate source changes need regression coverage and updated manifest digests while preserving the original archive provenance. Run `python3 vendor/verify.py` and `zsh extras/test/test-build.sh`. Linux static builds use the explicit `build/abook-deps.py` step with `--require-bundled` to reuse target ncurses and GNU readline from Dyne musl v2.6.0 (source fallbacks are only for manual older-toolchain builds), never host libraries or an implicit configure-time download.
