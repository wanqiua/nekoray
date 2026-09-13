# Retained for callers of the original build helper.
# Core versions now come directly from the module files; this script never
# checks out or overwrites a developer's Xray/sing-box fork.
CORE_SOURCE_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export XRAY_MODULE_VERSION="$(cd "$CORE_SOURCE_ROOT/go/cmd/nekoray_core" && go list -m -f '{{.Version}}' github.com/xtls/xray-core)"
export SING_BOX_MODULE_VERSION="$(cd "$CORE_SOURCE_ROOT/go/cmd/nekobox_core" && go list -m -f '{{.Version}}' github.com/sagernet/sing-box)"
