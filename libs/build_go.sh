#!/usr/bin/env bash
set -euo pipefail

SRC_ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$SRC_ROOT"
export GOOS="${GOOS:-$(go env GOOS)}"
export GOARCH="${GOARCH:-$(go env GOARCH)}"
export CGO_ENABLED=0

case "$GOOS/$GOARCH" in
  windows/amd64) target=windows64 ;;
  linux/amd64) target=linux64 ;;
  windows/arm64|linux/arm64|darwin/amd64|darwin/arm64) target="$GOOS-$GOARCH" ;;
  *) printf 'Unsupported target: %s/%s\n' "$GOOS" "$GOARCH" >&2; exit 1 ;;
esac

DEST="$SRC_ROOT/deployment/$target"
mkdir -p "$DEST"
extension=""
if [ "$GOOS" = windows ]; then extension=".exe"; fi
version="NekoRay_BY_WanQiu-$(tr -d '\r\n' < nekoray_version.txt)"
tags="$(tr -d '\r\n' < libs/core_build_tags.txt)"
ldflags="-w -s -X github.com/matsuridayo/libneko/neko_common.Version_neko=$version"

for core in nekoray_core nekobox_core; do
  (
    cd "$SRC_ROOT/go/cmd/$core"
    if [ "$core" = nekoray_core ]; then
      export GOTOOLCHAIN=go1.21.13
    else
      export GOTOOLCHAIN=go1.26.4
    fi
    flags=()
    if [ "$core" = nekobox_core ]; then flags=(-tags "$tags"); fi
    if [ "${NKR_TEST_CORES:-0}" = 1 ]; then
      go test "${flags[@]}" -count=1 -timeout 90s ./...
    fi
    go build -mod=readonly "${flags[@]}" -trimpath -ldflags "$ldflags" -o "$DEST/$core$extension" .
  )
done

# Linux portable builds still need the launcher to set their Qt environment.
# The GUI's legacy download/update RPC is disabled in this fork.
if [ "$GOOS" = linux ] || [ "${NKR_BUILD_UPDATER:-0}" = 1 ]; then
  (
    cd "$SRC_ROOT/go/cmd/updater"
    name="updater$extension"
    if [ "$GOOS" = linux ]; then name=launcher; fi
    go build -trimpath -ldflags "-w -s" -o "$DEST/$name" .
  )
fi
printf 'Cores built in %s\n' "$DEST"
