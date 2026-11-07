# Reference & downloads

## Manpages

| Reference | Read online | Original source |
| --- | --- | --- |
| `jaromail(1)` | [Complete user manual](jaromail-manual.md) | [Manpage](/downloads/jaromail.1), [Markdown](/downloads/jaromail-manual.md) |
| `jaro(1)` | [Command reference](jaro.1.md) | [Manpage](/downloads/jaro.1), [Markdown](/downloads/jaro.1.md) |

After installation, use `man jaromail`, `man jaro`, or `jaro -h` in a terminal.
The web chapters and reference pages are generated from the same Markdown
sources used to maintain the committed manpages.

## Original manual editions

- [PDF manual](/downloads/jaromail-manual.pdf) — the committed print edition;
  it may lag behind the current Markdown and web manual.
- [Org source](/downloads/jaromail-manual.org)
- [TeX source](/downloads/jaromail-manual.tex)
- [Architecture diagram](/downloads/jaromail-diagram.png)

The manual is copyright Denis Roio, distributed under the GNU Free Documentation
License 1.3 or later. See the [full notice](manual/shell-environment.md#license).

## Configuration templates

`jaro init` creates the appropriate configuration. These templates are also
available for reference:

- [Default account](/downloads/Accounts/default.txt) and [account notes](/downloads/Accounts/README.txt)
- [Native filter examples](/downloads/Filters.native.txt) and [legacy filters](/downloads/Filters.txt)
- [Aliases](/downloads/Aliases.txt), [applications](/downloads/Applications.txt), [Mutt settings](/downloads/Mutt.txt)

## Current behavior and maintenance

- [Command contracts](command-contracts.md): inputs, side effects and exit behavior.
- [Filter compatibility](filter-compatibility.md): native behavior, migration and rollback.
- [Filter policy](filter-policy.md) and [interfaces](filter-interfaces.md): precise routing and cache contracts.
- [Optional integrations](optional-integrations.md): legacy and platform-specific features.
- [Maintenance inventory](maintenance-inventory.md): maintained components and known gaps.
- [Domain notes](domain-notes.md), [benchmarks](filter-benchmarks.md) and [Sieve tools](sieve-tools.md).
- [Builds & releases](releases.md) and [contributing](contributing.md).
