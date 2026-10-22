#!/usr/bin/env bash
#
# Antigravity CLI - installer for PRoot (Debian/Ubuntu) userlands
#
# The upstream Antigravity CLI engine is an aarch64 Linux binary that assumes a
# 48-bit virtual address space.  Android userlands only provide 39 bits, so
# the engine has to be patched before it can run under PRoot.
#
# This installer:
#
#   1. detects the host and, when started from native Termux, sets up a PRoot
#      Debian/Ubuntu userland first,
#   2. installs the packages the build needs (curl, tar, ca-certificates, gcc),
#   3. downloads the official engine and verifies its SHA-512 checksum,
#   4. compiles the bootstrapper and applies the VA39 memory patches,
#   5. installs the resulting pair and verifies that it runs.
#
# Usage:
#   bash install.sh [options]
#
set -Eeuo pipefail

# ── Defaults ────────────────────────────────────────────────────────────────
readonly PKG_NAME="antigravity-cli-proot"
readonly PKG_VERSION="1.0.0"

# The Termux environment is captured first: in Termux, PREFIX points at the
# Termux prefix and must not be confused with the install target below.
TERMUX_HOME_PREFIX="${PREFIX:-}"
TERMUX_HOME_VERSION="${TERMUX_VERSION:-}"

INSTALL_PREFIX="${AGY_PREFIX:-/usr/local/bin}"
MANIFEST_URL="${AGY_MANIFEST_URL:-https://antigravity-cli-auto-updater-974169037036.us-central1.run.app/manifests/linux_arm64.json}"
ENGINE_URL="${AGY_ENGINE_URL:-}"
ENGINE_FILE="${AGY_ENGINE_FILE:-}"
PROOT_DISTRO="${AGY_PROOT_DISTRO:-ubuntu}"
readonly ENGINE_ARCH="aarch64"

LAUNCH=1
KEEP_STAGING=0
SKIP_DEPS="${AGY_SKIP_DEPS:-0}"
FORCE_DEPS="${AGY_FORCE_DEPS:-0}"
INSIDE_PROOT=0
UNINSTALL=0
ENGINE_VERSION="${AGY_VERSION:-}"
ENGINE_PATH=""
STAGING_DIR=""
SELF_PATH="${BASH_SOURCE[0]:-}"

# ── Output ──────────────────────────────────────────────────────────────────
# Everything except the final summary goes to stderr, so that the helpers can
# return data on stdout.
if [[ -t 2 ]]; then
  C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'; C_RED=$'\033[31m'; C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'; C_RESET=$'\033[0m'
else
  C_BOLD=""; C_DIM=""; C_RED=""; C_GREEN=""; C_YELLOW=""; C_RESET=""
fi

step() { printf '%s==>%s %s%s%s\n' "$C_GREEN" "$C_RESET" "$C_BOLD" "$*" "$C_RESET" >&2; }
info() { printf '    %s%s%s\n' "$C_DIM" "$*" "$C_RESET" >&2; }
warn() { printf '%swarning:%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
fail() { printf '%serror:%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; }
die()  { trap - ERR; fail "$*"; exit 1; }

on_error() {
  local code=$?
  local line=${1:-?}
  printf '\n%serror:%s installation failed at line %s (exit %s).\n' \
    "$C_RED" "$C_RESET" "$line" "$code" >&2
  exit "$code"
}
trap 'on_error $LINENO' ERR

usage() {
  cat <<EOF
${PKG_NAME} ${PKG_VERSION}

Installs the Antigravity CLI into a PRoot (Debian/Ubuntu) userland.

Usage:
  bash install.sh [options]

Options:
  --prefix DIR     Install the binaries into DIR (default: /usr/local/bin)
  --no-launch      Do not start the CLI after the installation
  --keep-staging   Keep the temporary build directory for inspection
  --uninstall      Remove a previous installation
  -h, --help       Show this help

Environment:
  AGY_PREFIX         Same as --prefix
  AGY_ENGINE_URL     Install a specific engine tarball instead of the latest
  AGY_ENGINE_FILE    Install a local engine tarball (no download)
  AGY_MANIFEST_URL   Override the official release manifest URL
  AGY_PROOT_DISTRO   PRoot distribution used from native Termux (default: ubuntu)
  AGY_SKIP_DEPS=1    Do not install packages
  AGY_FORCE_DEPS=1   Always run the package installation
EOF
}

ORIG_ARGS=("$@")
while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)       [[ $# -ge 2 ]] || die "--prefix requires a directory"; INSTALL_PREFIX="$2"; shift 2 ;;
    --prefix=*)     INSTALL_PREFIX="${1#*=}"; shift ;;
    --no-launch)    LAUNCH=0; shift ;;
    --keep-staging) KEEP_STAGING=1; shift ;;
    --uninstall)    UNINSTALL=1; shift ;;
    --inside-proot) INSIDE_PROOT=1; shift ;;
    -h|--help)      usage; exit 0 ;;
    *)              die "unknown option: $1 (try --help)" ;;
  esac
done

# ── Locate the package sources ──────────────────────────────────────────────
# The script has to be a real file so it can be re-executed inside a PRoot
# userland.  When it is piped straight from the network it is materialised in
# the Termux filesystem first, because that filesystem is visible from inside
# the PRoot container.
if [[ ! -f "$SELF_PATH" ]]; then
  SELF_DIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
  [[ -d "$SELF_DIR" ]] || SELF_DIR=/tmp
  mkdir -p "$SELF_DIR" || die "cannot create a temporary directory"
  SELF_COPY="$(mktemp -d "$SELF_DIR/agy-install.XXXXXX")/install.sh"
  cat >"$SELF_COPY"
  chmod 0755 "$SELF_COPY"
  exec bash "$SELF_COPY" "${ORIG_ARGS[@]}"
fi

PKG_ROOT="$(cd "$(dirname "$SELF_PATH")" && pwd)"
readonly PKG_ROOT
readonly SRC_DIR="$PKG_ROOT/src"
SELF_ABS="$PKG_ROOT/$(basename "$SELF_PATH")"
readonly SELF_ABS

# ── Uninstall ───────────────────────────────────────────────────────────────
if [[ "$UNINSTALL" -eq 1 ]]; then
  step "Removing $PKG_NAME from $INSTALL_PREFIX"
  for file in agy agy.va39 .agy.version; do
    if [[ -e "$INSTALL_PREFIX/$file" ]]; then
      rm -f "$INSTALL_PREFIX/$file"
      info "removed $INSTALL_PREFIX/$file"
    fi
  done
  printf '\n%sDone.%s\n' "$C_GREEN" "$C_RESET"
  exit 0
fi

# ── Host detection ──────────────────────────────────────────────────────────
is_glibc_host() {
  [[ -e /lib/ld-linux-aarch64.so.1 || -e /lib64/ld-linux-aarch64.so.1 || \
     -e /usr/lib/ld-linux-aarch64.so.1 || -e /usr/lib64/ld-linux-aarch64.so.1 ]]
}

is_termux_host() {
  [[ -n "$TERMUX_HOME_VERSION" && -n "$TERMUX_HOME_PREFIX" ]]
}

host_summary() {
  local os=""
  if [[ -r /etc/os-release ]]; then
    os="$(. /etc/os-release 2>/dev/null && printf '%s' "${PRETTY_NAME:-linux}")"
  fi
  printf '%s (%s, %s)' "${os:-Linux}" "$(uname -m)" "$(uname -r)"
}

# ── Native Termux: set up a PRoot userland and continue inside it ───────────
# proot-distro 5.x prints the installed container names with --quiet; older
# releases use a bullet list.
proot_container_installed() {
  local name="$1" output
  output="$(proot-distro list --quiet 2>/dev/null || true)"
  if [[ -n "$output" ]]; then
    printf '%s\n' "$output" | grep -qx -- "$name"
    return $?
  fi

  proot-distro list 2>/dev/null | sed -n 's/^[[:space:]]*\*[[:space:]]*//p' | grep -qx -- "$name"
}

bootstrap_proot() {
  step "Native Termux detected: preparing a PRoot $PROOT_DISTRO userland"

  if ! command -v proot-distro >/dev/null 2>&1; then
    info "installing the proot-distro package"
    if ! command -v pkg >/dev/null 2>&1; then
      die "proot-distro is missing and the Termux package manager is not available.
Install it with: pkg install proot-distro"
    fi
    pkg install -y proot-distro || die "could not install proot-distro"
  fi

  if ! proot_container_installed "$PROOT_DISTRO"; then
    info "installing the $PROOT_DISTRO distribution (this downloads a few hundred megabytes)"
    proot-distro install "$PROOT_DISTRO" || die "could not install the $PROOT_DISTRO distribution"
  fi

  info "entering the $PROOT_DISTRO userland"
  # The script keeps its absolute path, and the Termux filesystem is visible
  # from inside the container, so the same path works on both sides.
  exec proot-distro login "$PROOT_DISTRO" -- bash "$SELF_ABS" "${ORIG_ARGS[@]}" --inside-proot
}

# ── Package installation ────────────────────────────────────────────────────
apt_run() {
  if [[ "$(id -u)" -eq 0 ]]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo "$@"
  else
    die "this step needs root privileges and sudo is not available"
  fi
}

detect_compiler() {
  local candidates=() candidate resolved triple
  [[ -n "${CC:-}" ]] && candidates+=("$CC")
  candidates+=(/usr/bin/gcc /usr/bin/cc /usr/bin/clang /usr/local/bin/gcc /usr/local/bin/cc)
  candidates+=(gcc cc clang)

  for candidate in "${candidates[@]}"; do
    if [[ -x "$candidate" ]]; then
      resolved="$candidate"
    else
      resolved="$(command -v "$candidate" 2>/dev/null || true)"
      [[ -n "$resolved" ]] || continue
    fi
    triple="$("$resolved" -dumpmachine 2>/dev/null || true)"
    case "$triple" in
      *android*|*bionic*) continue ;;
    esac
    printf '%s\n' "$resolved"
    return 0
  done

  return 1
}

missing_tools() {
  local missing=()
  command -v curl >/dev/null 2>&1 || missing+=("curl")
  command -v tar >/dev/null 2>&1 || missing+=("tar")
  command -v sha512sum >/dev/null 2>&1 || missing+=("coreutils")
  command -v install >/dev/null 2>&1 || missing+=("coreutils")
  detect_compiler >/dev/null 2>&1 || missing+=("gcc" "libc6-dev")
  printf '%s\n' "${missing[@]:-}" | grep -v '^$'
}

install_requirements() {
  if [[ "$SKIP_DEPS" -eq 1 ]]; then
    info "skipping the package installation (AGY_SKIP_DEPS=1)"
    return 0
  fi

  local missing
  missing="$(missing_tools | sort -u || true)"

  if [[ -z "$missing" && "$FORCE_DEPS" -eq 0 ]]; then
    info "all build requirements are present"
    return 0
  fi

  [[ -n "$missing" ]] && info "missing packages: $(printf '%s' "$missing" | tr '\n' ' ')"

  if ! command -v apt-get >/dev/null 2>&1; then
    warn "apt-get is not available; install these packages manually: $missing"
    return 0
  fi

  step "Installing the build requirements"
  apt_run apt-get update -qq || warn "apt-get update failed; using the current package lists"
  apt_run apt-get install -y --no-install-recommends \
    ca-certificates curl tar coreutils gcc libc6-dev \
    || die "could not install the build requirements"
  info "build requirements installed"
}

# Makes sure a directory exists and can be written to.  Inside a PRoot
# container the login user is usually root, but a container may also be used
# with an unprivileged account.
ensure_writable_dir() {
  local dir="$1"
  [[ -d "$dir" && -w "$dir" ]] && return 0

  if [[ ! -d "$dir" ]] && mkdir -p "$dir" 2>/dev/null; then
    return 0
  fi

  if command -v sudo >/dev/null 2>&1; then
    if [[ ! -d "$dir" ]]; then
      sudo mkdir -p "$dir" 2>/dev/null || return 1
    fi
    sudo test -w "$dir" 2>/dev/null
    return $?
  fi

  return 1
}

# ── Engine download ─────────────────────────────────────────────────────────
manifest_field() {
  local json="$1" key="$2" value=""
  if command -v jq >/dev/null 2>&1; then
    value="$(printf '%s' "$json" | jq -r --arg k "$key" '.[$k] // empty' 2>/dev/null || true)"
  fi
  if [[ -z "$value" ]]; then
    value="$(printf '%s' "$json" | sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -1)"
  fi
  printf '%s' "$value"
}

download_engine() {
  local staging="$1"
  local version="" url="" sha=""

  if [[ -n "$ENGINE_FILE" ]]; then
    step "Using the local engine archive"
    [[ -f "$ENGINE_FILE" ]] || die "engine archive not found: $ENGINE_FILE"
    cp -f "$ENGINE_FILE" "$staging/engine.tar.gz"

    # Compare against the published checksum when the manifest is reachable.
    local reference
    reference="$(curl -fsSL --retry 2 --retry-delay 2 "$MANIFEST_URL" 2>/dev/null || true)"
    if [[ -n "$reference" ]]; then
      sha="$(manifest_field "$reference" sha512)"
      version="$(manifest_field "$reference" version)"
      local actual
      actual="$(sha512sum "$staging/engine.tar.gz" | cut -d' ' -f1)"
      if [[ -n "$sha" && "$actual" != "$sha" ]]; then
        warn "the local archive does not match the published checksum"
        info "expected: $sha"
        info "actual:   $actual"
      else
        info "checksum verified"
      fi
    fi
  else
    if [[ -z "$ENGINE_URL" ]]; then
      step "Querying the official release manifest"
      local manifest
      manifest="$(curl -fsSL --retry 3 --retry-delay 2 --retry-all-errors "$MANIFEST_URL")" \
        || die "could not query $MANIFEST_URL"
      version="$(manifest_field "$manifest" version)"
      url="$(manifest_field "$manifest" url)"
      sha="$(manifest_field "$manifest" sha512)"
      [[ -n "$version" ]] || die "the manifest did not contain a version"
      [[ -n "$url" ]] || die "the manifest did not contain a download URL"
      info "latest official version: $version"
    else
      url="$ENGINE_URL"
      version="${AGY_VERSION:-}"
    fi

    step "Downloading the upstream engine"
    curl -fL --progress-bar --retry 5 --retry-delay 3 --retry-all-errors \
      --connect-timeout 15 --speed-limit 1024 --speed-time 120 \
      -o "$staging/engine.tar.gz" "$url" || die "could not download the engine from $url"
  fi

  if [[ -n "$sha" ]]; then
    step "Verifying the SHA-512 checksum"
    printf '%s  %s\n' "$sha" "$staging/engine.tar.gz" | sha512sum -c - >/dev/null \
      || die "SHA-512 verification failed; the download was corrupted"
    info "checksum verified"
  else
    warn "the manifest did not provide a checksum; the download was not verified"
  fi

  step "Extracting the engine"
  mkdir -p "$staging/engine"
  tar -xzf "$staging/engine.tar.gz" -C "$staging/engine" || die "could not extract the engine archive"
  rm -f "$staging/engine.tar.gz"

  local candidate
  for candidate in antigravity agy bin/antigravity bin/agy; do
    if [[ -f "$staging/engine/$candidate" ]]; then
      ENGINE_PATH="$staging/engine/$candidate"
      ENGINE_VERSION="$version"
      return 0
    fi
  done

  die "could not find the engine binary inside the archive"
}

# ── Build ───────────────────────────────────────────────────────────────────
build_pair() {
  local staging="$1" engine="$2" version="$3"
  local compiler

  step "Compiling the bootstrapper"
  compiler="$(detect_compiler)" || die "no glibc-targeting C compiler found"
  info "compiler: $compiler"

  "$compiler" -O2 -Wall -Wextra -DAGY_VERSION="\"$version\"" \
    -o "$staging/agy" "$SRC_DIR/agy_proot.c" || die "compiling src/agy_proot.c failed"
  chmod 0755 "$staging/agy"

  local interp
  interp="$(readelf -l "$staging/agy" 2>/dev/null \
    | sed -n 's/.*program interpreter: \(.*\)\]/\1/p' | head -1)"
  if [[ -n "$interp" && "$interp" != "/lib/ld-linux-aarch64.so.1" && \
        "$interp" != "/lib64/ld-linux-aarch64.so.1" ]]; then
    die "the bootstrapper was linked against '$interp' instead of a glibc loader"
  fi

  step "Applying the VA39 memory patches"
  "$staging/agy" __patch-va39 "$engine" "$staging/agy.va39" \
    || die "the VA39 patcher did not complete"
  chmod 0755 "$staging/agy.va39"

  step "Validating the built pair"
  "$staging/agy" --version >/dev/null 2>&1 || die "the built pair did not start"
}

# ── Install ─────────────────────────────────────────────────────────────────
install_pair() {
  local staging="$1" version="$2"
  local backup=""

  step "Installing into $INSTALL_PREFIX"
  ensure_writable_dir "$INSTALL_PREFIX" \
    || die "cannot write to $INSTALL_PREFIX.
Run the installer with root privileges inside the container, or choose a
writable directory with --prefix."

  if [[ -e "$INSTALL_PREFIX/agy" || -e "$INSTALL_PREFIX/agy.va39" || \
      -e "$INSTALL_PREFIX/.agy.version" ]]; then
    backup="$(mktemp -d "${TMPDIR:-/tmp}/agy-backup.XXXXXX")"
    local name
    for name in agy agy.va39 .agy.version; do
      if [[ -f "$INSTALL_PREFIX/$name" ]]; then
        cp -p "$INSTALL_PREFIX/$name" "$backup/$name" \
          || warn "could not back up the previous $name"
      fi
    done
    info "a previous installation was backed up"
  fi

  restore_previous() {
    local name
    if [[ -n "$backup" ]]; then
      for name in agy agy.va39 .agy.version; do
        if [[ -f "$backup/$name" ]]; then
          cp -p "$backup/$name" "$INSTALL_PREFIX/$name" 2>/dev/null || true
        fi
      done
    fi
  }

  local ok=0
  if install -m 0755 "$staging/agy" "$INSTALL_PREFIX/agy" && \
     install -m 0755 "$staging/agy.va39" "$INSTALL_PREFIX/agy.va39"; then
    printf '%s\n' "$version" >"$INSTALL_PREFIX/.agy.version"
    ok=1
  fi

  if [[ "$ok" -ne 1 ]] || ! "$INSTALL_PREFIX/agy" --version >/dev/null 2>&1; then
    restore_previous
    die "the installation did not complete; the previous installation was restored"
  fi

  [[ -n "$backup" ]] && rm -rf "$backup"
  info "installed $INSTALL_PREFIX/agy and $INSTALL_PREFIX/agy.va39"
}

print_next_steps() {
  local version="$1" out="$2"

  printf '\n%s%sAntigravity CLI installed successfully.%s\n' "$C_GREEN" "$C_BOLD" "$C_RESET"
  printf '    version:  %s\n' "$version"
  printf '    binaries: %s/agy, %s/agy.va39\n' "$out" "$out"

  case ":$PATH:" in
    *":$out:"*) ;;
    *)
      printf '\n%snote:%s %s is not in PATH for this shell. Add this to ~/.bashrc:\n\n' \
        "$C_YELLOW" "$C_RESET" "$out"
      printf "    export PATH=\"%s:\$PATH\"\n\n" "$out"
      ;;
  esac

  printf '%sNext steps%s\n' "$C_BOLD" "$C_RESET"
  printf '    %s\n' "agy                            start the interactive CLI (a browser opens for the Google login)"
  printf '    %s\n' "agy --print \"hello\"           run a single prompt without the interactive UI"
  printf '    %s\n' "agy update                     check for a newer engine release"
  printf '    %s\n' "agy remote-control start       expose the web UI through a reverse tunnel"
  printf '\n'
}

# ── Main ────────────────────────────────────────────────────────────────────
main() {
  printf '\n%s%s %s%s\n' "$C_BOLD" "$PKG_NAME" "$PKG_VERSION" "$C_RESET" >&2

  if [[ "$(uname -m)" != "$ENGINE_ARCH" ]]; then
    die "this package targets $ENGINE_ARCH hosts, but this machine reports $(uname -m).
The upstream engine is only published for arm64 Linux."
  fi

  if [[ "$INSIDE_PROOT" -eq 0 ]] && ! is_glibc_host && is_termux_host; then
    bootstrap_proot
  fi

  if ! is_glibc_host; then
    die "no glibc dynamic loader was found.
This installer needs a Debian or Ubuntu userland.
Started from native Termux it can set one up for you."
  fi

  info "host: $(host_summary)"

  install_requirements

  STAGING_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agy-build.XXXXXX")" || die "cannot create a temporary directory"
  if [[ "$KEEP_STAGING" -eq 0 ]]; then
    trap '[[ -n "$STAGING_DIR" ]] && rm -rf "$STAGING_DIR"' EXIT
  else
    info "staging directory: $STAGING_DIR"
  fi

  download_engine "$STAGING_DIR"
  [[ -n "$ENGINE_VERSION" ]] || ENGINE_VERSION="unknown"

  build_pair "$STAGING_DIR" "$ENGINE_PATH" "$ENGINE_VERSION"
  install_pair "$STAGING_DIR" "$ENGINE_VERSION"
  print_next_steps "$ENGINE_VERSION" "$INSTALL_PREFIX"

  if [[ "$LAUNCH" -eq 1 && "${AGY_INSTALL_SKIP_LAUNCH:-0}" != "1" ]]; then
    step "Starting the CLI"
    exec "$INSTALL_PREFIX/agy"
  fi
}

main "$@"
