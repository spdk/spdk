#!/usr/bin/env bash
# SPDK Hot Upgrade Commit Script
# Called after successful takeover to finalize the upgrade:
#   1. Signal Primary process to exit cleanly (via SIGUSR1 or SIGTERM)
#   2. Verify Secondary is stable
#   3. Clean up temporary files (IPC socket, state file)
#   4. Optionally rename Secondary RPC socket to Primary location
#
# Usage: spdk_hot_upgrade_commit.sh [options]
#   -p, --primary-pid PID         Primary process PID
#   -s, --secondary-socket PATH   Secondary RPC socket (default: /var/tmp/spdk_secondary.sock)
#   -t, --target-socket PATH      Target RPC socket after rename (default: /var/tmp/spdk_hot_upgrade.sock)
#   --no-rename                   Keep Secondary RPC socket path as-is
#   --force                       Force commit even if state is not COMPLETE
#   -h, --help                    Show this help

set -euo pipefail

# ======================== Defaults ========================
PRIMARY_PID=""
SECONDARY_PID=""
SECONDARY_SOCK="/var/tmp/spdk_secondary.sock"
TARGET_SOCK="/var/tmp/spdk_hot_upgrade.sock"
NO_RENAME=0
FORCE=0
TIMEOUT=10

# ======================== Argument Parsing ========================
usage() {
    cat <<EOF
spdk_hot_upgrade_commit.sh - Finalize SPDK hot upgrade

Usage: $0 [options]

Options:
  -p, --primary-pid PID         Primary process PID (to terminate)
  --secondary-pid PID           Secondary process PID
  -s, --secondary-socket PATH   Secondary RPC socket (default: $SECONDARY_SOCK)
  -t, --target-socket PATH      Target RPC socket after commit (default: $TARGET_SOCK)
  --no-rename                   Keep Secondary RPC socket path as-is
  --force                       Force commit even if health check fails
  -h, --help                    Show this help

Exit codes:
  0  Commit successful
  1  Commit failed
EOF
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--primary-pid)    PRIMARY_PID="$2"; shift 2;;
        --secondary-pid)     SECONDARY_PID="$2"; shift 2;;
        -s|--secondary-socket) SECONDARY_SOCK="$2"; shift 2;;
        -t|--target-socket)  TARGET_SOCK="$2"; shift 2;;
        --no-rename)         NO_RENAME=1; shift;;
        --force)             FORCE=1; shift;;
        -h|--help)           usage 0;;
        *)                   echo "Unknown option: $1"; usage 1;;
    esac
done

# ======================== RPC Helper ========================
_rpc_call() {
    local sock="$1"
    local method="$2"
    local timeout="${3:-5}"
    python3 -c "
import socket,json
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
s.settimeout($timeout)
try:
    s.connect('$sock')
    s.sendall(json.dumps({'jsonrpc':'2.0','method':'$method','id':1}).encode())
    resp=json.loads(s.recv(4096))
    if 'result' in resp:
        print(json.dumps(resp['result']))
    elif 'error' in resp:
        print('ERROR:'+json.dumps(resp['error']))
    s.close()
except Exception as e:
    print('ERROR:'+str(e))
" 2>/dev/null
}

# ======================== Commit Functions ========================
commit_verify_secondary() {
    echo -n "[COMMIT] Verify Secondary health... "

    if ! _rpc_call "$SECONDARY_SOCK" "spdk_get_version" 3 >/dev/null 2>&1; then
        if [[ $FORCE -eq 1 ]]; then
            echo "WARNING (forcing commit despite failed health check)"
        else
            echo "FAILED"
            echo "  Secondary RPC not responding. Use --force to commit anyway."
            return 1
        fi
    else
        echo "OK"
    fi

    local state
    state=$(_rpc_call "$SECONDARY_SOCK" "hot_upgrade_status" 3 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin)['state'])" 2>/dev/null || echo "UNKNOWN")
    echo "  Secondary state: $state"

    return 0
}

commit_terminate_primary() {
    echo -n "[COMMIT] Terminate Primary process... "

    if [[ -z "$PRIMARY_PID" ]]; then
        echo "SKIPPED (no PID provided)"
        return 0
    fi

    if ! kill -0 "$PRIMARY_PID" 2>/dev/null; then
        echo "ALREADY_DEAD"
        return 0
    fi

    # Send SIGTERM for graceful shutdown
    if kill -TERM "$PRIMARY_PID" 2>/dev/null; then
        # Wait for process to exit
        local deadline
        deadline=$(($(date +%s) + TIMEOUT))
        while kill -0 "$PRIMARY_PID" 2>/dev/null; do
            if [[ $(date +%s) -gt $deadline ]]; then
                echo "TIMEOUT (sending SIGKILL)"
                kill -KILL "$PRIMARY_PID" 2>/dev/null || true
                return 0
            fi
            sleep 0.5
        done
        echo "DONE"
    else
        echo "FAILED (could not send signal)"
        return 1
    fi

    return 0
}

commit_cleanup_files() {
    echo "[COMMIT] Cleanup temporary files..."

    # Remove IPC socket
    if [[ -e "/var/tmp/spdk_hu_ipc.sock" ]]; then
        rm -f "/var/tmp/spdk_hu_ipc.sock"
        echo "  Removed IPC socket"
    fi

    # Remove state file
    if [[ -e "/var/tmp/spdk_hot_upgrade_state" ]]; then
        rm -f "/var/tmp/spdk_hot_upgrade_state"
        echo "  Removed state file"
    fi

    # Remove primary RPC socket if it still exists
    if [[ -e "$TARGET_SOCK" ]] && [[ "$TARGET_SOCK" != "$SECONDARY_SOCK" ]]; then
        rm -f "$TARGET_SOCK"
        echo "  Removed stale Primary socket: $TARGET_SOCK"
    fi
}

commit_rename_socket() {
    if [[ $NO_RENAME -eq 1 ]]; then
        echo "[COMMIT] Keeping Secondary socket at: $SECONDARY_SOCK"
        return 0
    fi

    if [[ "$SECONDARY_SOCK" == "$TARGET_SOCK" ]]; then
        echo "[COMMIT] Socket already at target: $TARGET_SOCK"
        return 0
    fi

    echo -n "[COMMIT] Move Secondary socket to $TARGET_SOCK... "

    if [[ ! -e "$SECONDARY_SOCK" ]]; then
        echo "FAILED (secondary socket does not exist)"
        return 1
    fi

    if mv "$SECONDARY_SOCK" "$TARGET_SOCK" 2>/dev/null; then
        echo "OK"
    else
        echo "FAILED (permission denied or target in use)"
        return 1
    fi

    echo "  NEW_PRIMARY_SOCK=$TARGET_SOCK"
    return 0
}

# ======================== Main ========================
main() {
    echo "========================================"
    echo "  SPDK Hot Upgrade Commit"
    echo "========================================"
    echo "Secondary socket: $SECONDARY_SOCK"
    echo "Target socket:    $TARGET_SOCK"
    echo "Primary PID:      ${PRIMARY_PID:-N/A}"
    echo ""

    commit_verify_secondary   || exit 1
    commit_terminate_primary  || exit 1
    commit_cleanup_files      || exit 1
    commit_rename_socket      || exit 1

    echo ""
    echo "========================================"
    echo "  Commit Complete!"
    if [[ $NO_RENAME -eq 0 ]]; then
        echo "  New Primary RPC: $TARGET_SOCK"
    else
        echo "  Secondary RPC:   $SECONDARY_SOCK"
    fi
    echo "========================================"

    exit 0
}

main