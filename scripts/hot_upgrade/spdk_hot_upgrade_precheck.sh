#!/usr/bin/env bash
# SPDK Hot Upgrade Precheck Script
# Verifies that the system is ready for a hot upgrade before proceeding.
#
# Checks:
#   1. Primary process is running and reachable
#   2. Hot upgrade state is IDLE
#   3. New binary exists, is executable, and is a valid SPDK binary
#   4. DPDK/SPDK version compatibility (binary vs Primary)
#   5. Primary /proc/<pid>/cmdline readable + --shm-id present (parallel flow dependency)
#   6. Shared state file is writable
#   7. Secondary RPC socket not in use
#   8. IPC socket free
#
# Usage: spdk_hot_upgrade_precheck.sh [options]
#   -p, --primary-socket PATH   Primary RPC socket path (default: /var/tmp/spdk_hot_upgrade.sock)
#   -n, --new-binary PATH       Path to new SPDK binary (default: ./build/bin/spdk_tgt)
#   -s, --secondary-socket PATH Secondary RPC socket path (default: /var/tmp/spdk_secondary.sock)
#   -h, --help                  Show this help

set -euo pipefail
shopt -s nullglob

# ======================== Defaults ========================
PRIMARY_SOCK="/var/tmp/spdk_hot_upgrade.sock"
NEW_BINARY="./build/bin/spdk_tgt"
SECONDARY_SOCK="/var/tmp/spdk_secondary.sock"
VERBOSE=0

# ======================== Argument Parsing ========================
usage() {
    cat <<EOF
spdk_hot_upgrade_precheck.sh - Verify hot upgrade prerequisites

Usage: $0 [options]

Options:
  -p, --primary-socket PATH   Primary RPC socket path (default: $PRIMARY_SOCK)
  -n, --new-binary PATH       Path to new SPDK binary (default: $NEW_BINARY)
  -s, --secondary-socket PATH Secondary RPC socket path (default: $SECONDARY_SOCK)
  -v, --verbose               Verbose output
  -h, --help                  Show this help

Exit codes:
  0  All checks passed
  1  Hot upgrade not possible (correctable)
  2  Hot upgrade not possible (fatal)
EOF
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--primary-socket)  PRIMARY_SOCK="$2"; shift 2;;
        -n|--new-binary)      NEW_BINARY="$2"; shift 2;;
        -s|--secondary-socket) SECONDARY_SOCK="$2"; shift 2;;
        -v|--verbose)          VERBOSE=1; shift;;
        -h|--help)             usage 0;;
        *)                     echo "Unknown option: $1"; usage 1;;
    esac
done

# ======================== RPC Helper ========================
_rpc_call() {
    local sock="$1"
    local method="$2"
    local timeout=${3:-3}
    python3 -c "
import socket,json,sys
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
s.settimeout($timeout)
try:
    s.connect('$sock')
    s.sendall(json.dumps({'jsonrpc':'2.0','method':'$method','id':1}).encode())
    resp=json.loads(s.recv(4096))
    if 'result' in resp:
        print(json.dumps(resp['result']))
    elif 'error' in resp:
        print('ERROR: '+json.dumps(resp['error']),file=sys.stderr)
        sys.exit(1)
    s.close()
except Exception as e:
    print('CONNECTION_ERROR: '+str(e),file=sys.stderr)
    sys.exit(2)
"
}

# ======================== Check Functions ========================
check_primary_running() {
    echo -n "[CHECK] Primary RPC socket reachable... "
    local result
    if result=$(_rpc_call "$PRIMARY_SOCK" "hot_upgrade_status" 3 2>/dev/null); then
        echo "OK (state=$(echo "$result" | python3 -c "import sys,json;print(json.load(sys.stdin)['state'])" 2>/dev/null || echo "unknown"))"
        return 0
    else
        echo "FAILED"
        echo "  Primary process not reachable at $PRIMARY_SOCK"
        return 1
    fi
}

check_state_idle() {
    echo -n "[CHECK] Hot upgrade state is IDLE... "
    local state
    state=$(_rpc_call "$PRIMARY_SOCK" "hot_upgrade_status" 3 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin)['state'])" 2>/dev/null || echo "UNKNOWN")
    if [[ "$state" == "IDLE" ]]; then
        echo "OK"
        return 0
    else
        echo "FAILED (current state: $state)"
        echo "  Cannot start hot upgrade unless state is IDLE."
        echo "  If in FAILED state, restart Primary first."
        return 1
    fi
}

check_state_file_writable() {
    echo -n "[CHECK] Shared state file writable... "
    local state_file="/var/tmp/spdk_hot_upgrade_state"
    if touch "$state_file" 2>/dev/null; then
        rm -f "$state_file"
        echo "OK"
        return 0
    else
        echo "WARNING (may be recreated by primary) "
        return 0
    fi
}

check_artifact() {
    echo -n "[CHECK] New binary exists and is executable... "
    if [[ ! -e "$NEW_BINARY" ]]; then
        echo "FAILED"
        echo "  Binary not found: $NEW_BINARY"
        return 1
    fi
    if [[ ! -x "$NEW_BINARY" ]]; then
        echo "FAILED"
        echo "  Binary not executable: $NEW_BINARY"
        return 1
    fi
    echo "OK ($NEW_BINARY)"

    echo -n "[CHECK] Binary is valid SPDK executable... "
    local ver_out
    ver_out=$("$NEW_BINARY" --version 2>&1 | head -1 || true)
    if [[ -z "$ver_out" ]]; then
        echo "FAILED"
        echo "  Binary does not respond to --version"
        return 1
    fi
    if [[ "$ver_out" != *"SPDK"* ]]; then
        echo "FAILED"
        echo "  --version output does not contain 'SPDK': $ver_out"
        return 1
    fi
    echo "OK ($ver_out)"
    return 0
}

check_dpdk_version() {
    echo "[CHECK] DPDK/SPDK version compatibility... "

    # Get Primary version via RPC
    local primary_ver
    primary_ver=$(_rpc_call "$PRIMARY_SOCK" "spdk_get_version" 3 2>/dev/null \
        | python3 -c "import sys,json;print(json.load(sys.stdin).get('version','unknown'))" 2>/dev/null \
        || echo "unknown")

    # Get new binary version
    local binary_ver
    binary_ver=$("$NEW_BINARY" --version 2>/dev/null | head -1 | tr -d '\n' || echo "unknown")

    echo "  Primary version:    $primary_ver"
    echo "  New binary version: $binary_ver"

    if [[ "$primary_ver" == "unknown" || "$binary_ver" == "unknown" ]]; then
        echo "  WARNING: Could not determine versions for comparison"
        return 0
    fi

    if [[ "$primary_ver" == "$binary_ver" ]]; then
        echo "  WARNING: Same version — this is a re-deploy, not an upgrade"
        return 0
    fi

    # Extract major version number (first digit group after 'v')
    local primary_major binary_major
    primary_major=$(echo "$primary_ver" | grep -oP 'v\K[0-9]+' | head -1 || echo "")
    binary_major=$(echo "$binary_ver" | grep -oP 'v\K[0-9]+' | head -1 || echo "")

    if [[ -n "$primary_major" && -n "$binary_major" && "$primary_major" != "$binary_major" ]]; then
        echo "  FAILED: Major version mismatch ($primary_major vs $binary_major)"
        echo "  Cross-major hot upgrade is not supported"
        return 1
    fi

    echo "  OK (compatible versions)"
    return 0
}

check_primary_cmdline() {
    echo "[CHECK] Primary /proc/<pid>/cmdline readability... "

    # Find Primary PID from socket
    local primary_pid
    primary_pid=$(lsof -t "$PRIMARY_SOCK" 2>/dev/null | head -1 || true)
    if [[ -z "$primary_pid" ]]; then
        primary_pid=$(pgrep -f "spdk_tgt.*$(basename "$PRIMARY_SOCK")" 2>/dev/null | head -1 || true)
    fi
    if [[ -z "$primary_pid" ]]; then
        echo "  FAILED: Cannot find Primary PID for socket $PRIMARY_SOCK"
        return 1
    fi
    echo "  Primary PID: $primary_pid"

    # Read cmdline (NUL-separated)
    local cmdline
    cmdline=$(tr '\0' '\n' < "/proc/$primary_pid/cmdline" 2>/dev/null || true)
    if [[ -z "$cmdline" ]]; then
        echo "  FAILED: Cannot read /proc/$primary_pid/cmdline"
        echo "  (permission denied? running in container?)"
        return 1
    fi
    echo "  cmdline readable: OK"

    # Check --shm-id (REQUIRED for parallel flow)
    local shm_id
    shm_id=$(echo "$cmdline" | grep -A1 '^\(--shm-id\|-i\)$' | tail -1 || true)
    if [[ -z "$shm_id" ]]; then
        shm_id=$(echo "$cmdline" | grep -oP '^\-\-shm-id=\K\d+' || true)
    fi
    if [[ -z "$shm_id" ]]; then
        echo "  FAILED: --shm-id not found in Primary cmdline"
        echo "  Parallel hot upgrade requires Primary started with --shm-id <N>"
        return 1
    fi
    echo "  --shm-id: $shm_id (OK)"

    # Check --base-virtaddr (RECOMMENDED for pointer inheritance)
    local base_virtaddr
    base_virtaddr=$(echo "$cmdline" | grep -A1 '^--base-virtaddr$' | tail -1 || true)
    if [[ -z "$base_virtaddr" ]]; then
        base_virtaddr=$(echo "$cmdline" | grep -oP '^\-\-base-virtaddr=\K0x[0-9a-fA-F]+' || true)
    fi
    if [[ -z "$base_virtaddr" ]]; then
        echo "  WARNING: --base-virtaddr not set (pointer inheritance may fail)"
    else
        echo "  --base-virtaddr: $base_virtaddr (OK)"
    fi

    return 0
}

check_secondary_socket_free() {
    echo -n "[CHECK] Secondary RPC socket not in use... "
    if [[ -e "$SECONDARY_SOCK" ]]; then
        # Check if someone is listening
        if _rpc_call "$SECONDARY_SOCK" "spdk_get_version" 1 >/dev/null 2>&1; then
            echo "FAILED (process already listening on $SECONDARY_SOCK)"
            return 1
        else
            rm -f "$SECONDARY_SOCK"
            echo "OK (removed stale socket)"
        fi
    else
        echo "OK"
    fi
    return 0
}

check_ipc_socket_free() {
    echo -n "[CHECK] IPC socket free... "
    local ipc_sock="/var/tmp/spdk_hu_ipc.sock"
    if [[ -e "$ipc_sock" ]]; then
        rm -f "$ipc_sock"
        echo "OK (removed stale)"
    else
        echo "OK"
    fi
    return 0
}

# ======================== Main ========================
main() {
    local errors=0
    local warnings=0

    echo "=== SPDK Hot Upgrade Precheck ==="
    echo "Primary socket:  $PRIMARY_SOCK"
    echo "Secondary socket: $SECONDARY_SOCK"
    echo "New binary:      $NEW_BINARY"
    echo ""

    check_primary_running       || ((errors++))
    check_state_idle            || ((errors++))
    check_artifact              || ((errors++))
    check_dpdk_version          || ((errors++))
    check_primary_cmdline       || ((errors++))
    check_state_file_writable   || ((warnings++))
    check_secondary_socket_free || ((errors++))
    check_ipc_socket_free       || ((warnings++))

    echo ""
    echo "=== Precheck Summary ==="
    echo "Errors:   $errors"
    echo "Warnings: $warnings"

    if [[ $errors -gt 0 ]]; then
        echo "RESULT: FAILED - $errors error(s) found"
        exit 1
    elif [[ $warnings -gt 0 ]]; then
        echo "RESULT: PASSED with $warnings warning(s)"
        exit 0
    else
        echo "RESULT: PASSED - All checks passed"
        exit 0
    fi
}

main