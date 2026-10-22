#!/usr/bin/env bash
#
# Removes a previous Antigravity CLI installation.
#
# Usage:
#   bash uninstall.sh [--prefix DIR]
#
set -Eeuo pipefail

INSTALL_PREFIX="${AGY_PREFIX:-/usr/local/bin}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)   [[ $# -ge 2 ]] || { echo "error: --prefix requires a directory" >&2; exit 1; }
                INSTALL_PREFIX="$2"; shift 2 ;;
    --prefix=*) INSTALL_PREFIX="${1#*=}"; shift ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--prefix DIR]"
      echo
      echo "Removes agy, agy.va39 and .agy.version from DIR (default: /usr/local/bin)."
      echo "The stored Google credentials are not touched."
      exit 0 ;;
    *) echo "error: unknown option: $1" >&2; exit 1 ;;
  esac
done

removed=0
for file in agy agy.va39 .agy.version; do
  if [[ -e "$INSTALL_PREFIX/$file" ]]; then
    rm -f "$INSTALL_PREFIX/$file"
    echo "removed $INSTALL_PREFIX/$file"
    removed=1
  fi
done

if [[ "$removed" -eq 0 ]]; then
  echo "nothing to remove in $INSTALL_PREFIX"
else
  echo "done"
fi
