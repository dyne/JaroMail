# Your humble and faithful electronic postman

Jaromail brings established free software tools together for private, efficient
email management in the terminal. Keep your messages locally in Maildir,
work offline, and use your own tools to read, search and organize your archives.

**[Read the manual](manual/index.md)** · [Install Jaromail](install.md) ·
[Look up a command](jaro.1.md)

## User manual

Follow the manual from beginning to end, or go straight to the chapter you need.

| Chapter | What you will find |
| --- | --- |
| [About Jaromail](manual/index.md) | Features and the vision behind local email |
| [Architecture](manual/architecture.md) | How the mail tools fit together |
| [Setup](manual/setup.md) | Build, install and initialize a mail directory |
| [Configuration](manual/configuration.md) | Accounts, folders, whitelists and blacklists |
| [Workflow](manual/workflow.md) | Fetch, read, compose, reply, send and peek |
| [Searching](manual/searching.md) | Search expressions, dates and time ranges |
| [Statistics](manual/statistics.md) | Analyze mail traffic and replay command output |
| [Addressbook](manual/addressbook.md) | Contacts, private recipient groups and exports |
| [Filters](manual/filters.md) | Route messages, merge folders and manage storage |
| [Shell environment](manual/shell-environment.md) | Completion, platform integrations and the manual license |

## Install the latest Linux release

For Linux x86_64 and arm64, this installs the matching release, including all
Jaromail helpers, the bundled addressbook, configuration templates and manpages:

```bash
curl -fsSL https://dyne.github.io/JaroMail/install.sh | bash
```

The installer verifies the release checksum and uses `~/.local` by default.
External mail tools need to be installed separately.
[Installation requirements and first steps](install.md).

## Keep a reference nearby

The [complete manual](jaromail-manual.md) and [jaro(1)](jaro.1.md) are available
as searchable web pages. [Reference & downloads](reference.md) includes the
original manpages, Markdown, Org, TeX and PDF editions, plus technical notes.

Use **Search** in the navigation bar to find a command or topic across the site.

## Project and community

Jaromail is a [Dyne.org](https://dyne.org) project. Find the source and report
issues on [GitHub](https://github.com/dyne/JaroMail), or join the
[Dyne community channels](https://dyne.org/linktree).
