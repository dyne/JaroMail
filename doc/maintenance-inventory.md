# Maintenance Inventory

| Path | Current role | Installed by default | Referenced by docs | External dependencies | Classification | Recommendation |
| --- | --- | --- | --- | --- | --- | --- |
| `src/jaro` | CLI entrypoint and command router | Yes (`Makefile` installs wrapper + script) | Yes (`README.md`, manual) | `zsh` + runtime tools | Core | Keep as thin bootstrap/dispatch only. |
| `src/zlibs/*` | Core use-case/domain shell libraries | Yes (`Makefile` copies all) | Indirect (manual command docs) | tool-specific (`mblaze`, `notmuch`, `fetchmail`, etc.) | Core | Keep; continue per-family handler extraction and test coverage. |
| `src/*.c` (`fetchaddr`, `parsedate`, `dotlock`, helpers) | Native helper binaries used by CLI flows | Yes (`build/gnu/*` copied) | Indirect/manual examples | `gcc`, libc, autotools build chain | Core | Keep; add direct helper tests and document exact call sites. |
| `src/mutt` | Mutt templates/config defaults | Yes (`Makefile` copies to `mutt/`) | Yes (`README.md`, manual) | `mutt` or `neomutt` | Core | Keep as default MUA integration. |
| `src/stats` | ASCII/web statistics utilities (`stats`, `timecloud`) | Yes (`Makefile` copies to `stats/`) | Yes (manual stats section) | shell + legacy JS assets | Legacy | Keep for now but mark legacy/optional and consider removing from default install later. |
| `src/MailToMutt` | macOS Mail.app bridge project | No | Not in README/manual | macOS/Xcode | Optional/Legacy | Keep in repo as optional macOS integration; exclude from core maintenance surface. |
| `src/gnome-keyring` | keyring backend adapter | No direct install path in root `Makefile` | Mentioned in manual | `libgnome-keyring` stack | Optional/Legacy | Keep but document as optional backend, not part of default test path. |
| `src/zuper` | shell utility library runtime dependency | Yes (`Makefile` copies `zuper*`) | Indirect | `zsh` | Core | Keep; treat as infrastructure dependency. |
| `extras/roundcube-majordomo` | Roundcube plugin/integration | No | Has local README only | Roundcube/PHP stack | Optional | Keep out of core path; document as optional integration. |
| `extras/osx-abquery` | macOS addressbook helper source | No | Not in README/manual | macOS frameworks/clang | Optional/Legacy | Keep as optional; no core test obligation. |
| `extras/emlx2maildir` | conversion helper script | No | Not in README/manual | shell/perl tools | Optional | Keep and document invocation when relevant. |
| `extras/gnupg-mass-recv` | GnuPG helper scripts | No | Not in README/manual | `gpg` | Optional | Keep optional, document with explicit support scope. |
| `extras/shell_completion` | shell completion snippets | No | Not in README/manual | shell-specific | Optional | Keep and document install-by-choice. |
| `extras/zaw-jaromail` | Zaw integration helper | No | Not in README/manual | Zaw/zsh ecosystem | Optional/Legacy | Keep optional, low priority maintenance. |
| `build/*` | build/install/release scripts per platform | N/A (tooling) | Indirect (`README.md` make flow) | platform toolchains | Core (tooling) | Keep; separate default Linux flow from platform-specific scripts in docs. |

## Evidence Notes

- Default install currently includes both `src/mutt` and `src/stats` plus all `src/zlibs/*` (`Makefile` `install` target).
- Stats is actively documented in `doc/jaromail-manual.md|org|tex`, but most extras are not referenced in top-level docs.
