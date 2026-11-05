# Repository Guidelines

## Project Structure & Module Organization

Jaromail is a terminal mail workflow built mostly from Zsh scripts plus small C and Objective-C helpers. The main command is `src/jaro`; reusable shell modules live under `src/zlibs/`, grouped by concern such as `accounts`, `addressbook`, `maildirs`, `filters`, `imap`, and `search`. C helpers such as `fetchaddr.c`, `parsedate.c`, `dotlock.c`, and `gpgewrap.c` are in `src/`. Mutt configuration and color assets are in `src/mutt/`. Build scripts are in `build/`; documentation is in `doc/`; optional integrations and tests are in `extras/`.

## Build, Test, and Development Commands

- `make`: runs `./build/auto build` and builds the default platform artifacts.
- `make clean`: removes generated helper binaries and object files from `src/`.
- `make test`: runs all source and focused regression tests, reporting missing dependencies as skips.
- `make test-strict`: runs the same suite and fails if any script is skipped; used by CI.
- `sudo make install`: installs Jaromail under `/usr/local/share/jaromail` and creates `/usr/local/bin/jaro`.
- `extras/test/run-source.sh`: smoke test for the checked-out code; runs `src/jaro` with a local staged install in a temp path.
- `extras/test/test-account-wizard.sh`: validates the interactive `wizard` account flow using scripted stdin.
- `./extras/test/inittest.sh`: runs the installed smoke test using `/tmp/jaromail-test`.
- `PREFIX=/tmp/jaro make install`: checks install layout without touching `/usr/local`; adjust the smoke test paths if using this.

## Coding Style & Naming Conventions

Prefer minimal, readable changes. Keep Zsh functions lowercase, shell modules grouped by feature under `src/zlibs/`, and environment knobs such as `JAROMAILDIR` and `JAROWORKDIR` uppercase. Preserve tab-indented Makefile recipes. For C helpers, follow the local K&R-style brace layout and keep helper functions small. Do not add dependencies unless they are necessary and documented.

## Testing Guidelines

Use `make test` for checkout-level validation and `make test-strict` for a complete dependency-equipped run. The runner discovers `extras/test/test-*.sh` automatically; module fixtures require no live mail service. Integration tests depend on external mail tools (`fetchmail`, `msmtp`, `mutt`/`neomutt`, `notmuch`, `pinentry-curses`, `abook`, `wipe`, `mblaze`/`maddr`) plus build tools from `.github/workflows/pr-tests.yml`. Missing prerequisites use exit 77; assertion failures must fail the test. Use `extras/test/inittest.sh` separately for installed-path validation. For narrow C helper changes, rebuild with `make` and add a focused command-line check where practical.

## Architecture Notes

Respect the existing slice-by-feature organization. Keep IO boundaries explicit: shell modules should call external mail tools clearly, and helpers should do one job well.
