#!/usr/bin/env bash
# Install the complete, checksum-verified Linux release for this machine.
set -Eeuo pipefail

install_tmp=''
runtime_stage=''
launcher_stage=''
install_runtime=''
install_bin=''
published_launchers=()
runtime_switch_started=0
cleanup() {
  local result=$?
  if (( result && runtime_switch_started )); then
    if [[ ! -e $runtime_stage/new && -d $install_runtime ]]; then
      mv -T -- "$install_runtime" "$runtime_stage/failed" ||
        fail "Could not move the failed runtime. Recovery files are in $runtime_stage"
    fi
    if [[ -d $runtime_stage/previous ]]; then
      mv -T -- "$runtime_stage/previous" "$install_runtime" ||
        fail "Restore the previous runtime from $runtime_stage/previous"
    fi
    local name
    for name in "${published_launchers[@]}"; do
      if [[ -e $launcher_stage/previous/$name || -L $launcher_stage/previous/$name ]]; then
        mv -fT -- "$launcher_stage/previous/$name" "$install_bin/$name" ||
          fail "Restore the previous launcher from $launcher_stage/previous/$name"
      elif [[ -e $install_bin/$name || -L $install_bin/$name ]]; then
        rm -- "$install_bin/$name"
      fi
    done
  fi
  if [[ -n $runtime_stage ]]; then rm -rf -- "$runtime_stage"; fi
  if [[ -n $launcher_stage ]]; then rm -rf -- "$launcher_stage"; fi
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
  local installed='' installed_base='' updating=0
  while (( $# )); do
    case $1 in
      --prefix)
        (( $# >= 2 )) || fail '--prefix needs an absolute directory'
        prefix=$2
        shift 2 ;;
      --installed-version)
        (( $# >= 2 )) || fail '--installed-version needs a version'
        installed=$2
        updating=1
        shift 2 ;;
      -h|--help)
        printf 'Usage: bash install.sh [--prefix /absolute/path]\nDefault: %s (or JARO_INSTALL_PREFIX). No sudo is used.\n' "$HOME/.local"
        return 0 ;;
      *) fail "Unknown option: $1 (use --help)" ;;
    esac
  done
  [[ $prefix == /* && $prefix != / ]] || fail 'Choose an absolute install prefix other than /'
  if (( updating )); then
    [[ $installed =~ ^([0-9]+\.[0-9]+\.[0-9]+)([-+][0-9A-Za-z.+-]+)?$ ]] ||
      fail 'Invalid installed version'
    installed_base=${BASH_REMATCH[1]}
  fi
  local tool arch release_url tag version bundle expected actual ordered
  for tool in curl tar sha256sum uname mktemp mkdir cp mv flock sort; do
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
  if (( updating )); then
    ordered=$(printf '%s\n' "$installed_base" "$version" | LC_ALL=C sort -V)
    if [[ ${ordered##*$'\n'} != "$version" ||
          ( $installed_base == "$version" && ${installed#"$installed_base"} != -* ) ]]; then
      printf 'JaroMail %s is up to date (latest stable release is %s).\n' "$installed" "$version"
      return 0
    fi
  fi
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
  [[ $(<"$package/share/jaromail/VERSION") == "$version" ]] ||
    fail 'The archive version differs from the release tag'

  mkdir -p -- "$prefix/bin" "$prefix/share"
  local lock_fd
  exec {lock_fd}< "$prefix/share"
  flock -n "$lock_fd" || fail 'Another JaroMail installation is in progress'
  install_runtime="$prefix/share/jaromail"
  install_bin="$prefix/bin"
  [[ ! -L $install_runtime && ( ! -e $install_runtime || -d $install_runtime ) ]] ||
    fail 'The installed runtime must be a regular directory'
  if (( updating )); then
    [[ -r $install_runtime/VERSION && $(<"$install_runtime/VERSION") == "$installed" ]] ||
      fail 'The installation changed while downloading; rerun jaro update'
  fi

  runtime_stage=$(mktemp -d "$prefix/share/.jaromail-install.XXXXXXXX")
  launcher_stage=$(mktemp -d "$prefix/bin/.jaromail-install.XXXXXXXX")
  cp -a -- "$package/share/jaromail" "$runtime_stage/new"
  mkdir "$launcher_stage/new" "$launcher_stage/previous"
  cp -a -- "$package/bin/." "$launcher_stage/new/"
  # Publish the complete runtime together, with its matching binaries and libraries.
  shopt -s dotglob nullglob
  local asset name
  for asset in "$package/share/"*; do
    [[ ${asset##*/} == jaromail ]] || cp -a --remove-destination -- "$asset" "$prefix/share/"
  done
  for asset in "$launcher_stage/new/"*; do
    name=${asset##*/}
    if [[ -e $install_bin/$name || -L $install_bin/$name ]]; then
      [[ -L $install_bin/$name || ! -d $install_bin/$name ]] ||
        fail "Cannot replace directory $install_bin/$name with a launcher"
      cp -a -- "$install_bin/$name" "$launcher_stage/previous/"
    fi
  done
  runtime_switch_started=1
  if [[ -d $install_runtime ]]; then
    mv -T -- "$install_runtime" "$runtime_stage/previous"
  fi
  mv -T -- "$runtime_stage/new" "$install_runtime"
  for asset in "$launcher_stage/new/"*; do
    name=${asset##*/}
    published_launchers+=("$name")
    mv -fT -- "$asset" "$install_bin/$name"
  done
  printf 'Installed Jaromail %s in %s\n' "$version" "$prefix"
  if (( ! updating )); then
    printf 'Add %s/bin to PATH, then run: jaro init\n' "$prefix"
    printf 'Runtime tools are separate: zsh, fetchmail, msmtp, neomutt, notmuch, pinentry, wipe, mblaze.\n'
  fi
}

main "$@"
