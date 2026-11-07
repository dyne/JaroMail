# Install Jaromail

## Quick install on Linux

Run this one-liner in a terminal:

```bash
curl -fsSL https://dyne.github.io/JaroMail/install.sh | bash
```

The Bash installer detects **x86_64** or **arm64**, resolves the latest stable
[GitHub release](https://github.com/dyne/JaroMail/releases/latest), downloads
that architecture's archive and its SHA-256 checksum, verifies the archive,
then installs the complete package under **`$HOME/.local`**. No `sudo` is used.
Running it again installs the latest release over the same prefix.

The package includes Jaromail's scripts, seven native helpers, bundled abook,
configuration templates, manpages and licenses. It does not bundle the external
mail applications listed below, and it does not change your mail directory.

To inspect the installer first or choose a different prefix:

```bash
curl -fsSL https://dyne.github.io/JaroMail/install.sh -o install-jaromail.sh
less install-jaromail.sh
bash install-jaromail.sh --prefix "$HOME/.local"
```

The installer needs Bash 4+, curl, tar and sha256sum. Unsupported systems,
missing release assets or checksum errors stop the installation with an error.
Its [source](https://github.com/dyne/JaroMail/blob/HEAD/build/install-release.sh) is in
the repository. [Download packages directly](https://github.com/dyne/JaroMail/releases/latest)
if you prefer to extract them yourself.

## Install the mail tools

Jaromail coordinates existing mail tools. On Debian and Ubuntu, install the
runtime packages with:

```bash
sudo apt-get install zsh fetchmail msmtp neomutt notmuch pinentry-curses \
  wipe mblaze vim netcat-traditional gnupg pass
```

Package names vary across distributions. The release includes its own abook;
a separate system abook is not required. Password storage uses `pass` and GnuPG;
configure your password store before saving account credentials.

Make the command available in your current shell:

```bash
export PATH="$HOME/.local/bin:$PATH"
jaro --version
```

Add that export to your shell profile to keep it for future sessions.

## Initialize your mail directory

```bash
export JAROMAILDIR="$HOME/Mail"
jaro init
jaro wizard
```

`jaro init` creates the mail directory and its configuration templates.
`jaro wizard` walks through account setup. You can also edit
`$JAROMAILDIR/Accounts/default.txt` with a text editor.

Continue with [Configuration](manual/configuration.md) and
[Workflow](manual/workflow.md) before fetching or sending mail.
For another mail identity, initialize another directory and select it with
`JAROMAILDIR`; keep that variable in your shell profile for later sessions.

## Build from source and other platforms

The manual's [Setup chapter](manual/setup.md) covers CMake builds and the
historical Apple/OSX installation. [Builds & releases](releases.md) documents
prefix overrides, system installation and packaging. See
[optional integrations](optional-integrations.md) for the status of platform
and legacy features.
