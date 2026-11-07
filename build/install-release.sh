#!/usr/bin/env bash
# Install the complete, checksum-verified Linux release for this machine.
set -Eeuo pipefail

install_tmp=''
cleanup() {
  local result=$?
  if [[ -n $install_tmp ]]; then rm -rf -- "$install_tmp"; fi
  exit "$result"
}
fail() { printf 'Jaromail installer: %s\n' "$*" >&2; exit 1; }
download() {
  curl --fail --silent --show-error --location --proto '=https' \
    --tlsv1.2 --connect-timeout 10 --max-time 180 --retry 2 "$@"
}

main() {
  local prefix=${JARO_INSTALL_PREFIX:-${HOME:?HOME must be set}/.local}
  while (( $# )); do
    case $1 in
      --prefix)
        (( $# >= 2 )) || fail '--prefix needs an absolute directory'
        prefix=$2
        shift 2 ;;
      -h|--help)
        printf 'Usage: bash install.sh [--prefix /absolute/path]\nDefault: %s (or JARO_INSTALL_PREFIX). No sudo is used.\n' "$HOME/.local"
        return 0 ;;
      *) fail "Unknown option: $1 (use --help)" ;;
    esac
  done
  [[ $prefix == /* && $prefix != / ]] || fail 'Choose an absolute install prefix other than /'
  local tool arch release_url tag version bundle expected actual
  for tool in curl tar sha256sum uname mktemp mkdir cp; do
    command -v "$tool" >/dev/null || fail "Required command is missing: $tool"
  done
  [[ $(uname -s) == Linux ]] || fail 'Prebuilt packages support Linux only'
  case $(uname -m) in
    x86_64|amd64) arch=x86_64 ;;
    aarch64|arm64) arch=arm64 ;;
    *) fail "Unsupported architecture: $(uname -m); use a source build" ;;
  esac
  install_tmp=$(mktemp -d "${TMPDIR:-/tmp}/jaromail-install.XXXXXXXX")
  trap cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  release_url=$(download --output /dev/null --write-out '%{url_effective}' \
    'https://github.com/dyne/JaroMail/releases/latest')
  tag=${release_url##*/}
  [[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail 'Cannot resolve the latest stable release'
  version=${tag#v}
  bundle="jaromail-${version}-linux-${arch}"
  printf 'Downloading Jaromail %s for Linux %s…\n' "$version" "$arch"
  download --output "$install_tmp/package.tar.gz" \
    "https://github.com/dyne/JaroMail/releases/download/$tag/$bundle.tar.gz"
  download --output "$install_tmp/package.sha256" \
    "https://github.com/dyne/JaroMail/releases/download/$tag/$bundle.tar.gz.sha256"
  read -r expected _ < "$install_tmp/package.sha256"
  [[ $expected =~ ^[a-fA-F0-9]{64}$ ]] || fail 'Invalid release checksum'
  actual=$(sha256sum "$install_tmp/package.tar.gz")
  actual=${actual%% *}
  [[ ${actual,,} == "${expected,,}" ]] || fail 'Checksum mismatch; nothing installed'
  mkdir "$install_tmp/unpacked"
  tar -xzf "$install_tmp/package.tar.gz" -C "$install_tmp/unpacked" --no-same-owner
  local package="$install_tmp/unpacked/$bundle"
  [[ -x $package/bin/jaro && -f $package/share/jaromail/VERSION ]] \
    || fail 'Unexpected release archive layout; nothing installed'
  # Copy the whole release: helpers, addressbook, templates, manuals and licenses.
  mkdir -p -- "$prefix/bin" "$prefix/share"
  cp -a -- "$package/bin/." "$prefix/bin/"
  cp -a -- "$package/share/." "$prefix/share/"
  printf 'Installed Jaromail %s in %s\n' "$version" "$prefix"
  printf 'Add %s/bin to PATH, then run: jaro init\n' "$prefix"
  printf 'Runtime tools are separate: zsh, fetchmail, msmtp, neomutt, notmuch, pinentry, wipe, mblaze.\n'
}

main "$@"
