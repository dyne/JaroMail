# Repository Guidelines

## Project Structure & Module Organization

Jaromail is a terminal mail workflow built mostly from Zsh scripts plus small C and Objective-C helpers. The main command is `src/jaro`; reusable shell modules live under `src/zlibs/`, grouped by concern such as `accounts`, `addressbook`, `maildirs`, `filters`, `imap`, and `search`. C helpers such as `fetchaddr.c`, `parsedate.c`, `dotlock.c`, and `gpgewrap.c` are in `src/`. Mutt configuration and color assets are in `src/mutt/`. Build scripts are in `build/`; documentation is in `doc/`; optional integrations and tests are in `extras/`.

## Build, Test, and Development Commands

- `make`: runs `./build/auto build` and builds the default platform artifacts.
- `make clean`: removes generated helper binaries and object files from `src/`.
- `sudo make install`: installs Jaromail under `/usr/local/share/jaromail` and creates `/usr/local/bin/jaro`.
- `extras/test/run-source.sh`: smoke test for the checked-out code; runs `src/jaro` with a local staged install in a temp path.
- `extras/test/test-account-wizard.sh`: validates the interactive `wizard` account flow using scripted stdin.
- `./extras/test/inittest.sh`: runs the installed smoke test using `/tmp/jaromail-test`.
- `PREFIX=/tmp/jaro make install`: checks install layout without touching `/usr/local`; adjust the smoke test paths if using this.

## Coding Style & Naming Conventions

Prefer minimal, readable changes. Keep Zsh functions lowercase, shell modules grouped by feature under `src/zlibs/`, and environment knobs such as `JAROMAILDIR` and `JAROWORKDIR` uppercase. Preserve tab-indented Makefile recipes. For C helpers, follow the local K&R-style brace layout and keep helper functions small. Do not add dependencies unless they are necessary and documented.

## Testing Guidelines

There is no broad unit-test suite. Use `extras/test/run-source.sh` for checkout-level validation, `extras/test/test-account-wizard.sh` for account onboarding behavior, and `extras/test/inittest.sh` for installed-path validation. The tests depend on external mail tools (`fetchmail`, `msmtp`, `mutt`/`neomutt`, `notmuch`, `pinentry-curses`, `abook`, `wipe`, `mblaze`/`maddr`) plus build tools from `.travis.yml`. For narrow C helper changes, rebuild with `make` and add a focused command-line check where practical.

## Architecture Notes

Respect the existing slice-by-feature organization. Keep IO boundaries explicit: shell modules should call external mail tools clearly, and helpers should do one job well.
