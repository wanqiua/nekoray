#!/usr/bin/env bash
set -euo pipefail

SRC_ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
# Dependencies are pinned in go.mod/go.sum. No sibling fork checkouts are needed.
for module in grpc_server cmd/nekoray_core cmd/nekobox_core; do
  (
    cd "$SRC_ROOT/go/$module"
    go mod download
  )
done
