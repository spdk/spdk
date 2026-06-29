#!/usr/bin/env bash
# SPDK Hot Upgrade Rollback Script
# Reverses a hot upgrade if takeover fails or decision is made to revert.
# Strategy:
#   1. If Secondary is running: stop it
#   2. If Primary is still suspended (in pause()): send SIGUSR1 to resume
#   3. If Primary has exited: restart it from the OLD binary
#   4. Clean up temporary files
#
# Usage: spdk_hot_upgrade_rollback.sh [options]
#   -p, --primary-pid PID          Primary process PID (for SIGUSR1 wake)
#   --primary-socket PATH          Primary RPC socket (default: /var/tmp/spdk_hot_upgrade.sock)
#   -s, --secondary-socket PATH    Secondary RPC socket (default: /var/tmp/spdk_secondary.sock)
#   --secondary-pid PID            Secondary process PID (to kill)
#   --old-binary PATH              Old binary to restart Primary (if PID is dead)
#   --primary-args ARGS            Args for restarting Primary
#   -f, --force                    Force rollback even if state checks fail
#   -h, --help                     Show this help

set -euo pipefail

# ======================== Defaults ========================
PRIMARY_PID=""
SECONDARY_PID=""
PRIMARY_SOCK="/var/tmp/spdk_hot_upgrade.sock"
SECONDARY_SOCK="/var/tmp/spdk_secondary.sock"
OLD_BINARY=""
PRIMARY_ARGS=""
FORCE=0
TIMEOUT=10

# ======================== Argument Parsing ========================
usage() {
    cat <<EOF
spdk_hot_upgrade_rollback.sh - Rollback SPDK hot upgrade

Usage: $0 [options]

Options:
  -p, --primary-pid PID          Primary process PID (to send SIGUSR1 for resume)
  --primary-socket PATH          Primary RPC socket (default: $PRIMARY_SOCK)
  -s, --secondary-socket PATH    Secondary RPC socket (default: $SECONDARY_SOCK)
  --secondary-pid PID            Secondary process PID (to kill)
  --old-binary PATH              Old binary to restart Primary (default: same as new)
  --primary-args ARGS            Extra args for Primary restart
  -f, --force                    Force rollback even if state checks fail
  -h, --help                     Show this help

Exit codes:
  0  Rollback successful
  1  Rollback failed
  2  Rollback partially successful (manual intervention recommended)
EOF
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--primary-pid)       PRIMARY_PID="$2"; shift 2;;
        --primary-socket)       PRIMARY_SOCK="$2"; shift 2;;
        -s|--secondary-socket)  SECONDARY_SOCK="$2"; shift 2;;
        --secondary-pid)        SECONDARY_PID="$2"; shift 2;;
        --old-binary)           OLD_BINARY="$2"; shift 2;;
        --primary-args)         PRIMARY_ARGS="$2"; shift 2;;
        -f|--force)             FORCE=1; shift;;
        -h|--help)              usage 0;;
        *)                      echo "Unknown option: $1"; usage 1;;
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

# ======================== Rollback Functions ========================
rollback_stop_secondary() {
    echo "[ROLLBACK] Stop Secondary process..."

    if [[ -n "$SECONDARY_PID" ]]; then
        if kill -0 "$SECONDARY_PID" 2>/dev/null; then
            echo -n "  Killing Secondary PID=$SECONDARY_PID... "
            kill -TERM "$SECONDARY_PID" 2>/dev/null || true
            sleep 1
            if kill -0 "$SECONDARY_PID" 2>/dev/null; then
                kill -KILL "$SECONDARY_PID" 2>/dev/null || true
                echo "KILLED"
            else
                echo "TERMINATED"
            fi
            return 0
        else
            echo "  Secondary PID=$SECONDARY_PID already dead"
            return 0
        fi
    fi

    # Try to stop Secondary via RPC
    if [[ -S "$SECONDARY_SOCK" ]]; then
        if _rpc_call "$SECONDARY_SOCK" "spdk_kill_instance" 3 >/dev/null 2>&1; then
            echo "  Sent kill_instance to Secondary via RPC"
            sleep 2
        fi
    fi

    echo "  Secondary stop complete"
    return 0
}

rollback_resume_primary() {
    echo "[ROLLBACK] Resume Primary process..."

    if [[ -z "$PRIMARY_PID" ]]; then
        echo "  No Primary PID provided, trying SIGUSR1 discovery..."
        # Try to find the PID from the socket owner
        if [[ -S "$PRIMARY_SOCK" ]]; then
            PRIMARY_PID=$(lsof -t "$PRIMARY_SOCK" 2>/dev/null | head -1 || echo "")
        fi
        if [[ -z "$PRIMARY_PID" ]]; then
            echo "  Could not determine Primary PID. Trying any spdk_tgt process..."
            PRIMARY_PID=$(pgrep -f spdk_tgt 2>/dev/null | head -1 || echo "")
        fi
    fi

    if [[ -z "$PRIMARY_PID" ]]; then
        echo "  No Primary process found"
        return 1
    fi

    if ! kill -0 "$PRIMARY_PID" 2>/dev/null; then
        echo "  Primary PID=$PRIMARY_PID is dead, will restart"
        return 2
    fi

    echo "  Sending SIGUSR1 to Primary PID=$PRIMARY_PID..."
    if kill -USR1 "$PRIMARY_PID" 2>/dev/null; then
        echo "  SIGUSR1 sent. Waiting for Primary to resume..."

        # Wait for Primary to respond to RPC
        local deadline
        deadline=$(($(date +%s) + TIMEOUT))
        while true; do
            if _rpc_call "$PRIMARY_SOCK" "hot_upgrade_status" 3 >/dev/null 2>&1; then
                echo "  Primary resumed successfully!"
                return 0
            fi
            if [[ $(date +%s) -gt $deadline ]]; then
                echo "  TIMEOUT: Primary did not resume via SIGUSR1"
                break
            fi
            sleep 1
        done
    fi

    # Fallback: try RPC-based resume
    if _rpc_call "$PRIMARY_SOCK" "primary_resume" 3 >/dev/null 2>&1; then
        echo "  Primary resumed via RPC"
        return 0
    fi

    echo "  WARNING: Could not confirm Primary resume, but SIGUSR1 was sent"
    return 0
}

rollback_restart_primary() {
    if [[ -z "$OLD_BINARY" ]]; then
        echo "[ROLLBACK] No old binary specified, skipping Primary restart"
        echo "  WARNING: Primary is dead and cannot be restarted automatically"
        return 1
    fi

    echo "[ROLLBACK] Restart Primary process..."
    echo "  Binary: $OLD_BINARY"
    echo "  Socket: $PRIMARY_SOCK"

    rm -f "$PRIMARY_SOCK"

    $OLD_BINARY \
        -r "$PRIMARY_SOCK" \
        $PRIMARY_ARGS \
        > /var/tmp/spdk_primary_restart.log 2>&1 &

    local new_pid=$!
    echo "  New Primary PID: $new_pid"

    # Wait for it to become reachable
    local deadline
    deadline=$(($(date +%s) + TIMEOUT))
    while true; do
        if _rpc_call "$PRIMARY_SOCK" "spdk_get_version" 3 >/dev/null 2>&1; then
            echo "  Primary restarted successfully!"
            echo "  ROLLBACK_PRIMARY_PID=$new_pid"
            return 0
        fi
        if [[ $(date +%s) -gt $deadline ]]; then
            echo "  TIMEOUT: Primary restart did not complete"
            return 1
        fi
        sleep 1
    done
}

rollback_cleanup_files() {
    echo "[ROLLBACK] Cleanup temporary files..."

    if [[ -e "/var/tmp/spdk_hu_ipc.sock" ]]; then
        rm -f "/var/tmp/spdk_hu_ipc.sock"
        echo "  Removed IPC socket"
    fi

    if [[ -e "/var/tmp/spdk_hot_upgrade_state" ]]; then
        rm -f "/var/tmp/spdk_hot_upgrade_state"
        echo "  Removed state file"
    fi

    if [[ -e "$SECONDARY_SOCK" ]]; then
        rm -f "$SECONDARY_SOCK"
        echo "  Removed Secondary socket"
    fi
}

# ======================== Main ========================
main() {
    echo "========================================"
    echo "  SPDK Hot Upgrade Rollback"
    echo "========================================"
    echo "Primary PID:      ${PRIMARY_PID:-auto-detect}"
    echo "Primary socket:   $PRIMARY_SOCK"
    echo "Secondary socket: $SECONDARY_SOCK"
    echo ""

    local ret_resume=0
    local ret_restart=0

    # Step 1: Stop Secondary
    rollback_stop_secondary || true

    # Step 2: Resume Primary (via SIGUSR1 if still suspended, or RPC)
    rollback_resume_primary
    ret_resume=$?

    # Step 3: If Primary is dead, restart it
    if [[ $ret_resume -eq 2 ]] || [[ $ret_resume -eq 1 ]]; then
        rollback_restart_primary
        ret_restart=$?
    fi

    # Step 4: Clean up
    rollback_cleanup_files

    echo ""
    echo "========================================"
    if [[ $ret_resume -eq 0 ]]; then
        echo "  Rollback Successful!"
        echo "  Primary is running at: $PRIMARY_SOCK"
        exit 0
    elif [[ $ret_restart -eq 0 ]]; then
        echo "  Rollback Successful (restarted)!"
        echo "  Primary is running at: $PRIMARY_SOCK"
        exit 0
    else
        echo "  Rollback PARTIALLY COMPLETE"
        echo "  Manual intervention may be required"
        exit 2
    fi
}

main